#include <winapi/shell32.h>
#include <winapi/support.h>
#include <platform/log.h>

namespace win32 { namespace shell32
{

static const HINSTANCE SE_ERR_NOASSOC = 31;

HINSTANCE ShellExecuteA(WinApplication* app, x86::CPU& cpu, HWND hwnd, LPCSTR lpOperation, LPCSTR lpFile,
                        LPCSTR lpParameters, LPCSTR lpDirectory, int nShowCmd)
{
    RC_USE(hwnd);
    RC_USE(lpParameters);
    RC_USE(lpDirectory);
    RC_USE(nShowCmd);
    pf::logInfo("ShellExecuteA %s %s from 0x%08x: no shell, not opened", lpOperation ? lpOperation : "(default)",
                lpFile ? lpFile : "", callerAddress(app, cpu));
    return SE_ERR_NOASSOC;
}

}}
