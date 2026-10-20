#ifndef LIB_SCHEDULER_H_
#define LIB_SCHEDULER_H_

// Scheduling of the host threads that run guest code under the execution lock: Windows priorities, preemption,
// SuspendThread/ResumeThread and the core each guest thread runs on.

#include <lib/winapp.h>
#include <platform/cores.h>
#include <platform/perf.h>

namespace win32
{

// Per host thread; the thread block's teb::HostControl word points to it while guest code runs.
struct ThreadControl;

namespace scheduler
{

// A thread waiting for the execution lock makes the owner yield at its next preemption point when its level is
// higher, as a higher-priority thread preempts on Windows.
enum Level
{
    LevelNormal,        // THREAD_PRIORITY_NORMAL and below
    LevelAboveNormal,
    LevelHighest,
    LevelTimeCritical,  // also the winmm timer callbacks
    LevelCount
};

Level levelOf(int windowsPriority);
// Host thread priority for a Windows thread priority.
int hostPriorityOf(int windowsPriority);

// Core role and log name of the guest threads started at an entry point; others get pf::cores::Helper. Before
// guest code runs.
void setThreadRole(x86::reg32 entryPoint, pf::cores::Role role, const char* name = nullptr);
pf::cores::Role threadRole(x86::reg32 entryPoint);
const char* threadName(x86::reg32 entryPoint);
// A start routine that calls the real entry point stored in its parameter block at entryOffset, with the
// parameter stored at parameterOffset (the CRT's _beginthreadex thunk). Before guest code runs.
void setStartThunk(x86::reg32 thunk, x86::reg32 entryOffset, x86::reg32 parameterOffset);
// The entry point a CreateThread start routine and parameter lead to.
x86::reg32 resolveEntry(WinApplication* app, x86::reg32 startAddress, x86::reg32 parameter);
// The threads of Speed.exe (roles, names, the CRT thunk).
void configureGameThreads();

// Control of the calling host thread; one that is not a guest thread gets a normal-level control on first use.
ThreadControl* currentControl();
Level level(const ThreadControl* control);
// Kind of thread, for the execution lock accounting of perf_log.
pf::perf::Holder holder(const ThreadControl* control);
// Raises (never lowers) the level of the calling host thread, e.g. for a service thread running callbacks.
void raiseCurrentLevel(Level level);
// SetThreadPriority on the calling thread (GetCurrentThread pseudo handle).
void setCurrentPriority(int windowsPriority);
int currentPriority();
// Windows priority of a guest thread (0 if unknown).
int priority(x86::reg32 threadId);

// SuspendThread / ResumeThread by guest thread id: the previous suspend count, or -1 for an unknown thread. A
// suspended thread stops before it next takes the execution lock; the thread holding the lock calls these, so the
// target never runs guest code while suspended.
int suspend(x86::reg32 threadId);
int resume(x86::reg32 threadId);
// True while any thread is suspended (cheap check for the lock path).
bool anySuspended();
// True when the thread should stop at its lock point.
bool mustPark(const ThreadControl* control);
// Blocks a thread that released the execution lock until it is resumed or cpu terminates.
void park(ThreadControl* control, const x86::CPU& cpu);

// Exit code of a guest thread (GetExitCodeThread): set by ExitThread or by the thread's return value, whichever
// comes first. False while the thread runs.
void setExitCode(x86::reg32 threadId, x86::reg32 code);
bool exitCode(x86::reg32 threadId, x86::reg32* code);

}

}

#endif
