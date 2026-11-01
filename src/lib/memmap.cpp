#include <lib/memmap.h>
#include <lib/mutex.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <nfsu_layout.h>
#include <psp2/kernel/sysmem.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace win32
{

// Guest heap, thread stacks and shim buffers: the free user memory at startup less s_reserveMemory (graphics
// heaps and pools, host thread stacks, system libraries allocated later), between s_minMemory and s_maxMemory.
static const x86::reg32 s_minMemory = 96*1024*1024;
static const x86::reg32 s_maxMemory = 192*1024*1024;
static const x86::reg32 s_reserveMemory = 64*1024*1024;
// End of the highest image section (the generated layout), page-aligned.
static const x86::reg32 s_sectionLimit = (nfsu::layout::imageEnd + 4095) & ~4095u;
// One byte per 4 KB block of the simulated memory, page-aligned.
static x86::reg32 blockMapSize(x86::reg32 memory)
{
    return (memory / 4096 + 4095) & ~4095u;
}
// Set by allocArena. Layout: sections, block map, simulated memory.
static x86::reg32 s_simulatedMemory = 0;
static x86::reg32 s_blockMapSize = 0;
static x86::reg32 s_arenaSize = 0;
static SceKernelFreeMemorySizeInfo s_freeAtStart;

x86::reg8* MemMap::s_blocks = nullptr;
x86::reg32 MemMap::s_blockCount = 0;
x86::reg32 MemMap::s_blockSize = 0;
x86::reg32 MemMap::s_sectionSize = 0;
x86::reg32 MemMap::s_addressOffset = 0;
x86::reg32 MemMap::s_lowFree = 0;
x86::reg32 MemMap::s_highFree = 0;
MemMap::Reclaim MemMap::s_reclaim = nullptr;
Mutex*     MemMap::s_lock = nullptr;
std::vector<MemMap*> MemMap::s_memMaps;

static SceUID s_arenaBlock = -1;

// The guest address space is one zeroed memblock; pages are always accessible.
// Runs before main, so failure is reported by MemMap::init once logging is up.
static SceUID allocBlock(x86::reg32 memory)
{
    s_simulatedMemory = memory;
    s_blockMapSize = blockMapSize(memory);
    s_arenaSize = s_sectionLimit + s_blockMapSize + memory;
    return sceKernelAllocMemBlock("guest", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW, s_arenaSize, nullptr);
}

static x86::reg8* allocArena()
{
    x86::reg32 memory = s_minMemory;
    memset(&s_freeAtStart, 0, sizeof(s_freeAtStart));
    s_freeAtStart.size = sizeof(s_freeAtStart);
    if (sceKernelGetFreeMemorySize(&s_freeAtStart) >= 0)
    {
        const x86::reg32 overhead = s_sectionLimit + blockMapSize(s_maxMemory) + s_reserveMemory;
        const x86::reg32 free = x86::reg32(std::max(0, s_freeAtStart.size_user));
        if (free > overhead + s_minMemory)
            memory = std::min(s_maxMemory, (free - overhead) & ~(1024u * 1024 - 1));
    }
    s_arenaBlock = allocBlock(memory);
    if (s_arenaBlock < 0 && memory > s_minMemory)
        s_arenaBlock = allocBlock(s_minMemory);
    if (s_arenaBlock < 0)
        return nullptr;
    void* base = nullptr;
    sceKernelGetMemBlockBase(s_arenaBlock, &base);
    memset(base, 0, s_arenaSize);
    return static_cast<x86::reg8*>(base);
}

static void freeArena()
{
    if (s_arenaBlock >= 0)
    {
        sceKernelFreeMemBlock(s_arenaBlock);
        s_arenaBlock = -1;
    }
}

x86::reg8* const g_guestBase = allocArena();

// One bit per block of the simulated memory, set while the block is free; kept with s_blocks under s_lock.
static const x86::reg32 s_freeWordCount = s_maxMemory / 4096 / 32;
static uint32_t s_freeBits[s_freeWordCount];

static void markBlocks(x86::reg32 begin, x86::reg32 count, bool free)
{
    x86::reg32 b = begin;
    const x86::reg32 end = begin + count;
    while (b < end)
    {
        const x86::reg32 bit = b & 31;
        const x86::reg32 n = std::min<x86::reg32>(32 - bit, end - b);
        const uint32_t mask = (n == 32 ? ~0u : ((1u << n) - 1)) << bit;
        if (free)
            s_freeBits[b >> 5] |= mask;
        else
            s_freeBits[b >> 5] &= ~mask;
        b += n;
    }
}

// First block from b on whose free bit equals free, or limit.
static x86::reg32 nextBlock(x86::reg32 b, x86::reg32 limit, bool free)
{
    if (b >= limit)
        return limit;
    x86::reg32 word = b >> 5;
    uint32_t bits = (free ? s_freeBits[word] : ~s_freeBits[word]) & (~0u << (b & 31));
    while (!bits)
    {
        if (++word >= (limit + 31) >> 5)
            return limit;
        bits = free ? s_freeBits[word] : ~s_freeBits[word];
    }
    return std::min<x86::reg32>((word << 5) + x86::reg32(__builtin_ctz(bits)), limit);
}

// Last block below e whose free bit equals free, plus 1; 0 when there is none.
static x86::reg32 previousBlockEnd(x86::reg32 e, bool free)
{
    if (e == 0)
        return 0;
    x86::reg32 word = (e - 1) >> 5;
    const x86::reg32 top = (e - 1) & 31;
    uint32_t bits = (free ? s_freeBits[word] : ~s_freeBits[word]) & (top == 31 ? ~0u : ((2u << top) - 1));
    while (!bits)
    {
        if (word == 0)
            return 0;
        --word;
        bits = free ? s_freeBits[word] : ~s_freeBits[word];
    }
    return (word << 5) + 32 - x86::reg32(__builtin_clz(bits));
}

// Lowest b with [b, b + count) free: the same block plain first fit from block 0 finds.
x86::reg32 MemMap::findLow(x86::reg32 count)
{
    x86::reg32 b = s_lowFree;
    bool first = true;
    while (b + count <= s_blockCount)
    {
        b = nextBlock(b, s_blockCount, true);
        if (b == s_blockCount)
            break;
        if (first)
        {
            s_lowFree = b;
            first = false;
        }
        const x86::reg32 end = b + count;
        if (end > s_blockCount)
            break;
        const x86::reg32 s = nextBlock(b, end, false);
        if (s == end)
            return b;
        b = s + 1;
    }
    return x86::reg32(-1);
}

// Highest b with [b, b + count) free.
x86::reg32 MemMap::findHigh(x86::reg32 count)
{
    x86::reg32 e = s_highFree;
    bool first = true;
    while (e >= count)
    {
        e = previousBlockEnd(e, true);
        if (first)
        {
            s_highFree = e;
            first = false;
        }
        if (e < count)
            break;
        const x86::reg32 begin = e - count;
        const x86::reg32 s = std::max(previousBlockEnd(e, false), begin);
        if (s == begin)
            return begin;
        e = s - 1;
    }
    return x86::reg32(-1);
}

MemMap::MemMap(x86::reg32 size, pf::perf::Alloc kind, Unplaced)
    :   m_block(x86::reg32(-1))
    ,   m_blockCount(size / 4096 + (size % 4096 != 0))
    ,   m_kind(kind)
{
    RC_ASSERT(m_blockCount >= 1);
}

MemMap::MemMap(x86::reg32 size, Placement placement, pf::perf::Alloc kind)
    :   MemMap(size, kind, Unplaced())
{
    if (!place(size, placement))
    {
        report(size);
        pf::fatal("guest memory exhausted (requested %u bytes)", size);
    }
}

MemMap* MemMap::tryCreate(x86::reg32 size, Placement placement, pf::perf::Alloc kind)
{
    MemMap* memMap = new MemMap(size, kind, Unplaced());
    if (memMap->place(size, placement))
        return memMap;
    delete memMap;
    return nullptr;
}

bool MemMap::place(x86::reg32 size, Placement placement)
{
    if (m_blockCount > s_blockCount)
        return false;
    const bool timed = pf::perf::timed();
    const bool searchTimed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    uint32_t searchUs = 0;
    for (;;)
    {
        s_lock->lock();
        const uint32_t searchStart = searchTimed ? pf::perf::now() : 0;
        m_block = placement == High ? findHigh(m_blockCount) : findLow(m_blockCount);
        if (searchTimed)
            searchUs += pf::perf::now() - searchStart;
        if (m_block != x86::reg32(-1))
            break;
        s_lock->unlock();
        if (!s_reclaim || !s_reclaim())
            return false;
    }
    memset(s_blocks + m_block, 1, m_blockCount);
    markBlocks(m_block, m_blockCount, false);
    if (m_block == s_lowFree)
        s_lowFree = m_block + m_blockCount;
    if (m_block + m_blockCount == s_highFree)
        s_highFree = m_block;
    s_memMaps.push_back(this);
    s_lock->unlock();
    if (timed)
        pf::perf::guestBlock(pf::perf::Alloc(m_kind), size, pf::perf::now() - start, searchUs);
    return true;
}

MemMap::~MemMap()
{
    if (m_block == x86::reg32(-1))
        return;
    s_lock->lock();
    for (std::vector<MemMap*>::iterator it = s_memMaps.begin(); it != s_memMaps.end(); ++it)
    {
        if (*it == this)
        {
            s_memMaps.erase(it);
            break;
        }
    }
    memset(s_blocks + m_block, 0, m_blockCount);
    markBlocks(m_block, m_blockCount, true);
    if (m_block < s_lowFree)
        s_lowFree = m_block;
    if (m_block + m_blockCount > s_highFree)
        s_highFree = m_block + m_blockCount;
    s_lock->unlock();
}

void MemMap::setReclaim(Reclaim reclaim)
{
    s_reclaim = reclaim;
}

static MemMap::Report s_reports[4];

void MemMap::addReport(Report report)
{
    for (MemMap::Report& slot : s_reports)
    {
        if (!slot)
        {
            slot = report;
            return;
        }
    }
}

static void logFreeMemory(const char* when)
{
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) >= 0)
        pf::logInfo("memory: free %s: user %d KB, cdram %d KB, phycont %d KB", when, info.size_user / 1024,
                    info.size_cdram / 1024, info.size_phycont / 1024);
}

