#include <winapi/tapi32.h>
#include <winapi/support.h>
#include <lib/winapp.h>

namespace win32 { namespace tapi32
{

// tapi.h error codes.
static const LONG LINEERR_BADDEVICEID       = LONG(0x80000002);
static const LONG LINEERR_INVALAPPHANDLE    = LONG(0x80000014);
static const LONG LINEERR_INVALCALLHANDLE   = LONG(0x80000018);
static const LONG LINEERR_INVALLINEHANDLE   = LONG(0x8000002B);

// The usage handle lineInitialize returns; there are no line or call handles.
static const DWORD AppHandle = 1;

LONG lineInitialize(WinApplication* app, x86::CPU& cpu, LPDWORD lphLineApp, HINSTANCE hInstance,
                    x86::reg32 lpfnCallback, LPCSTR lpszAppName, LPDWORD lpdwNumDevs)
{
    RC_USE(hInstance);
    RC_USE(lpfnCallback);
    RC_USE(lpszAppName);
    offline(app, cpu, "lineInitialize");
    if (lphLineApp)
        *lphLineApp = AppHandle;
    if (lpdwNumDevs)
        *lpdwNumDevs = 0;
    return 0;
}

LONG lineShutdown(WinApplication* app, x86::CPU& cpu, DWORD hLineApp)
{
    RC_USE(app);
    RC_USE(cpu);
    return hLineApp == AppHandle ? 0 : LINEERR_INVALAPPHANDLE;
}

LONG lineNegotiateAPIVersion(WinApplication* app, x86::CPU& cpu, DWORD hLineApp, DWORD dwDeviceID,
                             DWORD dwAPILowVersion, DWORD dwAPIHighVersion, LPDWORD lpdwAPIVersion,
                             x86::reg32 lpExtensionID)
{
    RC_USE(dwDeviceID);
    RC_USE(dwAPILowVersion);
    RC_USE(dwAPIHighVersion);
    RC_USE(lpdwAPIVersion);
    RC_USE(lpExtensionID);
    offline(app, cpu, "lineNegotiateAPIVersion");
    return hLineApp == AppHandle ? LINEERR_BADDEVICEID : LINEERR_INVALAPPHANDLE;
}

LONG lineGetDevCaps(WinApplication* app, x86::CPU& cpu, DWORD hLineApp, DWORD dwDeviceID, DWORD dwAPIVersion,
                    DWORD dwExtVersion, x86::reg32 lpLineDevCaps)
{
    RC_USE(dwDeviceID);
    RC_USE(dwAPIVersion);
    RC_USE(dwExtVersion);
    RC_USE(lpLineDevCaps);
    offline(app, cpu, "lineGetDevCaps");
    return hLineApp == AppHandle ? LINEERR_BADDEVICEID : LINEERR_INVALAPPHANDLE;
}

LONG lineOpen(WinApplication* app, x86::CPU& cpu, DWORD hLineApp, DWORD dwDeviceID, LPDWORD lphLine,
              DWORD dwAPIVersion, DWORD dwExtVersion, DWORD dwCallbackInstance, DWORD dwPrivileges,
              DWORD dwMediaModes, x86::reg32 lpCallParams)
{
    RC_USE(dwDeviceID);
    RC_USE(lphLine);
    RC_USE(dwAPIVersion);
    RC_USE(dwExtVersion);
    RC_USE(dwCallbackInstance);
    RC_USE(dwPrivileges);
    RC_USE(dwMediaModes);
    RC_USE(lpCallParams);
    offline(app, cpu, "lineOpen");
    return hLineApp == AppHandle ? LINEERR_BADDEVICEID : LINEERR_INVALAPPHANDLE;
}

LONG lineClose(WinApplication* app, x86::CPU& cpu, DWORD hLine)
{
    RC_USE(hLine);
    offline(app, cpu, "lineClose");
    return LINEERR_INVALLINEHANDLE;
}

LONG lineMakeCall(WinApplication* app, x86::CPU& cpu, DWORD hLine, LPDWORD lphCall, LPCSTR lpszDestAddress,
                  DWORD dwCountryCode, x86::reg32 lpCallParams)
{
    RC_USE(hLine);
    RC_USE(lphCall);
    RC_USE(lpszDestAddress);
    RC_USE(dwCountryCode);
    RC_USE(lpCallParams);
    offline(app, cpu, "lineMakeCall");
    return LINEERR_INVALLINEHANDLE;
}

LONG lineAnswer(WinApplication* app, x86::CPU& cpu, DWORD hCall, LPCSTR lpsUserUserInfo, DWORD dwSize)
{
    RC_USE(hCall);
    RC_USE(lpsUserUserInfo);
    RC_USE(dwSize);
    offline(app, cpu, "lineAnswer");
    return LINEERR_INVALCALLHANDLE;
}

LONG lineGetID(WinApplication* app, x86::CPU& cpu, DWORD hLine, DWORD dwAddressID, DWORD hCall, DWORD dwSelect,
               x86::reg32 lpDeviceID, LPCSTR lpszDeviceClass)
{
    RC_USE(hLine);
    RC_USE(dwAddressID);
    RC_USE(hCall);
    RC_USE(dwSelect);
    RC_USE(lpDeviceID);
    RC_USE(lpszDeviceClass);
    offline(app, cpu, "lineGetID");
    return LINEERR_INVALLINEHANDLE;
}

}}
