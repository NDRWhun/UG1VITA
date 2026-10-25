#include <platform/perf.h>
#include <platform/log.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace pf { namespace perf {

bool g_enabled = false;
bool g_counting = false;
int g_nextBlock = BlockOther;

namespace
{

std::atomic<uint32_t> s_counts[CountCount];
std::atomic<uint32_t> s_blockedTotal{0};

const uint32_t LogIntervalUs = 5000000;

std::atomic<uint32_t> s_counters[CounterCount];
std::atomic<Reporter> s_reporter{nullptr};
std::atomic<FrameObserver> s_frameObserver{nullptr};
std::atomic<uint32_t> s_preemptCount{0};
std::atomic<uint32_t> s_preemptSum{0};
std::atomic<uint32_t> s_preemptMax{0};
std::atomic<uint32_t> s_gpuBusyCount{0};
std::atomic<uint32_t> s_gpuBusySum{0};

double msPerFrame(uint32_t microseconds, uint32_t frames)
{
    return double(microseconds) / 1000.0 / double(frames);
}

std::atomic<uint32_t> s_blocked[BlockCount];
std::atomic<uint32_t> s_blockCount{0};
std::atomic<uint32_t> s_relock{0};
std::atomic<uint32_t> s_held[HolderCount];
std::atomic<uint32_t> s_uploads[UploadCount];
std::atomic<uint32_t> s_cacheBytes{0};
std::atomic<uint32_t> s_cacheLevels{0};
std::atomic<uint32_t> s_audio[AudioCount];

// Execution lock holders by name.
struct HolderStats
{
    std::atomic<const char*> name{nullptr};
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> us{0};
    std::atomic<uint32_t> max{0};
};
const int HolderSlots = 8;
HolderStats s_holders[HolderSlots];

// Guest memory blocks by caller kind.
struct AllocStats
{
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> kb{0};
    std::atomic<uint32_t> us{0};
    std::atomic<uint32_t> searchUs{0};
};
AllocStats s_allocs[AllocCount];
const char* const s_allocNames[AllocCount] = { "other", "virtual", "heap", "chunk", "view", "texlock", "buffer",
                                               "thread", "sound" };

// Reasons of voice renders and SND tick parts under the lock, with the guest address of the first failure.
struct LockedAudio
{
    std::atomic<const char*> reason{nullptr};
    std::atomic<uint32_t> address{0};
    std::atomic<uint32_t> count{0};
};
const int LockedAudioSlots = 10;
LockedAudio s_lockedAudio[LockedAudioSlots];

// Timer callbacks by guest address; written by the timer service thread.
struct TimerStats
{
    std::atomic<uint32_t> callback;
    std::atomic<uint32_t> count;
    std::atomic<uint32_t> us;
    std::atomic<uint32_t> max;
};
const int TimerCount = 4;
TimerStats s_timers[TimerCount];

// Threads whose CPU time is logged.
struct Tracked
{
    std::atomic<const char*> name;
    SceUID thread;
    uint64_t lastClocks;    // updated by the log
};
const int TrackedCount = 6;
Tracked s_tracked[TrackedCount];
std::atomic<int> s_trackedCount{0};

const char* const s_blockNames[BlockCount] = { "other", "sleep", "wait", "cs", "file", "swap", "preempt" };
const char* const s_holderNames[HolderCount] = { "game", "timer", "io", "helper", "main" };

void raiseMax(std::atomic<uint32_t>& max, uint32_t value)
{
    uint32_t current = max.load(std::memory_order_relaxed);
    while (value > current && !max.compare_exchange_weak(current, value, std::memory_order_relaxed))
    {
    }
}

struct Line
{
    char text[1024] = {};
    int length = 0;

