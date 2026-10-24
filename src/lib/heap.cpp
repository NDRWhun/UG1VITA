#include <lib/heap.h>
#include <lib/memmap.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace win32 { namespace heap
{

namespace
{

const x86::reg32 Granularity = 16;
const x86::reg32 ChunkSize = 1024 * 1024;
// Blocks up to this size come from size classes; larger ones are guest memory blocks of their own.
const x86::reg32 LargeSize = 256 * 1024;

struct Block
{
    x86::reg32  size;       // requested
    int         sizeClass;  // -1 for a large block
};

struct State
{
    pf::LwMutex                                 mutex{"heap"};
    std::vector<x86::reg32>                     classSizes;     // ascending
    std::vector<std::vector<x86::reg32>>        freeLists;      // per class
    std::unordered_map<x86::reg32, Block>       blocks;         // live blocks by address
    std::unordered_map<x86::reg32, MemMap*>     large;
    std::vector<MemMap*>                        chunks;
    x86::reg32                                  chunkNext = 0;  // next free byte of the last chunk
    x86::reg32                                  chunkEnd = 0;
    x86::reg32                                  used = 0;

    State()
    {
        // 16-byte steps to 1 KB, then about 1/8 growth, 16-byte aligned, up to LargeSize.
        for (x86::reg32 size = Granularity; size <= 1024; size += Granularity)
            classSizes.push_back(size);
        for (x86::reg32 size = 1024; size < LargeSize; )
        {
            size = (size + size / 8 + Granularity - 1) & ~(Granularity - 1);
            classSizes.push_back(size < LargeSize ? size : LargeSize);
        }
        freeLists.resize(classSizes.size());
    }

    int classOf(x86::reg32 size) const
    {
        size_t low = 0, high = classSizes.size();
        while (low < high)
        {
            const size_t mid = (low + high) / 2;
            if (classSizes[mid] < size)
                low = mid + 1;
            else
                high = mid;
        }
        return int(low);
    }

    // Carves a block of a class out of the current chunk; a new chunk when it does not fit.
    x86::reg32 carve(x86::reg32 size)
    {
        if (chunkEnd - chunkNext < size)
        {
            MemMap* chunk = new MemMap(ChunkSize, MemMap::Low, pf::perf::AllocHeapChunk);
            chunks.push_back(chunk);
            chunkNext = chunk->getBlockStart();
            chunkEnd = chunkNext + chunk->getBlockSize();
        }
        const x86::reg32 address = chunkNext;
        chunkNext += size;
        return address;
    }
};

State& state()
{
    static State s_state;
    return s_state;
}

}

x86::reg32 allocate(x86::reg32 size, bool zero)
{
    State& s = state();
    const x86::reg32 rounded = size ? (size + Granularity - 1) & ~(Granularity - 1) : Granularity;
    x86::reg32 address;
    int sizeClass = -1;
    {
        pf::LwLockGuard guard(s.mutex);
        if (rounded <= LargeSize)
        {
            sizeClass = s.classOf(rounded);
            std::vector<x86::reg32>& list = s.freeLists[sizeClass];
            if (!list.empty())
            {
                address = list.back();
                list.pop_back();
            }
            else
            {
                address = s.carve(s.classSizes[sizeClass]);
            }
        }
        else
        {
            MemMap* block = new MemMap(rounded, MemMap::Low, pf::perf::AllocHeapLarge);
            address = block->getBlockStart();
            s.large[address] = block;
        }
        s.blocks[address] = Block{size, sizeClass};
        s.used += size;
    }
    if (zero)
        memset(g_guestBase + address, 0, size);
    return address;
}

bool release(x86::reg32 address)
{
    State& s = state();
    MemMap* large = nullptr;
    {
        pf::LwLockGuard guard(s.mutex);
        const auto it = s.blocks.find(address);
        if (it == s.blocks.end())
            return false;
        s.used -= it->second.size;
        if (it->second.sizeClass >= 0)
        {
            s.freeLists[it->second.sizeClass].push_back(address);
        }
        else
        {
            const auto block = s.large.find(address);
            large = block->second;
            s.large.erase(block);
        }
        s.blocks.erase(it);
    }
    delete large;
    return true;
}

x86::reg32 size(x86::reg32 address)
{
    State& s = state();
    pf::LwLockGuard guard(s.mutex);
    const auto it = s.blocks.find(address);
    return it == s.blocks.end() ? 0xffffffff : it->second.size;
}

x86::reg32 used()
{
    State& s = state();
    pf::LwLockGuard guard(s.mutex);
    return s.used;
}

}}
