#include <winapi/support.h>
#include <lib/teb.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <string>

namespace win32
{

void unsupported(WinApplication* app, const x86::CPU& cpu, const char* api, const char* format, ...)
{
    char details[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(details, sizeof(details), format, arguments);
    va_end(arguments);
    const x86::reg32 caller = callerAddress(app, cpu);
    pf::logError("%s: not supported by the port (%s), called from 0x%08x", api, details, caller);
    x86::guestTrap(caller, api);
}

void offline(WinApplication* app, const x86::CPU& cpu, const char* api)
{
    static pf::LwMutex s_mutex("offline_log");
    static std::set<std::string> s_logged;
    {
        pf::LwLockGuard guard(s_mutex);
        if (!s_logged.insert(api).second)
            return;
    }
    pf::logInfo("%s: offline port, the call fails (first call from 0x%08x)", api, callerAddress(app, cpu));
}

void setLastError(WinApplication* app, const x86::CPU& cpu, x86::reg32 error)
{
    app->getMemory<x86::reg32>(cpu.efs + teb::LastError) = error;
}

}