    __attribute__((format(printf, 2, 3))) void append(const char* format, ...)
    {
        if (length >= int(sizeof(text)))
            return;
        va_list args;
        va_start(args, format);
        length += vsnprintf(text + length, sizeof(text) - length, format, args);
        va_end(args);
    }
};

// Second log line, per frame: where the game thread's time without the execution lock went, who held the lock,
// CPU time of the tracked threads and the timer callbacks.
void logBlocking(uint32_t elapsed, uint32_t frames)
{
    Line line;
    uint32_t blocked[BlockCount];
    uint32_t total = 0;
    for (int i = 0; i < BlockCount; ++i)
    {
        blocked[i] = s_blocked[i].exchange(0, std::memory_order_relaxed);
        total += blocked[i];
    }
    line.append("perf: game blocked %.1f ms (%.1f releases, relock %.1f):", msPerFrame(total, frames),
                double(s_blockCount.exchange(0, std::memory_order_relaxed)) / frames,
                msPerFrame(s_relock.exchange(0, std::memory_order_relaxed), frames));
    for (int i = 0; i < BlockCount; ++i)
        line.append(" %s %.1f", s_blockNames[i], msPerFrame(blocked[i], frames));
    line.append(" | lock held:");
    for (int i = 0; i < HolderCount; ++i)
        line.append(" %s %.1f", s_holderNames[i],
                    msPerFrame(s_held[i].exchange(0, std::memory_order_relaxed), frames));
    line.append(" | cpu");
    const int tracked = std::min(s_trackedCount.load(std::memory_order_acquire), TrackedCount);
    for (int i = 0; i < tracked; ++i)
    {
        Tracked& thread = s_tracked[i];
        const char* name = thread.name.load(std::memory_order_acquire);
        SceKernelThreadInfo info;
        info.size = sizeof(info);
        if (!name || sceKernelGetThreadInfo(thread.thread, &info) < 0)
            continue;
        const uint64_t clocks = info.runClocks;
        line.append(" %s %.1f", name, msPerFrame(uint32_t(clocks - thread.lastClocks), frames));
        thread.lastClocks = clocks;
    }
    line.append(" | timers");
    for (TimerStats& timer : s_timers)
    {
        const uint32_t callback = timer.callback.load(std::memory_order_relaxed);
        if (!callback)
            break;
        const uint32_t count = timer.count.exchange(0, std::memory_order_relaxed);
        const uint32_t us = timer.us.exchange(0, std::memory_order_relaxed);
        const uint32_t max = timer.max.exchange(0, std::memory_order_relaxed);
        line.append(" %08x %.0f/s %.2f max %.1f ms", unsigned(callback), double(count) * 1000000.0 / double(elapsed),
                    count ? double(us) / 1000.0 / double(count) : 0.0, double(max) / 1000.0);
    }
    logInfo("%s", line.text);
}

// Third log line, per interval: texture uploads, read-backs, the kept linear copies, waits for worker writes and
// guest memory blocks.
void logUploads()
{
    uint32_t u[UploadCount];
    for (int i = 0; i < UploadCount; ++i)
        u[i] = s_uploads[i].exchange(0, std::memory_order_relaxed);
    const double mb = 1.0 / (1024.0 * 1024.0);
    logInfo("perf: textures %u uploads %.1f MB (%u on workers, %.1f ms) | read-backs %u %.1f MB %.1f ms"
            " | cache %u hits %u evictions, %.1f MB in %u levels | waits render %u %.1f ms game %u %.1f ms"
            " | guest blocks %u %.1f ms",
            unsigned(u[UploadLevels]), double(u[UploadBytes]) * mb, unsigned(u[UploadAsync]),
            double(u[UploadWorkerUs]) / 1000.0, unsigned(u[UploadReadBacks]), double(u[UploadReadBackBytes]) * mb,
            double(u[UploadReadBackUs]) / 1000.0, unsigned(u[UploadCacheHits]), unsigned(u[UploadEvictions]),
            double(s_cacheBytes.load(std::memory_order_relaxed)) * mb,
            unsigned(s_cacheLevels.load(std::memory_order_relaxed)), unsigned(u[UploadRenderWaits]),
            double(u[UploadRenderWaitUs]) / 1000.0, unsigned(u[UploadGameWaits]), double(u[UploadGameWaitUs]) / 1000.0,
            unsigned(u[UploadGuestBlocks]), double(u[UploadGuestBlockUs]) / 1000.0);
}

// Fourth log line, per interval: execution lock hold time by thread (ms per frame, holds per second, longest hold)
// and the guest memory blocks by caller kind (count, KB, ms, ms of it searching the block map).
void logHolders(uint32_t elapsed, uint32_t frames)
{
    Line line;
    line.append("perf: lock by thread:");
    for (HolderStats& holder : s_holders)
    {
        const char* name = holder.name.load(std::memory_order_acquire);
        if (!name)
            break;
        const uint32_t count = holder.count.exchange(0, std::memory_order_relaxed);
        const uint32_t us = holder.us.exchange(0, std::memory_order_relaxed);
        const uint32_t max = holder.max.exchange(0, std::memory_order_relaxed);
        line.append(" %s %.1f (%.0f/s max %.1f)", name, msPerFrame(us, frames),
                    double(count) * 1000000.0 / double(elapsed), double(max) / 1000.0);
    }
    line.append(" | blocks:");
    for (int i = 0; i < AllocCount; ++i)
    {
        AllocStats& alloc = s_allocs[i];
        const uint32_t count = alloc.count.exchange(0, std::memory_order_relaxed);
        const uint32_t kb = alloc.kb.exchange(0, std::memory_order_relaxed);
        const uint32_t us = alloc.us.exchange(0, std::memory_order_relaxed);
        const uint32_t searchUs = alloc.searchUs.exchange(0, std::memory_order_relaxed);
        if (count)
            line.append(" %s %u %u KB %.2f (search %.2f) ms", s_allocNames[i], unsigned(count), unsigned(kb),
                        double(us) / 1000.0, double(searchUs) / 1000.0);
    }
    logInfo("%s", line.text);
}

// Fifth log line, per interval: EA SND voice renders without and with the execution lock (count, ms per frame,
// why the lock was kept, for the tick parts too), the render loop without the lock (runs, ms per frame, the parts
// that took it again), the bank routines (runs, ms per frame, translated and interpreted runs, instructions
// interpreted, decodes) and the SND output module's 16-bit saturation.
void logAudio(uint32_t frames)
{
    uint32_t a[AudioCount];
    for (int i = 0; i < AudioCount; ++i)
        a[i] = s_audio[i].exchange(0, std::memory_order_relaxed);
    Line line;
    line.append("perf: audio renders unlocked %u %.2f ms locked %u %.2f ms (", unsigned(a[AudioRendersUnlocked]),
                msPerFrame(a[AudioRenderUnlockedUs], frames), unsigned(a[AudioRendersLocked]),
                msPerFrame(a[AudioRenderLockedUs], frames));
    bool first = true;
    for (LockedAudio& locked : s_lockedAudio)
    {
        const char* reason = locked.reason.load(std::memory_order_acquire);
        if (!reason)
            break;
        const uint32_t count = locked.count.exchange(0, std::memory_order_relaxed);
        if (!count)
            continue;
        line.append("%s%s 0x%08x %u", first ? "" : ", ", reason,
                    unsigned(locked.address.load(std::memory_order_relaxed)), unsigned(count));
        first = false;
    }
    line.append(") | loop unlocked %u %.2f ms, locked ticks %u callbacks %u notifications %u library calls %u",
                unsigned(a[AudioLoopsUnlocked]), msPerFrame(a[AudioLoopUnlockedUs], frames),
                unsigned(a[AudioTicksLocked]), unsigned(a[AudioCallbacksLocked]),
                unsigned(a[AudioNotificationsLocked]), unsigned(a[AudioLibraryLocked]));
    line.append(" | bank code %u runs %.2f ms (%u native %u interpreted) %u instructions %u decodes | output %u"
                " samples saturated %u high %u low", unsigned(a[AudioBankRuns]), msPerFrame(a[AudioBankUs], frames),
                unsigned(a[AudioBankNative]), unsigned(a[AudioBankInterpreted]), unsigned(a[AudioBankInstructions]),
                unsigned(a[AudioBankDecodes]), unsigned(a[AudioOutputSamples]), unsigned(a[AudioSaturatedHigh]),
                unsigned(a[AudioSaturatedLow]));
    logInfo("%s", line.text);
}

// Render thread only (set by init() before that thread starts).
uint32_t s_intervalStart = 0;
uint32_t s_frames = 0;
uint32_t s_drawCalls = 0;
uint32_t s_triangles = 0;
uint32_t s_peakTriangles = 0;
uint32_t s_lastPresent = 0;
uint32_t s_frameMax = 0;

}