void MemMap::report(x86::reg32 requested)
{
    static const int Largest = 8;
    x86::reg32 freeBlocks = 0, runs = 0, largestRun = 0;
    x86::reg32 kindBlocks[pf::perf::AllocCount] = {};
    x86::reg32 kindCounts[pf::perf::AllocCount] = {};
    const MemMap* largest[Largest] = {};
    size_t live = 0;
    s_lock->lock();
    for (x86::reg32 b = nextBlock(0, s_blockCount, true); b < s_blockCount; )
    {
        const x86::reg32 end = nextBlock(b, s_blockCount, false);
        freeBlocks += end - b;
        largestRun = std::max(largestRun, end - b);
        ++runs;
        b = nextBlock(end, s_blockCount, true);
    }
    for (const MemMap* m : s_memMaps)
    {
        const int kind = m->m_kind >= 0 && m->m_kind < pf::perf::AllocCount ? m->m_kind : 0;
        kindBlocks[kind] += m->m_blockCount;
        ++kindCounts[kind];
        for (int i = 0; i < Largest; ++i)
        {
            if (!largest[i] || largest[i]->m_blockCount < m->m_blockCount)
            {
                for (int j = Largest - 1; j > i; --j)
                    largest[j] = largest[j - 1];
                largest[i] = m;
                break;
            }
        }
    }
    live = s_memMaps.size();
    char line[512];
    int length = 0;
    for (int i = 0; i < Largest && largest[i]; ++i)
    {
        length += snprintf(line + length, sizeof(line) - length, " %s %u KB at 0x%08x;",
                           pf::perf::allocName(pf::perf::Alloc(largest[i]->m_kind)),
                           largest[i]->m_blockCount * 4, largest[i]->getBlockStart());
        if (length >= int(sizeof(line)))
            break;
    }
    s_lock->unlock();

    pf::logInfo("memory: guest exhausted, %u bytes requested; arena %u KB, free %u KB in %u ranges, largest %u KB, "
                "%u blocks live", requested, s_simulatedMemory / 1024, freeBlocks * 4, runs, largestRun * 4,
                unsigned(live));
    for (int kind = 0; kind < pf::perf::AllocCount; ++kind)
    {
        if (kindCounts[kind])
            pf::logInfo("memory: %s %u blocks, %u KB", pf::perf::allocName(pf::perf::Alloc(kind)),
                        kindCounts[kind], kindBlocks[kind] * 4);
    }
    pf::logInfo("memory: largest blocks:%s", line);
    logFreeMemory("now");
    for (MemMap::Report r : s_reports)
    {
        if (r)
            r();
    }
}

