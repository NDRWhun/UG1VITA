#include <winapi/advapi32.h>
#include <lib/registry.h>
#include <platform/log.h>
#include <x86.h>

namespace win32 { namespace advapi32
{

static const LSTATUS ERROR_SUCCESS = 0;
static const LSTATUS ERROR_FILE_NOT_FOUND = 2;
static const LSTATUS ERROR_INVALID_HANDLE = 6;
static const DWORD REG_CREATED_NEW_KEY = 1;
static const DWORD REG_OPENED_EXISTING_KEY = 2;

// The key of a handle: a predefined root or an opened key.
static registry::Key* keyOf(WinApplication* app, HKEY hKey)
{
    if (registry::Key* root = registry::root(hKey))
        return root;
    RegistryKey* key = dynamic_cast<RegistryKey*>(app->getResource(hKey));
    return key ? key->key() : nullptr;
}

static LSTATUS open(WinApplication* app, HKEY hKey, LPCSTR lpSubKey, bool create, PHKEY phkResult,
                    LPDWORD lpdwDisposition)
{
    registry::Key* parent = keyOf(app, hKey);
    if (!parent || !phkResult)
        return ERROR_INVALID_HANDLE;
    registry::Key* key = registry::find(parent, lpSubKey, false);
    if (lpdwDisposition)
        *lpdwDisposition = key ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
    if (!key && create)
        key = registry::find(parent, lpSubKey, true);
    if (!key)
        return ERROR_FILE_NOT_FOUND;
    *phkResult = app->allocateResource(new RegistryKey(key));
    return ERROR_SUCCESS;
}

LSTATUS RegCloseKey(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey)
{
    RC_USE(cpu);
    if (registry::root(hKey))
        return ERROR_SUCCESS;
    return dynamic_cast<RegistryKey*>(app->getResource(hKey)) && app->freeResource(hKey) ? ERROR_SUCCESS
                                                                                       : ERROR_INVALID_HANDLE;
}

LSTATUS RegCreateKeyExA(WinApplication* app, x86::CPU& cpu,
                        HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions,
                        REGSAM samDesired, LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                        LPDWORD lpdwDisposition)
{
    RC_USE(cpu);
    RC_USE(Reserved);
    RC_USE(lpClass);
    RC_USE(dwOptions);
    RC_USE(samDesired);
    RC_USE(lpSecurityAttributes);
    return open(app, hKey, lpSubKey, true, phkResult, lpdwDisposition);
}

LSTATUS RegFlushKey(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hKey);
    return ERROR_SUCCESS;
}

LSTATUS RegOpenKeyExA(WinApplication* app, x86::CPU& cpu,
                      HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
{
    RC_USE(cpu);
    RC_USE(ulOptions);
    RC_USE(samDesired);
    return open(app, hKey, lpSubKey, false, phkResult, nullptr);
}

// A null name is the key's default value.
LSTATUS RegQueryValueExA(WinApplication* app, x86::CPU& cpu,
                         HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
    RC_USE(cpu);
    RC_USE(lpReserved);
    registry::Key* key = keyOf(app, hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;
    const LSTATUS result = LSTATUS(registry::query(key, lpValueName ? lpValueName : "", lpType, lpData, lpcbData));
    if (result == ERROR_FILE_NOT_FOUND)
        pf::logInfo("RegQueryValueExA: no value %s", lpValueName ? lpValueName : "(default)");
    return result;
}

LSTATUS RegSetValueExA(WinApplication* app, x86::CPU& cpu,
                       HKEY hKey, LPCSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE* lpData,
                       DWORD cbData)
{
    RC_USE(cpu);
    RC_USE(Reserved);
    registry::Key* key = keyOf(app, hKey);
    if (!key)
        return ERROR_INVALID_HANDLE;
    registry::setValue(key, lpValueName ? lpValueName : "", dwType, lpData, lpData ? cbData : 0);
    return ERROR_SUCCESS;
}

}}
