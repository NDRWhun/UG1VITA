#ifndef PLATFORM_JOBS_H_
#define PLATFORM_JOBS_H_

// A small pool of worker threads for data-parallel native phases of the game thread. The workers sit on core 2,
// and on core 3 with core3_layout=1 (cores.h); with core3_layout=0 the core 3 worker runs posted tasks only. A phase splits [0, count) into batches that the calling thread
// and the workers claim with one atomic counter; results go to per-item slots, so they do not depend on which
// thread ran an item or on how many workers there are.
//
// Only one thread may run phases (the game thread, holding the execution lock). Batches must not run guest code
// or write guest memory; they read guest memory the caller does not change before run() returns.
//
// Tasks are posted and run later on a worker, between phases, at a priority below the phases and the render
// thread's command list workers. They start in the order they were posted.

namespace pf { namespace jobs
{

// Runs items [begin, end) of a phase. thread is 0 for the calling thread and 1..threads()-1 for the workers.
typedef void (*Batch)(void* context, int begin, int end, int thread);

// Phase statistics, for perf logs.
struct Stats
{
    int batches;    // batches run (1 when inline)
    bool forked;    // false when run inline on the calling thread
};

// Threads that can run batches, the caller included (the workers start at the first run()).
int threads();

// Splits [0, count) into batches of batchSize items and returns when all have run. With fewer than 2 batches,
// or no workers, the caller runs everything inline. Workers run with the caller's FPSCR.
Stats run(int count, int batchSize, Batch batch, void* context);

typedef void (*Task)(void* context);

// Queues task(context) for a worker; returns at once. Tasks must not run guest code or write guest memory.
void post(Task task, void* context);

}}

#endif
