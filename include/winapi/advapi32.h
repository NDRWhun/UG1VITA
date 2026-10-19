#ifndef advapi32_H_
#define advapi32_H_
#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace advapi32
{

typedef x86::reg32  HKEY;

LSTATUS RegCloseKey(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey);
LSTATUS RegCreateKeyExA(WinApplication* app, x86::CPU& cpu,
                        HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions,
                        REGSAM samDesired, LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                        LPDWORD lpdwDisposition);
LSTATUS RegFlushKey(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey);
LSTATUS RegOpenKeyA(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult);
LSTATUS RegOpenKeyExA(WinApplication* app, x86::CPU& cpu,
                      HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult);
LSTATUS RegQueryValueExA(WinApplication* app, x86::CPU& cpu,
                         HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
LSTATUS RegSetValueExA(WinApplication* app, x86::CPU& cpu,
                       HKEY hKey, LPCSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE* lpData,
                       DWORD cbData);

}}

#endif
