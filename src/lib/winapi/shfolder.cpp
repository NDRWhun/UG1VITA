#include <winapi/shfolder.h>
#include <winapi/support.h>
#include <lib/file.h>
#include <platform/log.h>
#include <cstring>
#include <sys/stat.h>

namespace win32 { namespace shfolder
{

const char* const SaveRoot = "C:\\save";

static const int CSIDL_PERSONAL = 0x05;
static const int CSIDL_APPDATA = 0x1a;
static const int CSIDL_LOCAL_APPDATA = 0x1c;
static const int CSIDL_COMMON_APPDATA = 0x23;
static const int CSIDL_FLAG_MASK = 0xff00;
static const HRESULT S_OK = 0;
static const HRESULT E_FAIL = 0x80004005;
static const HRESULT E_INVALIDARG = 0x80070057;

HRESULT SHGetFolderPathA(WinApplication* app, x86::CPU& cpu, HWND hwndOwner, int nFolder, HANDLE hToken,
                         DWORD dwFlags, LPSTR pszPath)
{
    RC_USE(hwndOwner);
    RC_USE(hToken);
    RC_USE(dwFlags);
    if (!pszPath)
        return E_INVALIDARG;
    pszPath[0] = 0;
    const int folder = nFolder & ~CSIDL_FLAG_MASK;
    if (folder != CSIDL_COMMON_APPDATA && folder != CSIDL_APPDATA && folder != CSIDL_LOCAL_APPDATA
        && folder != CSIDL_PERSONAL)
    {
        pf::logError("SHGetFolderPathA: folder 0x%x has no Vita location (from 0x%08x)", unsigned(nFolder),
                     callerAddress(app, cpu));
        return E_FAIL;
    }
    // The folder always exists: it is made on first use, with or without CSIDL_FLAG_CREATE.
    const std::string host = File::hostPath(SaveRoot);
    struct stat status;
    if (stat(host.c_str(), &status) != 0 && mkdir(host.c_str(), 0777) != 0)
    {
        pf::logError("SHGetFolderPathA: cannot create %s", host.c_str());
        return E_FAIL;
    }
    strcpy(pszPath, SaveRoot);
    return S_OK;
}

}}
