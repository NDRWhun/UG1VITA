#include <lib/memmap.h>
#include <lib/mutex.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <nfsu_layout.h>
#include <psp2/kernel/sysmem.h>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace win32
{

// Guest heap, thread stacks and shim buffers.
static const x86::reg32 s_simulatedMemory = 96*1024*1024;
// One byte per 4 KB block of the simulated memory, page-aligned.
static const x86::reg32 s_blockMapSize = (s_simulatedMemory / 4096 + 4095) & ~4095u;
// End of the highest image section (the generated layout), page-aligned.
static const x86::reg32 s_sectionLimit = (nfsu::layout::imageEnd + 4095) & ~4095u;
// Layout: sections, block map, simulated memory.
static const x86::reg32 s_arenaSize = s_sectionLimit + s_blockMapSize + s_simulatedMemory;
static_assert(s_arenaSize % 4096 == 0, "memblock sizes are whole pages");

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
static x86::reg8* allocArena()
{
    s_arenaBlock = sceKernelAllocMemBlock("guest",SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW, s_arenaSize, nullptr);
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

// Lowest b with [b, b + count) free: the same block plain first fit from block 0 finds.
x86::reg32 MemMap::findLow(x86::reg32 count)
{
    x86::reg32 b = s_lowFree;
    bool first = true;
    while (b + count <= s_blockCount)
    {
        const void* free = memchr(s_blocks + b, 0, s_blockCount - b);
        if (!free)
            break;
        b = x86::reg32(static_cast<const x86::reg8*>(free) - s_blocks);
        if (first)
        {
            s_lowFree = b;
            first = false;
        }
        const x86::reg32 end = b + count;
        if (end > s_blockCount)
            break;
        x86::reg32 s = b;
        while (s < end && !s_blocks[s])
            ++s;
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
        while (e > 0 && s_blocks[e - 1])
            --e;
        if (first)
        {
            s_highFree = e;
            first = false;
        }
        if (e < count)
            break;
        const x86::reg32 begin = e - count;
        x86::reg32 s = e;
        while (s > begin && !s_blocks[s - 1])
            --s;
        if (s == begin)
            return begin;
        e = s - 1;
    }
    return x86::reg32(-1);
}

MemMap::MemMap(x86::reg32 size, Placement placement, pf::perf::Alloc kind)
    :   m_block(x86::reg32(-1))
    ,   m_blockCount(size / 4096 + (size % 4096 != 0))
{
    RC_ASSERT(m_blockCount >= 1);
    if (m_blockCount > s_blockCount)
        pf::fatal("guest memory exhausted (requested %u bytes)", size);
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    uint32_t searchUs = 0;
    for (;;)
    {
        s_lock->lock();
        const uint32_t searchStart = timed ? pf::perf::now() : 0;
        m_block = placement == High ? findHigh(m_blockCount) : findLow(m_blockCount);
        if (timed)
            searchUs += pf::perf::now() - searchStart;
        if (m_block != x86::reg32(-1))
            break;
        s_lock->unlock();
        if (!s_reclaim || !s_reclaim())
            pf::fatal("guest memory exhausted (requested %u bytes)", size);
    }
    memset(s_blocks + m_block, 1, m_blockCount);
    if (m_block == s_lowFree)
        s_lowFree = m_block + m_blockCount;
    if (m_block + m_blockCount == s_highFree)
        s_highFree = m_block;
    s_memMaps.push_back(this);
    s_lock->unlock();
    if (timed)
        pf::perf::guestBlock(kind, size, pf::perf::now() - start, searchUs);
}

MemMap::~MemMap()
{
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
    pf::logInfo("guest memory: %u bytes at %p", s_arenaSize, static_cast<void*>(g_guestBase));
    s_blockCount = s_simulatedMemory / 4096;
    s_blockSize = s_blockMapSize;
    s_lowFree = 0;
    s_highFree = s_blockCount;
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
