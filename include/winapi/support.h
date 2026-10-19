#ifndef WINAPI_SUPPORT_H_
#define WINAPI_SUPPORT_H_

#include <lib/winapp.h>

// Helpers for shims: the guest caller of an import, traps for calls the port does not implement, and the one-line
// log of a network or modem call that fails because the port is offline.
namespace win32
{

// Return address of the guest call that entered the shim (the word at esp while its arguments are unread).
inline x86::reg32 callerAddress(WinApplication* app, const x86::CPU& cpu)
{
    return app->getMemory<x86::reg32>(cpu.esp);
}

// Logs the API, the details and the guest caller, then traps.
[[noreturn]] void unsupported(WinApplication* app, const x86::CPU& cpu, const char* api, const char* format, ...)
    __attribute__((format(printf, 4, 5)));

// The first call of an offline API logs its name and caller; later calls are silent.
void offline(WinApplication* app, const x86::CPU& cpu, const char* api);

// SetLastError for the calling guest thread.
void setLastError(WinApplication* app, const x86::CPU& cpu, x86::reg32 error);

// Win32 error codes the shims set.
namespace error
{
enum : x86::reg32
{
    Success             = 0,
    FileNotFound        = 2,
    PathNotFound        = 3,
    AccessDenied        = 5,
    InvalidHandle       = 6,
    NoMoreFiles         = 18,
    NotSupported        = 50,
    InvalidParameter    = 87,
    CallNotImplemented  = 120,
    InsufficientBuffer  = 122,
    AlreadyExists       = 183,
    DeviceNotConnected  = 1167,
};
}

}

#endif
