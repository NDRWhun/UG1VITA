// The scene build's worker thread (scenethread.h).

#include "scenethread.h"

#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace nfsu
{

void sub_566e60(win32::WinApplication* app, x86::CPU& cpu);

namespace scenethread
{

namespace
{

using win32::WinApplication;
using x86::reg32;
using x86::sreg32;

typedef void (*Function)(WinApplication*, x86::CPU&);

// 70: below the audio output (64), above the timer and the guest threads at THREAD_PRIORITY_TIME_CRITICAL (72), the
// job workers (76) and the GXM command list workers (80) of core 2. Those wait for the execution lock the views hold
// or for the views' job phases anyway; below them the views took twice their CPU time in wall time.
const int DefaultPriority = pf::Thread::PriorityHigh + 6;
// As a guest thread's host stack: the views and the guest functions they call nest as deep there as on the game
// thread.
const uint32_t StackBytes = 1024 * 1024;
// The game thread logs a worker that has not returned for this long (once).
const uint32_t SlowUs = 2000000;
// Spin iterations between two clock reads of the game thread's wait.
const uint32_t ClockMask = 1023;
// The worker spins this long for a forwarded operation, then sleeps: the operation may wait for a thread of lower
// priority on the worker's core (a lock holder, the thread a preemption point gives the lock to).
const uint32_t ReplySpinUs = 30;
// Views longer than this on the worker are counted with the time their forwarded operations took.
const uint32_t SlowViewsUs = 25000;

struct Settings
{
    bool on;
    int core;
    int priority;
};

const Settings& settings()
{
    static const Settings s = []
    {
        Settings r = { pf::Config::getBool("scene_thread", false)
                           || pf::Config::getBool("shadow_scene_thread", false),
                       pf::Config::getInt("scene_thread_core", 2),
                       pf::Config::getInt("scene_thread_priority", DefaultPriority) };
        if (r.core < 1 || r.core > 3 || (r.core == 3 && !pf::cores::hasCore3()))
            r.core = 2;
        if (r.priority < pf::Thread::PriorityHigh || r.priority > pf::Thread::PriorityLow)
            r.priority = DefaultPriority;
        if (r.on)
            pf::logInfo("scene_thread: the world views on a worker thread on core %d, priority %d; the game thread "
                        "waits for them", r.core, r.priority);
        return r;
    }();
    return s;
}

enum Message
{
    MessageNone,
    MessageDone,        // the views returned
    MessageOperation    // a lock operation to run (Worker::op)
};

struct Worker
{
    pf::Thread* thread = nullptr;
    pf::Semaphore* go = nullptr;        // to the worker: run the views
    std::atomic<int> id{0};             // host thread id of the worker
    // The game thread spins for the messages and the worker for the replies: the game thread has nothing else to
    // run until the views return, and a forwarded operation is two semaphore wake-ups shorter.
    std::atomic<int> message{MessageNone};
    std::atomic<int> replied{0};        // the forwarded operation ran
    std::atomic<int> sleeping{0};       // 1 while the worker sleeps on reply; the game thread takes it and posts
    pf::Semaphore* reply = nullptr;
    WinApplication::LockOp op = WinApplication::LockOpLock;

    WinApplication* app = nullptr;
    x86::CPU* cpu = nullptr;
    Function function = nullptr;
    uint32_t fpscr = 0;
    uint32_t postedAt = 0, startedAt = 0, doneAt = 0;
    uint32_t forwardUs = 0;             // this frame's wait for forwarded operations
};

Worker s_worker;

struct Stats
{
    uint32_t frames = 0;
    uint64_t wakeUs = 0, joinUs = 0, runUs = 0;
    uint32_t maxWakeUs = 0, maxJoinUs = 0, maxRunUs = 0;
    uint32_t operations[3] = {};
    uint32_t criticalSections = 0, criticalSectionWaits = 0;    // entered by the views (counted with timing_phases)
    uint64_t forwardUs = 0;         // the worker's wait for its forwarded operations
    uint32_t maxForwardUs = 0;      // the most in one frame
    uint32_t slowFrames = 0;        // views over SlowViewsUs on the worker
    uint64_t slowForwardUs = 0;     // the forwarded operations' share of those
    bool slowLogged = false;
};

Stats s_stats;


// The draw queue's slot pools (SlotPool: free list +0x10, flags +0x14 with 1 = may expand, used +0x18, used maximum
// +0x1c, slots of the first block +0x24, slots in all blocks +0x2c): node, packet and vertex pool.
const reg32 PoolPointers[] = { 0x7363d4, 0x7363d8, 0x7363b8 };
const char* const PoolNames[] = { "node", "packet", "vertex" };
const uint32_t PoolCount = 3;
// Free slots each pool has at the handoff before the views' use of it is measured.
const uint32_t InitialNeed[PoolCount] = { 512, 2048, 176 };

struct Pool
{
    reg32 pool;
    reg32 savedMaximum;
    reg32 used;
    reg32 total;
};

struct PoolStats
{
    uint32_t need;
    uint32_t peak;                  // most slots the views took in one frame
    uint32_t handoffExpansions;     // expansions run by the game thread at the handoff
    uint32_t workerExpansions;      // expansions the views still ran
};

Pool s_pools[PoolCount];
PoolStats s_poolStats[PoolCount] = {
    { InitialNeed[0], 0, 0, 0 }, { InitialNeed[1], 0, 0, 0 }, { InitialNeed[2], 0, 0, 0 } };

// ExpandSlotPool 0x566e60 (esi = the pool, eax = slots) with the slot count 0x567160 passes: a new block, its slots in
// front of the free list.
void expandPool(WinApplication* app, x86::CPU& cpu, reg32 pool)
{
    const sreg32 first = sreg32(rc::ld32(pool + 0x24));
    const x86::CPU saved = cpu;
    cpu.esi = pool;
    cpu.eax = reg32(((first + ((first >> 31) & 3)) >> 2) + 1);
    cpu.esp = saved.esp - 4;
    sub_566e60(app, cpu);
    if (!cpu.terminate)
        cpu = saved;
}

// At the handoff: every pool gets the free slots the views may take, so the worker never expands one; the used
// maximum starts at the used count, so the join reads the views' peak from it.
bool fillPools(WinApplication* app, x86::CPU& cpu)
{
    for (uint32_t i = 0; i < PoolCount; ++i)
    {
        Pool& p = s_pools[i];
        p.pool = rc::ld32(PoolPointers[i]);
        if (p.pool == 0)
            continue;
        p.used = rc::ld32(p.pool + 0x18);
        p.total = rc::ld32(p.pool + 0x2c);
        if ((rc::ld8(p.pool + 0x14) & 1) != 0)
            while (sreg32(p.total - p.used) < sreg32(s_poolStats[i].need))
            {
                expandPool(app, cpu, p.pool);
                if (cpu.terminate)
                    return false;
                const reg32 total = rc::ld32(p.pool + 0x2c);
                if (total == p.total)
                    break;
                p.total = total;
                ++s_poolStats[i].handoffExpansions;
            }
        p.savedMaximum = rc::ld32(p.pool + 0x1c);
        rc::st32(p.pool + 0x1c, p.used);
    }
    return true;
}

// At the join: the used maximum as the views would have left it (the larger of the saved one and the views' peak),
// and the views' use and expansions counted.
void readPools()
{
    for (uint32_t i = 0; i < PoolCount; ++i)
    {
        const Pool& p = s_pools[i];
        if (p.pool == 0 || rc::ld32(PoolPointers[i]) != p.pool)
            continue;
        const reg32 peak = rc::ld32(p.pool + 0x1c);
        if (sreg32(p.savedMaximum) > sreg32(peak))
            rc::st32(p.pool + 0x1c, p.savedMaximum);
        PoolStats& s = s_poolStats[i];
        const uint32_t took = sreg32(peak - p.used) > 0 ? peak - p.used : 0;
        s.peak = std::max(s.peak, took);
        s.need = std::max(s.need, took + took / 4);
        if (rc::ld32(p.pool + 0x2c) != p.total)
            ++s.workerExpansions;
    }
}

inline uint32_t readFpscr()
{
    uint32_t value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
}

inline void writeFpscr(uint32_t value)
{
    __asm__ volatile("vmsr fpscr, %0" : : "r"(value) : "memory");
}

int entry(void*)
{
    Worker& w = s_worker;
    w.id.store(pf::Thread::currentId(), std::memory_order_release);
    pf::perf::trackThread("scene");
    for (;;)
    {
        w.go->wait();
        w.startedAt = pf::perf::now();
        if (readFpscr() != w.fpscr)
            writeFpscr(w.fpscr);
        w.function(w.app, *w.cpu);
        w.doneAt = pf::perf::now();
        w.message.store(MessageDone, std::memory_order_release);
    }
    return 0;
}

void start()
{
    Worker& w = s_worker;
    w.go = new pf::Semaphore("scene_go", 0);
    w.reply = new pf::Semaphore("scene_reply", 0);
    w.thread = new pf::Thread("scene", &entry, nullptr, settings().priority, StackBytes, pf::Thread::Cpu0 << settings().core);
    w.thread->start();
}

// The worker's lock operations: run by the game thread, which waits in run().
bool forward(WinApplication::LockOp op, const x86::CPU& cpu)
{
    (void)cpu;
    Worker& w = s_worker;
    if (pf::Thread::currentId() != w.id.load(std::memory_order_acquire))
        return false;
    w.op = op;
    ++s_stats.operations[op];
    w.message.store(MessageOperation, std::memory_order_release);
    const uint32_t start = pf::perf::now();
    for (uint32_t spin = 1; w.replied.load(std::memory_order_acquire) == 0; ++spin)
    {
        if ((spin & 15) != 0 || pf::perf::now() - start < ReplySpinUs)
            continue;
        w.sleeping.store(1, std::memory_order_seq_cst);
        // Sleeps unless the reply came and the flag is taken back here.
        if (w.replied.load(std::memory_order_seq_cst) == 0 || w.sleeping.exchange(0) == 0)
            w.reply->wait();
        break;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    w.replied.store(0, std::memory_order_relaxed);
    w.forwardUs += pf::perf::now() - start;
    return true;
}

}

bool on()
{
    return settings().on;
}

void run(WinApplication* app, x86::CPU& cpu, Function function)
{
    if (!settings().on)
    {
        function(app, cpu);
        return;
    }
    if (!fillPools(app, cpu))
        return;
    Worker& w = s_worker;
    if (!w.thread)
        start();
    w.app = app;
    w.cpu = &cpu;
    w.function = function;
    w.fpscr = readFpscr();
    w.forwardUs = 0;
    // Only the views run guest code until they return (the lock is held here), so the counts they add are theirs.
    const uint32_t sections = pf::perf::peekCount(pf::perf::CountCritSec);
    const uint32_t sectionWaits = pf::perf::peekCount(pf::perf::CountCritSecWait);
    app->setLockForward(&forward);
    w.postedAt = pf::perf::now();
    w.go->post();
    for (uint32_t spin = 1;; ++spin)
    {
        const int message = w.message.exchange(MessageNone, std::memory_order_acquire);
        if (message == MessageNone)
        {
            if ((spin & ClockMask) == 0 && !s_stats.slowLogged && pf::perf::now() - w.postedAt >= SlowUs)
            {
                s_stats.slowLogged = true;
                pf::logInfo("scene_thread: the views have not returned for %u ms (lock operations so far: lock %u, "
                            "unlock %u, preemption points %u)", unsigned(SlowUs / 1000),
                            unsigned(s_stats.operations[WinApplication::LockOpLock]),
                            unsigned(s_stats.operations[WinApplication::LockOpUnlock]),
                            unsigned(s_stats.operations[WinApplication::LockOpYield]));
            }
            continue;
        }
        if (message == MessageDone)
            break;
        switch (w.op)
        {
        case WinApplication::LockOpLock:
            app->lockContext(cpu);
            break;
        case WinApplication::LockOpUnlock:
            app->unlockContext(cpu);
            break;
        case WinApplication::LockOpYield:
            app->allowPreemption(cpu);
            break;
        }
        w.replied.store(1, std::memory_order_seq_cst);
        if (w.sleeping.exchange(0) == 1)
            w.reply->post();
    }
    const uint32_t resumedAt = pf::perf::now();
    app->setLockForward(nullptr);
    readPools();

    Stats& s = s_stats;
    const uint32_t wake = w.startedAt - w.postedAt, join = resumedAt - w.doneAt, ran = w.doneAt - w.startedAt;
    ++s.frames;
    s.criticalSections += pf::perf::peekCount(pf::perf::CountCritSec) - sections;
    s.criticalSectionWaits += pf::perf::peekCount(pf::perf::CountCritSecWait) - sectionWaits;
    s.wakeUs += wake;
    s.joinUs += join;
    s.runUs += ran;
    s.maxWakeUs = std::max(s.maxWakeUs, wake);
    s.maxJoinUs = std::max(s.maxJoinUs, join);
    s.maxRunUs = std::max(s.maxRunUs, ran);
    s.forwardUs += w.forwardUs;
    s.maxForwardUs = std::max(s.maxForwardUs, w.forwardUs);
    if (ran > SlowViewsUs)
    {
        ++s.slowFrames;
        s.slowForwardUs += w.forwardUs;
    }
}

void logTotal()
{
    if (!settings().on)
        return;
    const Stats& s = s_stats;
    const double f = s.frames ? double(s.frames) : 1.0;
    pf::logInfo("scene_thread: %u views on core %d; wake-up %.1f us avg (max %u), views %.2f ms avg (max %.2f), join "
                "%.1f us avg (max %u); lock operations run by the game thread: lock %u, unlock %u, preemption points %u;"
                " critical sections entered %u (%u blocking; counted with timing_phases=on)",
                unsigned(s.frames), settings().core, double(s.wakeUs) / f, unsigned(s.maxWakeUs),
                double(s.runUs) / 1000.0 / f, double(s.maxRunUs) / 1000.0, double(s.joinUs) / f,
                unsigned(s.maxJoinUs), unsigned(s.operations[WinApplication::LockOpLock]),
                unsigned(s.operations[WinApplication::LockOpUnlock]),
                unsigned(s.operations[WinApplication::LockOpYield]), unsigned(s.criticalSections),
                unsigned(s.criticalSectionWaits));
    pf::logInfo("scene_thread: forwarded operations took %.1f us per frame (max %u); views over %u ms %u, their "
                "forwarded operations %.2f ms avg", double(s.forwardUs) / f, unsigned(s.maxForwardUs),
                unsigned(SlowViewsUs / 1000), unsigned(s.slowFrames),
                double(s.slowForwardUs) / 1000.0 / (s.slowFrames ? double(s.slowFrames) : 1.0));
    char text[512];
    int at = 0;
    for (uint32_t i = 0; i < PoolCount && at < int(sizeof(text)); ++i)
    {
        const PoolStats& p = s_poolStats[i];
        at += snprintf(text + at, sizeof(text) - at, "%s%s: views took up to %u, kept free %u, expanded at the handoff "
                       "%u, by the views %u", i ? "; " : "", PoolNames[i], unsigned(p.peak), unsigned(p.need),
                       unsigned(p.handoffExpansions), unsigned(p.workerExpansions));
    }
    pf::logInfo("scene_thread: slot pools: %s", text);
}

}}
