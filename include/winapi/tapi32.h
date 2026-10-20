#ifndef tapi32_H_
#define tapi32_H_

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

// Telephony (modem play). The port has no modem: lineInitialize succeeds with no line devices, so the game's TAPI
// thread ends at once; calls on a device or line fail.
namespace win32 { namespace tapi32
{

LONG lineInitialize(WinApplication* app, x86::CPU& cpu, LPDWORD lphLineApp, HINSTANCE hInstance,
                    x86::reg32 lpfnCallback, LPCSTR lpszAppName, LPDWORD lpdwNumDevs);
LONG lineShutdown(WinApplication* app, x86::CPU& cpu, DWORD hLineApp);
LONG lineNegotiateAPIVersion(WinApplication* app, x86::CPU& cpu, DWORD hLineApp, DWORD dwDeviceID,
                             DWORD dwAPILowVersion, DWORD dwAPIHighVersion, LPDWORD lpdwAPIVersion,
                             x86::reg32 lpExtensionID);
LONG lineGetDevCaps(WinApplication* app, x86::CPU& cpu, DWORD hLineApp, DWORD dwDeviceID, DWORD dwAPIVersion,
                    DWORD dwExtVersion, x86::reg32 lpLineDevCaps);
LONG lineOpen(WinApplication* app, x86::CPU& cpu, DWORD hLineApp, DWORD dwDeviceID, LPDWORD lphLine,
              DWORD dwAPIVersion, DWORD dwExtVersion, DWORD dwCallbackInstance, DWORD dwPrivileges,
              DWORD dwMediaModes, x86::reg32 lpCallParams);
LONG lineClose(WinApplication* app, x86::CPU& cpu, DWORD hLine);
LONG lineMakeCall(WinApplication* app, x86::CPU& cpu, DWORD hLine, LPDWORD lphCall, LPCSTR lpszDestAddress,
                  DWORD dwCountryCode, x86::reg32 lpCallParams);
LONG lineAnswer(WinApplication* app, x86::CPU& cpu, DWORD hCall, LPCSTR lpsUserUserInfo, DWORD dwSize);
LONG lineGetID(WinApplication* app, x86::CPU& cpu, DWORD hLine, DWORD dwAddressID, DWORD hCall, DWORD dwSelect,
               x86::reg32 lpDeviceID, LPCSTR lpszDeviceClass);

}}

#endif
