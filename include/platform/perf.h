#ifndef PLATFORM_PERF_H_
#define PLATFORM_PERF_H_

#include <cstdint>

namespace pf { namespace perf {

// Time sums in microseconds, reported per frame and reset with every log line.
enum Counter
{
    SlotWait,       // game thread waiting for a free frame slot, FenceWait included
    FenceWait,      // game thread waiting for the GPU to finish reading a frame slot
    GilWait,        // game thread re-taking the execution lock after a frame present
    Render,         // render thread building and queueing a frame, QueueWait included
    QueueWait,      // render thread blocked in sceGxmDisplayQueueAddEntry
    GpuLatency,     // sceGxmEndScene until the display callback runs, frames queued ahead included
    FlipWait,       // display callback setting the frame buffer and waiting for vblank
    Preempted,      // normal-priority threads (the game thread) giving the execution lock to preempting threads
    CounterCount
};

// Why the game thread gave up the execution lock.
enum Block
{
    BlockOther,
    BlockSleep,     // Sleep
    BlockWait,      // WaitForSingleObject / WaitForMultipleObjects
    BlockCritSec,   // EnterCriticalSection held by another thread
    BlockFile,      // ReadFile, WriteFile, MapViewOfFile
    BlockSwap,      // Flip / Present: frame slot wait
    BlockPreempt,   // preemption point, a higher-level thread took the lock
    BlockCount
};

// Kinds of threads holding the execution lock.
enum Holder
{
    HolderGame,
    HolderTimer,    // timer callbacks
    HolderIo,       // file reading threads
    HolderHelper,   // other guest threads
    HolderMain,     // message loop
    HolderCount
};

// Texture uploads and guest memory blocks: event counts and sums, logged as one line per interval.
enum Upload
{
    UploadLevels,           // levels written to the GPU copy
    UploadBytes,
    UploadAsync,            // writes and copies run on the job workers
    UploadWorkerUs,         // worker time of those
    UploadReadBacks,        // locks that read a level back from the GPU copy
    UploadReadBackBytes,
    UploadReadBackUs,
    UploadCacheHits,        // locks served from a kept linear copy
    UploadEvictions,        // kept copies dropped (byte cap or guest memory pressure)
    UploadRenderWaits,      // render thread waits for writes of textures its frame reads
    UploadRenderWaitUs,
    UploadGameWaits,        // game thread waits for writes (re-lock, read-back, eviction, release)
    UploadGameWaitUs,
    UploadGuestBlocks,      // guest memory blocks created (MemMap), any caller
    UploadGuestBlockUs,
    UploadCount
};

extern bool g_enabled;
extern int g_nextBlock;

// Main thread, before any other thread starts: sets the switch and logs start-up facts.
void init(bool enabled);
inline bool enabled() { return g_enabled; }
// The thread holding the execution lock names the reason of its next release (read and reset by the release).
inline void blocking(Block block) { g_nextBlock = block; }
// Game thread: the execution lock was given up for a reason from release to re-acquisition, relockUs of it waiting
// for the lock.
void gameBlocked(Block block, uint32_t microseconds, uint32_t relockUs);
// The calling thread's CPU time is logged under this name (a string literal); a few threads at most.
void trackThread(const char* name);
// Time a thread of a kind held the execution lock.
void held(Holder holder, uint32_t microseconds);
// Timer service: one timer callback ran for this long, waiting for the lock included.
void timerFired(uint32_t callback, uint32_t microseconds);
// Process time in microseconds, wrapping; the difference of two readings is a duration.
uint32_t now();
// Any thread.
void add(Counter counter, uint32_t microseconds);
// Any thread: time a preempting thread (timer callback, raised guest priority) waited for the execution lock.
void preemptWait(uint32_t microseconds);
// Display callback: GPU time of one frame, from the later of its sceGxmEndScene and the previous frame's
// completion until its own completion; only reported for frames whose completion time is known.
void gpuBusy(uint32_t microseconds);
// Any thread: adds to an upload counter.
void upload(Upload counter, uint32_t value);
// Any thread: a guest memory block was created in this many microseconds.
inline void guestBlock(uint32_t microseconds)
{
    upload(UploadGuestBlocks, 1);
    upload(UploadGuestBlockUs, microseconds);
}
// Game thread: kept linear texture copies at the moment.
void uploadCache(uint32_t bytes, uint32_t levels);
// Render thread, after each game frame it queued; logs one line about every 5 s.
void framePresented(uint32_t drawCalls, uint32_t triangles);
// Called on the render thread after each log line with the interval length in microseconds.
typedef void (*Reporter)(uint32_t intervalUs);
void setReporter(Reporter reporter);

}}

#endif
