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
int g_nextBlock = BlockOther;

namespace
{

const uint32_t LogIntervalUs = 5000000;

std::atomic<uint32_t> s_counters[CounterCount];
std::atomic<Reporter> s_reporter{nullptr};
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
    char text[640] = {};
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

// Render thread only (set by init() before that thread starts).
uint32_t s_intervalStart = 0;
uint32_t s_frames = 0;
uint32_t s_drawCalls = 0;
uint32_t s_triangles = 0;
uint32_t s_peakTriangles = 0;

}

void init(bool enabled)
{
    g_enabled = enabled;
    s_intervalStart = now();
    logInfo("perf: timing log %s (perf_log), main thread priority %d affinity 0x%x, clocks arm %d bus %d gpu %d xbar %d MHz",
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
    return uint32_t(sceKernelGetProcessTimeWide());
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

void held(Holder holder, uint32_t microseconds)
{
    s_held[holder].fetch_add(microseconds, std::memory_order_relaxed);
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

void framePresented(uint32_t drawCalls, uint32_t triangles)
{
    ++s_frames;
    s_drawCalls += drawCalls;
    s_triangles += triangles;
    if (triangles > s_peakTriangles)
        s_peakTriangles = triangles;
    const uint32_t time = now();
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
    logInfo("perf: %.1f fps | frame %.1f ms = game %.1f (preempted %.1f) + wait %.1f (fence %.1f) + gil %.1f"
            " | render %.1f (queue %.1f) | gpu %.1f flip %.1f busy %.1f (%u%%) | %u calls %u tris (peak %u)"
            " | preempt %.0f/s wait %.2f max %.1f ms",
            double(frames) * 1000000.0 / double(elapsed), frame, game, msPerFrame(sums[Preempted], frames), wait,
            msPerFrame(sums[FenceWait], frames), gil,
            msPerFrame(sums[Render], frames), msPerFrame(sums[QueueWait], frames),
            msPerFrame(sums[GpuLatency], frames), msPerFrame(sums[FlipWait], frames),
            gpuBusyCount ? msPerFrame(gpuBusySum, gpuBusyCount) : 0.0, gpuBusyCount * 100 / frames,
            s_drawCalls / frames, s_triangles / frames, s_peakTriangles,
            double(preempts) * 1000000.0 / double(elapsed), preempts ? msPerFrame(preemptSum, preempts) : 0.0,
            double(preemptMax) / 1000.0);
    logBlocking(elapsed, frames);
    if (Reporter reporter = s_reporter.load(std::memory_order_acquire))
        reporter(elapsed);
    s_intervalStart = time;
    s_frames = 0;
    s_drawCalls = 0;
    s_triangles = 0;
    s_peakTriangles = 0;
}

void setReporter(Reporter reporter)
{
    s_reporter.store(reporter, std::memory_order_release);
}

}}
