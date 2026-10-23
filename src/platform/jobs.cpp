#include <platform/jobs.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <psp2/kernel/processmgr.h>
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace pf { namespace jobs
{

namespace
{

// 76: below the audio output and render threads (64) and the timer and guest threads at
// THREAD_PRIORITY_TIME_CRITICAL (72), above the other guest threads (80 to 124).
const int WorkerPriority = Thread::PriorityHigh + 12;
const uint32_t WorkerStack = 64 * 1024;
const int MaxWorkers = 2;
// The caller spins this long for workers still in their last batch before it sleeps.
const uint32_t JoinSpinUs = 50;

struct Phase
{
    Batch batch;
    void* context;
    int count;
    int batchSize;
    uint32_t fpscr;
    std::atomic<int> next;      // next batch index to claim
    std::atomic<int> batches;   // batches run
};

Phase s_phase;
// The open phase; null between phases and while the caller closes one.
std::atomic<Phase*> s_current{nullptr};
// Workers between waking and leaving a phase.
std::atomic<int> s_busy{0};
// 1 while the caller sleeps on s_done; the worker that ends the last batch takes it and posts.
std::atomic<int> s_waiter{0};
Semaphore* s_wake = nullptr;
Semaphore* s_done = nullptr;
Thread* s_threads[MaxWorkers] = {};
int s_workers = 0;
bool s_started = false;

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

void claim(Phase& phase, int thread)
{
    for (;;)
    {
        const int index = phase.next.fetch_add(1, std::memory_order_relaxed);
        const int begin = index * phase.batchSize;
        if (begin >= phase.count)
            return;
        phase.batch(phase.context, begin, std::min(phase.count, begin + phase.batchSize), thread);
        phase.batches.fetch_add(1, std::memory_order_relaxed);
    }
}

int workerEntry(void* argument)
{
    const int thread = int(reinterpret_cast<intptr_t>(argument));
    for (;;)
    {
        s_wake->wait();
        // Counted before the phase is read.
        s_busy.fetch_add(1, std::memory_order_seq_cst);
        if (Phase* phase = s_current.load(std::memory_order_seq_cst))
        {
            if (readFpscr() != phase->fpscr)
                writeFpscr(phase->fpscr);
            claim(*phase, thread);
        }
        if (s_busy.fetch_sub(1, std::memory_order_seq_cst) == 1 && s_waiter.exchange(0) == 1)
            s_done->post();
    }
    return 0;
}

void start()
{
    s_started = true;
    const int cores[MaxWorkers] = { 2, 3 };
    const int count = cores::hasCore3() ? 2 : 1;
    s_wake = new Semaphore("jobs_wake", 0);
    s_done = new Semaphore("jobs_done", 0);
    static const char* const names[MaxWorkers] = { "jobs_core2", "jobs_core3" };
    for (int i = 0; i < count; ++i)
    {
        s_threads[i] = new Thread(names[i], &workerEntry, reinterpret_cast<void*>(intptr_t(i + 1)), WorkerPriority,
                                  WorkerStack, Thread::Cpu0 << cores[i]);
        s_threads[i]->start();
    }
    s_workers = count;
    logInfo("jobs: %d worker(s) on core%s", count, count == 2 ? "s 2 and 3" : " 2");
}

void join()
{
    s_current.store(nullptr, std::memory_order_seq_cst);
    const uint32_t start = sceKernelGetProcessTimeLow();
    while (s_busy.load(std::memory_order_acquire) != 0)
    {
        if (sceKernelGetProcessTimeLow() - start < JoinSpinUs)
            continue;
        s_waiter.store(1, std::memory_order_seq_cst);
        // Waits for the post unless no worker is busy and the flag is taken back here.
        if (s_busy.load(std::memory_order_seq_cst) != 0 || s_waiter.exchange(0) == 0)
            s_done->wait();
        break;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
}

}

int threads()
{
    if (!s_started)
        start();
    return 1 + s_workers;
}

Stats run(int count, int batchSize, Batch batch, void* context)
{
    if (!s_started)
        start();
    batchSize = std::max(1, batchSize);
    if (count <= 0)
        return Stats{0, false};
    if (s_workers == 0 || count < 2 * batchSize)
    {
        batch(context, 0, count, 0);
        return Stats{1, false};
    }
    Phase& phase = s_phase;
    phase.batch = batch;
    phase.context = context;
    phase.count = count;
    phase.batchSize = batchSize;
    phase.fpscr = readFpscr();
    phase.next.store(0, std::memory_order_relaxed);
    phase.batches.store(0, std::memory_order_relaxed);
    s_current.store(&phase, std::memory_order_seq_cst);
    s_wake->post(s_workers);
    claim(phase, 0);
    join();
    return Stats{phase.batches.load(std::memory_order_relaxed), true};
}

}}