void init(bool enabled)
{
    g_enabled = enabled;
    s_intervalStart = now();
    logInfo("perf: timing log %s (timing_log), main thread priority %d affinity 0x%x, clocks arm %d bus %d gpu %d xbar %d MHz",
            enabled ? "on" : "off", sceKernelGetThreadCurrentPriority(),
            sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()),
            scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
            scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    // A profiler maps sampled PCs to the ELF with this (load address minus the symbol's ELF address).
    if (enabled)
        logInfo("perf: pf::perf::init at %p", reinterpret_cast<void*>(&init));
}

uint32_t now()
{
    count(CountClock);
    return uint32_t(sceKernelGetProcessTimeWide());
}

void countSlow(Count counter)
{
    s_counts[counter].fetch_add(1, std::memory_order_relaxed);
}

void startCounting()
{
    g_counting = true;
}

uint32_t takeCount(Count counter)
{
    return s_counts[counter].exchange(0, std::memory_order_relaxed);
}

void add(Counter counter, uint32_t microseconds)
{
    s_counters[counter].fetch_add(microseconds, std::memory_order_relaxed);
}

void preemptWait(uint32_t microseconds)
{
    s_preemptCount.fetch_add(1, std::memory_order_relaxed);
    s_preemptSum.fetch_add(microseconds, std::memory_order_relaxed);
    raiseMax(s_preemptMax, microseconds);
}

