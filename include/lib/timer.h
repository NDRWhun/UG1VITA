#ifndef LIB_TIMER_H_
#define LIB_TIMER_H_

#include <lib/winapp.h>
#include <winapi/types.h>
#include <cstdint>

namespace win32
{

void getSystemTime(SYSTEMTIME* systemTime);
x86::reg32 timeGetTickCount();

// Multimedia timers (timeSetEvent), one-shot or periodic; all callbacks run on one service thread in due order, so
// a timer's callbacks never overlap. A fired one-shot or killed timer id is invalid, as on Windows. The service
// thread runs on the audio core at time-critical priority: its guest callbacks preempt every guest thread.
class Timer
{
public:
    // Host replacement for a guest timer callback; runs on the timer thread without the execution lock. It may run
    // the guest callback itself with runGuest.
    typedef void (*NativeCallback)(WinApplication* app, x86::reg32 callback, x86::reg32 id, x86::reg32 parameter);

    // Returns the timer id, 0 after shutdown. A guest callback runs as a preempting guest thread and gets the
    // TimeProc arguments (id, 0, parameter, 0, 0).
    static x86::reg32 set(WinApplication* app, x86::reg32 delayMs, x86::reg32 callback, x86::reg32 parameter,
                          bool periodic);
    // False if the id is not pending or firing (fired one-shot, killed or unknown).
    static bool kill(x86::reg32 id);
    // Timers set with this guest callback run the native one instead. Before guest code runs.
    static void setNativeCallback(x86::reg32 callback, NativeCallback native);
    // Runs a guest TimeProc on the timer thread with the arguments (id, 0, parameter, 0, 0).
    static void runGuest(WinApplication* app, x86::reg32 callback, x86::reg32 id, x86::reg32 parameter);

    static void init();
    static void fini();
};

}

#endif
