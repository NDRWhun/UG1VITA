#ifndef LIB_MEMMAP_H
#define LIB_MEMMAP_H

#include <x86.h>
#include <vector>

namespace pf { namespace perf { enum Alloc : int; } }

namespace win32
{

class Mutex;

// Host address of guest address 0: one memblock allocated by a static initialiser in memmap.cpp.
// Const, so compiled code need not reload it after guest stores. No static initialiser may touch
// guest memory.
extern x86::reg8* const g_guestBase;

struct Section
{
    const x86::reg8*    data;
    x86::reg32          baseAddress;
    x86::reg32          size;
};

class MemMap
{
public:
    // Low: the lowest free range (first fit). High: the highest free range, for buffers of the runtime itself.
    enum Placement
    {
        Low,
        High
    };
    // Called without the allocator lock when no range fits; returns true when it freed guest memory, and the
    // allocation is tried again.
    typedef bool (*Reclaim)();

    // Called when guest memory is exhausted, before the fatal error, to log what holds it.
    typedef void (*Report)();

    // The kind names the caller in the timing_log block counts and the exhaustion report. Exhaustion is fatal.
    MemMap(x86::reg32 size, Placement placement = Low, pf::perf::Alloc kind = pf::perf::Alloc(0));
    ~MemMap();
    // Null instead of the fatal error when no range fits after the reclaim.
    static MemMap* tryCreate(x86::reg32 size, Placement placement, pf::perf::Alloc kind);

    x86::reg32 getBlockStart() const  { return s_addressOffset + m_block * 4096; }
    x86::reg32 getBlockSize() const   { return m_blockCount * 4096; }

    static void init(x86::reg32 baseAddress, const std::vector<Section>& sections);
    static void fini();
    static MemMap* findBlock(x86::reg32 memIndex);
    // True when [address, address + size) lies inside the guest address space.
    static bool isGuestRange(x86::reg32 address, x86::reg32 size);
    static void setReclaim(Reclaim reclaim);
    // Up to 4 reports.
    static void addReport(Report report);
    // Logs the free ranges, the live blocks per kind, the largest blocks and the added reports.
    static void report(x86::reg32 requested);

    static void fillDebugGraph(x86::reg16* graph);
    static void fillDebugGraph(x86::reg32* graph);
private:
    struct Unplaced {};
    MemMap(x86::reg32 size, pf::perf::Alloc kind, Unplaced);
    // Takes a free range, after the reclaim if needed; false when none fits.
    bool place(x86::reg32 size, Placement placement);

    // First block of a free range of count blocks, or -1; s_lock held.
    static x86::reg32 findLow(x86::reg32 count);
    static x86::reg32 findHigh(x86::reg32 count);

    x86::reg32 m_block;
    x86::reg32 m_blockCount;
    int        m_kind;

private:
    static x86::reg8*           s_blocks;
    static x86::reg32           s_blockCount;
    static x86::reg32           s_blockSize;
    static x86::reg32           s_sectionSize;
    static x86::reg32           s_addressOffset;
    static x86::reg32           s_lowFree;      // every block below is used
    static x86::reg32           s_highFree;     // every block from here on is used
    static Reclaim              s_reclaim;
    static Mutex*               s_lock;
    static std::vector<MemMap*> s_memMaps;
};

}

#endif // MEMMAP_H