void gpuBusy(uint32_t microseconds)
{
    s_gpuBusyCount.fetch_add(1, std::memory_order_relaxed);
    s_gpuBusySum.fetch_add(microseconds, std::memory_order_relaxed);
}

void gameBlocked(Block block, uint32_t microseconds, uint32_t relockUs)
{
    s_blocked[block].fetch_add(microseconds, std::memory_order_relaxed);
    s_blockCount.fetch_add(1, std::memory_order_relaxed);
    s_relock.fetch_add(relockUs, std::memory_order_relaxed);
    s_blockedTotal.fetch_add(microseconds, std::memory_order_relaxed);
}

uint32_t gameBlockedTotal()
{
    return s_blockedTotal.load(std::memory_order_relaxed);
}

void trackThread(const char* name)
{
    const int index = s_trackedCount.fetch_add(1, std::memory_order_relaxed);
    if (index >= TrackedCount)
        return;
    Tracked& thread = s_tracked[index];
    thread.thread = sceKernelGetThreadId();
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    thread.lastClocks = sceKernelGetThreadInfo(thread.thread, &info) < 0 ? 0 : info.runClocks;
    thread.name.store(name, std::memory_order_release);
}

int holderSlot(const char* name)
{
    for (int i = 0; i < HolderSlots; ++i)
    {
        const char* current = s_holders[i].name.load(std::memory_order_acquire);
        if (!current && s_holders[i].name.compare_exchange_strong(current, name, std::memory_order_acq_rel))
            return i;
        if (current == name)
            return i;
    }
    return -1;
}

void held(Holder holder, int slot, uint32_t microseconds)
{
    s_held[holder].fetch_add(microseconds, std::memory_order_relaxed);
    if (slot < 0 || slot >= HolderSlots)
        return;
    HolderStats& stats = s_holders[slot];
    stats.count.fetch_add(1, std::memory_order_relaxed);
    stats.us.fetch_add(microseconds, std::memory_order_relaxed);
    raiseMax(stats.max, microseconds);
}

void upload(Upload counter, uint32_t value)
{
    s_uploads[counter].fetch_add(value, std::memory_order_relaxed);
}

void guestBlock(Alloc kind, uint32_t bytes, uint32_t microseconds, uint32_t searchUs)
{
    upload(UploadGuestBlocks, 1);
    upload(UploadGuestBlockUs, microseconds);
    AllocStats& stats = s_allocs[kind];
    stats.count.fetch_add(1, std::memory_order_relaxed);
    stats.kb.fetch_add((bytes + 1023) / 1024, std::memory_order_relaxed);
    stats.us.fetch_add(microseconds, std::memory_order_relaxed);
    stats.searchUs.fetch_add(searchUs, std::memory_order_relaxed);
}

void audio(Audio counter, uint32_t value)
{
    s_audio[counter].fetch_add(value, std::memory_order_relaxed);
}

