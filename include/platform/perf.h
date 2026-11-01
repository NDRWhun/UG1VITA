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

// Callers of guest memory blocks (MemMap), counted per kind.
enum Alloc : int
{
    AllocOther,
    AllocVirtual,       // VirtualAlloc
    AllocHeapLarge,     // HeapAlloc / GlobalAlloc / LocalAlloc above the size classes
    AllocHeapChunk,     // a chunk of the size-class heap
    AllocFileView,      // MapViewOfFile
    AllocTextureLock,   // D3D texture level lock buffer
    AllocBuffer,        // D3D vertex / index buffer
    AllocThread,        // thread block and stack
    AllocSound,         // DirectSound buffer
    AllocCount
};

// EA SND voice render and the routines of its audio banks, logged as one line per interval.
enum Audio
{
    AudioRendersUnlocked,       // voice renders without the execution lock
    AudioRenderUnlockedUs,
    AudioRendersLocked,         // voice renders under the lock (the audit failed)
    AudioRenderLockedUs,
    AudioLoopsUnlocked,         // runs of the render loop 0x654050 without the execution lock
    AudioLoopUnlockedUs,        // their time, the parts that took the lock again included
    AudioTicksLocked,           // SND ticks (0x64E330) that took the lock again (the audit failed)
    AudioCallbacksLocked,       // millisecond callback walks (0x64E2F0) that took the lock again
    AudioNotificationsLocked,   // stream notification deliveries (0x655880) that took the lock again
    AudioLibraryLocked,         // library calls of bank routines and object releases that took the lock again
    AudioBankRuns,              // bank routines run
    AudioBankUs,                // their time, called library functions included
    AudioBankInstructions,      // bank instructions interpreted
    AudioBankDecodes,           // bank instructions decoded (not taken from the cache)
    AudioBankNative,            // bank routine runs translated (nested ones included)
    AudioBankInterpreted,       // bank routine runs interpreted (nested and shadow ones included)
    AudioOutputSamples,         // samples the SND output module converted to 16 bits
    AudioSaturatedHigh,         // of them saturated to 32767
    AudioSaturatedLow,          // of them saturated to -32768
    AudioCount
};

// Event counts of all threads, taken by the phase log (timing_phases).
enum Count
{
    CountClock,         // process clock reads (now, ticksUs, ticksMs)
    CountCritSec,       // EnterCriticalSection
    CountCritSecWait,   // of them blocking
    CountRelease,       // execution lock releases (outermost)
    CountYield,         // preemption points that gave the lock to a waiting thread
    CountCount
};

// Event sums of all threads for the hitch log (hitch_log); a count is followed by its time where both exist.
enum Event
{
    EventTextureLocks,      // texture level locks
    EventBufferLocks,       // vertex and index buffer locks (outermost)
    EventFileReads,         // ReadFile and MapViewOfFile calls
    EventFileBytes,
    EventFileUs,            // their read time
    EventJdlz,              // JDLZ decompressions (0x58D190)
    EventJdlzUs,
    EventTextureCreates,    // texture creations of texture records (0x407DB0)
    EventTextureCreateUs,
    EventSkins,             // car skin compositions (CompositeSkin32, 0x5A8A50)
    EventSkinUs,
    EventDrainWaits,        // waits for the Direct3D queue thread (drains and a full queue)
    EventDrainWaitUs,
    EventQueueRunUs,        // Direct3D queue run time (the queue thread, or the draining thread when inline)
    EventSlotWaitUs,        // game thread opening a frame: waiting for a free frame slot, the lock re-taken included
    EventFenceWaitUs,       // of it: waiting for the GPU to finish the slot's previous frame
    EventCount
};

// Sums since start-up, wrapping, counted while the hitch log is on: the difference of two snapshots is what
// happened in between. Only uint32_t fields.
struct Totals
{
    uint32_t events[EventCount];
    uint32_t uploads[UploadCount];
    uint32_t blocked[BlockCount];   // game thread lock releases by reason, re-acquisition included
    uint32_t releases;
    uint32_t relockUs;              // of the blocked time: waiting to take the lock again
    uint32_t held[HolderCount];     // execution lock hold time by kind of holder
    uint32_t preempted;             // Counter Preempted
    uint32_t preemptWaits;          // preemptWait calls and their time
    uint32_t preemptWaitUs;
};