// Null for an address outside every block.
MemMap* MemMap::findBlock(x86::reg32 memIndex)
{
    if (memIndex < s_addressOffset || memIndex >= s_addressOffset + s_simulatedMemory)
        return nullptr;
    MemMap* result = nullptr;
    s_lock->lock();
    for (std::vector<MemMap*>::iterator it = s_memMaps.begin(); it != s_memMaps.end(); ++it)
    {
        if ((*it)->getBlockStart() <= memIndex && (*it)->getBlockStart() + (*it)->getBlockSize() > memIndex)
        {
            RC_ASSERT(!result);
            result = *it;
            break;
        }
    }
    s_lock->unlock();
    return result;
}

bool MemMap::isGuestRange(x86::reg32 address, x86::reg32 size)
{
    return x86::reg64(address) + size <= x86::reg64(s_addressOffset) + s_simulatedMemory;
}

void MemMap::init(x86::reg32 baseAddress, const std::vector<Section>& sections)
{
    if (!g_guestBase)
        pf::fatal("guest memory allocation of %u bytes failed: 0x%08x", s_arenaSize, unsigned(s_arenaBlock));
    pf::logInfo("guest memory: %u bytes at %p, %u KB for blocks (free user memory at startup %d KB, cdram %d KB)",
                s_arenaSize, static_cast<void*>(g_guestBase), s_simulatedMemory / 1024, s_freeAtStart.size_user / 1024,
                s_freeAtStart.size_cdram / 1024);
    logFreeMemory("after the guest memory");
    s_blockCount = s_simulatedMemory / 4096;
    s_blockSize = s_blockMapSize;
    s_lowFree = 0;
    s_highFree = s_blockCount;
    markBlocks(0, s_blockCount, true);
    x86::reg32 maxEnd = 0;
    for (std::vector<Section>::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        x86::reg32 end = it->baseAddress + it->size;
        if (end > maxEnd)
            maxEnd = end;
    }
    s_sectionSize = (maxEnd + 4095) & ~4095u;
    if (s_sectionSize > s_sectionLimit)
        pf::fatal("image sections end at 0x%08x, past the guest arena limit 0x%08x", s_sectionSize, s_sectionLimit);
    s_blocks = g_guestBase + s_sectionSize;
    s_addressOffset = s_sectionSize + s_blockSize;
    for (std::vector<Section>::const_iterator it = sections.begin(); it != sections.end(); ++it)
    {
        RC_ASSERT(it->baseAddress >= baseAddress);
        RC_ASSERT(it->baseAddress + it->size <= s_sectionSize);
        if (it->data)
        {
            memcpy(g_guestBase + it->baseAddress, it->data, it->size);
        }
    }
    s_lock = new Mutex("MemMap");
}

