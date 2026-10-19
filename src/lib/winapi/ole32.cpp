#include <winapi/ole32.h>
#include <platform/log.h>
#include <x86.h>
#include <cstdio>

namespace win32 { namespace ole32
{

HRESULT CoCreateGuid(WinApplication* app, x86::CPU& cpu, GUID *pguid)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(pguid);
    pf::logError("CoCreateGuid is not implemented");
    return 0x80004005;  // E_FAIL
}

// "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" in UTF-16; returns the length with the terminator, 0 if it does not fit.
int StringFromGUID2(WinApplication* app, x86::CPU& cpu, const GUID* rguid, LPWSTR lpsz, int cchMax)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!rguid || !lpsz)
        return 0;
    const x86::reg8* b = reinterpret_cast<const x86::reg8*>(rguid);
    char text[40];
    snprintf(text, sizeof(text), "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    const int length = 39;
    if (cchMax < length)
        return 0;
    for (int i = 0; i < length; ++i)
        lpsz[i] = WCHAR(x86::reg8(text[i]));
    return length;
}

}}