// Hitch log: a present whose interval from the previous present exceeded HitchUs, with the render thread's time for
// the frame (resolve, draw and display queueing) and its running averages.
const uint32_t HitchUs = 41700;
struct SlowPresent
{
    uint32_t frame;         // render::Frame index, 0 for a layer present
    uint32_t presentUs;     // now() after the present
    uint32_t intervalUs;
    uint32_t renderUs;      // render thread, queueUs included
    uint32_t queueUs;       // sceGxmDisplayQueueAddEntry
    uint32_t renderAvgUs;   // running averages over every present
    uint32_t queueAvgUs;
};

extern bool g_enabled;
extern bool g_timed;
extern bool g_hitch;
extern bool g_counting;
extern int g_nextBlock;

void eventSlow(Event event, uint32_t value);
// Any thread: adds to an event sum while the hitch log is on.
inline void event(Event event, uint32_t value)
{
    if (__builtin_expect(g_hitch, 0))
        eventSlow(event, value);
}
// True with hitch_log=on.
inline bool hitch() { return g_hitch; }
// True with the timing log or the hitch log on: the timed counters (lock releases, waits, uploads) are measured.
inline bool timed() { return g_timed; }
// Any thread: the sums at the moment (each field read on its own).
void totals(Totals& out);
// Game thread: the oldest slow present not taken yet, null if none; popSlowPresent removes it.
const SlowPresent* peekSlowPresent();
void popSlowPresent();
// Slow presents dropped because the game thread did not take them in time.
uint32_t droppedSlowPresents();

void countSlow(Count counter);
// Any thread: counts an event while counting is on.
inline void count(Count counter)
{
    if (__builtin_expect(g_counting, 0))
        countSlow(counter);
}
// Turns the counts on (before other threads start).
void startCounting();
// The count since the last take.
uint32_t takeCount(Count counter);

// Main thread, before any other thread starts: sets the switches (timing log, hitch log) and logs start-up facts.
void init(bool enabled, bool hitchLog);
inline bool enabled() { return g_enabled; }
// The thread holding the execution lock names the reason of its next release (read and reset by the release).
inline void blocking(Block block) { g_nextBlock = block; }
// Game thread: the execution lock was given up for a reason from release to re-acquisition, relockUs of it waiting
// for the lock.
void gameBlocked(Block block, uint32_t microseconds, uint32_t relockUs);
// Sum of the game thread's lock releases since start-up in microseconds, wrapping (counted while timed()).
uint32_t gameBlockedTotal();
// The calling thread's CPU time is logged under this name (a string literal); a few threads at most.
void trackThread(const char* name);
// Slot of a named execution lock holder (a string literal), -1 when the table is full.
int holderSlot(const char* name);
// Time a thread of a kind held the execution lock; slot from holderSlot, or -1.
void held(Holder holder, int slot, uint32_t microseconds);
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
// Any thread: a guest memory block of a kind was created in this many microseconds, searchUs of them finding the
// free range.
void guestBlock(Alloc kind, uint32_t bytes, uint32_t microseconds, uint32_t searchUs);
// Any thread: adds to an audio counter.
void audio(Audio counter, uint32_t value);
// Audio thread: a voice render or a part of the SND tick kept the execution lock, with the failed audit check (a
// string literal) and the guest address it failed on.
void audioLocked(const char* reason, uint32_t address);
// Game thread: kept linear texture copies at the moment.
void uploadCache(uint32_t bytes, uint32_t levels);
// Render thread, after each game frame it queued, with the timing or hitch log on or a frame observer set: frame is
// the render::Frame index (0 for a layer present), renderUs and queueUs its render thread and display queue times
// (measured when timed()). Calls the observer, queues a slow present for the hitch log and, with the timing log on,
// logs one line about every 5 s.
void framePresented(uint32_t frame, uint32_t renderUs, uint32_t queueUs, uint32_t drawCalls, uint32_t triangles);
// True when a frame observer is set.
bool frameObserved();
// Called on the render thread after each log line with the interval length in microseconds.
typedef void (*Reporter)(uint32_t intervalUs);
void setReporter(Reporter reporter);
// Called on the render thread at each framePresented with its now() time.
typedef void (*FrameObserver)(uint32_t presentUs);
void setFrameObserver(FrameObserver observer);

}}

#endif