// The audio thread only.
void audioLocked(const char* reason, uint32_t address)
{
    for (LockedAudio& locked : s_lockedAudio)
    {
        const char* current = locked.reason.load(std::memory_order_relaxed);
        if (!current)
        {
            locked.address.store(address, std::memory_order_relaxed);
            locked.reason.store(reason, std::memory_order_release);
            current = reason;
        }
        if (current == reason && locked.address.load(std::memory_order_relaxed) == address)
        {
            locked.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void uploadCache(uint32_t bytes, uint32_t levels)
{
    s_cacheBytes.store(bytes, std::memory_order_relaxed);
    s_cacheLevels.store(levels, std::memory_order_relaxed);
}

void timerFired(uint32_t callback, uint32_t microseconds)
{
    for (TimerStats& timer : s_timers)
    {
        uint32_t current = timer.callback.load(std::memory_order_relaxed);
        if (current == 0)
        {
            timer.callback.store(callback, std::memory_order_relaxed);
            current = callback;
        }
        if (current == callback)
        {
            timer.count.fetch_add(1, std::memory_order_relaxed);
            timer.us.fetch_add(microseconds, std::memory_order_relaxed);
            raiseMax(timer.max, microseconds);
            return;
        }
    }
}

bool frameObserved()
{
    return s_frameObserver.load(std::memory_order_relaxed) != nullptr;
}

void framePresented(uint32_t drawCalls, uint32_t triangles)
{
    if (!g_enabled)
    {
        if (FrameObserver observer = s_frameObserver.load(std::memory_order_acquire))
            observer(now());
        return;
    }
    ++s_frames;
    s_drawCalls += drawCalls;
    s_triangles += triangles;
    if (triangles > s_peakTriangles)
        s_peakTriangles = triangles;
    const uint32_t time = now();
    if (s_lastPresent && time - s_lastPresent > s_frameMax)
        s_frameMax = time - s_lastPresent;
    s_lastPresent = time;
    if (FrameObserver observer = s_frameObserver.load(std::memory_order_acquire))
        observer(time);
    const uint32_t elapsed = time - s_intervalStart;
    if (elapsed < LogIntervalUs)
        return;

    uint32_t sums[CounterCount];
    for (int i = 0; i < CounterCount; ++i)
        sums[i] = s_counters[i].exchange(0, std::memory_order_relaxed);
    const uint32_t frames = s_frames;
    const double frame = msPerFrame(elapsed, frames);
    const double wait = msPerFrame(sums[SlotWait], frames);
    const double gil = msPerFrame(sums[GilWait], frames);
    const double game = frame > wait + gil ? frame - wait - gil : 0.0;
    const uint32_t preempts = s_preemptCount.exchange(0, std::memory_order_relaxed);
    const uint32_t preemptSum = s_preemptSum.exchange(0, std::memory_order_relaxed);
    const uint32_t preemptMax = s_preemptMax.exchange(0, std::memory_order_relaxed);
    const uint32_t gpuBusyCount = s_gpuBusyCount.exchange(0, std::memory_order_relaxed);
    const uint32_t gpuBusySum = s_gpuBusySum.exchange(0, std::memory_order_relaxed);
    logInfo("perf: %.1f fps | frame %.1f ms (max %.1f) = game %.1f (preempted %.1f) + wait %.1f (fence %.1f) + gil %.1f"
            " | render %.1f (queue %.1f) | gpu %.1f flip %.1f busy %.1f (%u%%) | %u calls %u tris (peak %u)"
            " | preempt %.0f/s wait %.2f max %.1f ms",
            double(frames) * 1000000.0 / double(elapsed), frame, double(s_frameMax) / 1000.0, game,
            msPerFrame(sums[Preempted], frames), wait,
            msPerFrame(sums[FenceWait], frames), gil,
            msPerFrame(sums[Render], frames), msPerFrame(sums[QueueWait], frames),
            msPerFrame(sums[GpuLatency], frames), msPerFrame(sums[FlipWait], frames),
            gpuBusyCount ? msPerFrame(gpuBusySum, gpuBusyCount) : 0.0, gpuBusyCount * 100 / frames,
            s_drawCalls / frames, s_triangles / frames, s_peakTriangles,
            double(preempts) * 1000000.0 / double(elapsed), preempts ? msPerFrame(preemptSum, preempts) : 0.0,
            double(preemptMax) / 1000.0);
    logBlocking(elapsed, frames);
    logUploads();
    logHolders(elapsed, frames);
    logAudio(frames);
    if (Reporter reporter = s_reporter.load(std::memory_order_acquire))
        reporter(elapsed);
    s_intervalStart = time;
    s_frames = 0;
    s_frameMax = 0;
    s_drawCalls = 0;
    s_triangles = 0;
    s_peakTriangles = 0;
}

void setReporter(Reporter reporter)
{
    s_reporter.store(reporter, std::memory_order_release);
}

void setFrameObserver(FrameObserver observer)
{
    s_frameObserver.store(observer, std::memory_order_release);
}

}}
