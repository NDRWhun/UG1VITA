#include <winapi/winmm.h>
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

MMRESULT timeBeginPeriod(WinApplication* app, x86::CPU& cpu,
                         UINT uPeriod)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(uPeriod);
    return 0;
}

MMRESULT timeEndPeriod(WinApplication* app, x86::CPU& cpu,
                       UINT uPeriod)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(uPeriod);
    return 0;
}

MMRESULT timeGetDevCaps(WinApplication* app, x86::CPU& cpu,
                        LPTIMECAPS ptc, UINT cbtc)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_ASSERT(cbtc == sizeof(*ptc));
    ptc->wPeriodMin = 1;
    ptc->wPeriodMax = 1000;
    return 0;
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
    // TIMERR_NOCANDO (TIMERR_BASE + 1) for an id that is not pending.
    return win32::Timer::kill(uTimerID) ? 0 : 97;
}

MMRESULT timeSetEvent(WinApplication* app, x86::CPU& cpu,
                      UINT uDelay, UINT uResolution, LPTIMECALLBACK lpTimeProc,
                      x86::reg32 dwUser, UINT fuEvent)
{
    // TIME_PERIODIC (1); the callback is always a function (TIME_CALLBACK_FUNCTION).
    RC_USE(uResolution);
    if (cpu.terminate)
        return 0;
    if (fuEvent & ~0x101u)
        pf::logError("timeSetEvent: flags 0x%x not supported", fuEvent);
    return win32::Timer::set(app, uDelay, lpTimeProc, dwUser, (fuEvent & 1) != 0);
}

}}
