#include <winapi/winmm.h>
#include <winapi/support.h>
#include <lib/timer.h>
#include <lib/gamepad.h>
#include <platform/log.h>
#include <x86.h>

namespace win32 { namespace winmm
{

UINT joyGetNumDevs(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return Gamepad::getCount();
}

// System sounds are not played.
BOOL PlaySoundA(WinApplication* app, x86::CPU& cpu,
                x86::reg32 pszSound, HANDLE hmod, DWORD fdwSound)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(pszSound);
    RC_USE(hmod);
    RC_USE(fdwSound);
    return 1;
}

static const MMRESULT TIMERR_NOERROR = 0;
static const MMRESULT TIMERR_NOCANDO = 97;     // TIMERR_BASE + 1
static const UINT PeriodMin = 1;
static const UINT PeriodMax = 1000000;

// Timers always run at 1 ms resolution (the timer service sleeps to the due time), so a period only has to be
// valid: 1 ms up to the maximum timeGetDevCaps reports.
MMRESULT timeBeginPeriod(WinApplication* app, x86::CPU& cpu,
                         UINT uPeriod)
{
    RC_USE(app);
    RC_USE(cpu);
    return uPeriod >= PeriodMin && uPeriod <= PeriodMax ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

MMRESULT timeEndPeriod(WinApplication* app, x86::CPU& cpu,
                       UINT uPeriod)
{
    RC_USE(app);
    RC_USE(cpu);
    return uPeriod >= PeriodMin && uPeriod <= PeriodMax ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

MMRESULT timeGetDevCaps(WinApplication* app, x86::CPU& cpu,
                        LPTIMECAPS ptc, UINT cbtc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!ptc || cbtc != sizeof(*ptc))
        return TIMERR_NOCANDO;
    ptc->wPeriodMin = PeriodMin;
    ptc->wPeriodMax = PeriodMax;
    return TIMERR_NOERROR;
}

// Same clock as GetTickCount.
DWORD timeGetTime(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return timeGetTickCount();
}

MMRESULT timeKillEvent(WinApplication* app, x86::CPU& cpu,
                       UINT uTimerID)
{
    RC_USE(app);
    RC_USE(cpu);
    // TIMERR_NOCANDO for an id that is not pending.
    return win32::Timer::kill(uTimerID) ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

// One-shot (TIME_ONESHOT 0) or periodic (TIME_PERIODIC 1) timers calling a function (TIME_CALLBACK_FUNCTION); the
// game's EA timer re-arms a one-shot timer from its own callback every 10 ms (docs/re/cpu_threads.md section 2.1).
// TIME_KILL_SYNCHRONOUS (0x100) needs nothing more: a callback runs guest code only under the execution lock, which
// the caller of timeKillEvent holds, and a killed timer is not fired again.
// Event callbacks (TIME_CALLBACK_EVENT_SET / _PULSE) are not used by the game and trap.
MMRESULT timeSetEvent(WinApplication* app, x86::CPU& cpu,
                      UINT uDelay, UINT uResolution, LPTIMECALLBACK lpTimeProc,
                      x86::reg32 dwUser, UINT fuEvent)
{
    RC_USE(uResolution);
    if (fuEvent & ~0x101u)
        unsupported(app, cpu, "timeSetEvent", "flags 0x%x", fuEvent);
    if (cpu.terminate || !lpTimeProc || uDelay < PeriodMin || uDelay > PeriodMax)
        return 0;
    return win32::Timer::set(app, uDelay, lpTimeProc, dwUser, (fuEvent & 1) != 0);
}

}}