void MemMap::fini()
{
    std::vector<MemMap*> memMapsCopy(s_memMaps);
    for (std::vector<MemMap*>::iterator it = memMapsCopy.begin(); it != memMapsCopy.end(); ++it)
    {
        delete *it;
    }
    RC_ASSERT(s_memMaps.empty());
    s_blocks = nullptr;
    s_blockCount = 0;
    s_blockSize = 0;
    s_addressOffset = 0;
    delete s_lock;
    s_lock = nullptr;
    freeArena();
}

void MemMap::fillDebugGraph(x86::reg16* graph)
{
    s_lock->lock();
    const x86::reg32 blocksPerPixel = (s_blockCount+639)/640;
    for (x86::reg32 b = 0; b < s_blockCount; b += blocksPerPixel)
    {
        x86::reg16 c = 0;
        for (x86::reg32 b1 = b; b1 < b+blocksPerPixel; ++b1)
        {
            if (s_blocks[b1])
                c++;
        }
        x86::reg16 r = (0x1f*c/blocksPerPixel) << (5+6);
        x86::reg16 g = (0x3f*(blocksPerPixel-c)/blocksPerPixel) << 5;
        graph[b/blocksPerPixel + 640*0] = r|g;
        graph[b/blocksPerPixel + 640*1] = r|g;
        graph[b/blocksPerPixel + 640*2] = r|g;
    }
    s_lock->unlock();
}

void MemMap::fillDebugGraph(x86::reg32* graph)
{
    s_lock->lock();
    const x86::reg32 blocksPerPixel = (s_blockCount+639)/640;
    for (x86::reg32 b = 0; b < s_blockCount; b += blocksPerPixel)
    {
        x86::reg16 c = 0;
        for (x86::reg32 b1 = b; b1 < b+blocksPerPixel; ++b1)
        {
            if (s_blocks[b1])
                c++;
        }
        x86::reg32 r = (0xff*c/blocksPerPixel) << 24;
        x86::reg32 g = (0xff*(blocksPerPixel-c)/blocksPerPixel) << 16;
        graph[b/blocksPerPixel + 640*0] = r|g;
        graph[b/blocksPerPixel + 640*1] = r|g;
        graph[b/blocksPerPixel + 640*2] = r|g;
    }
    s_lock->unlock();
}

}
