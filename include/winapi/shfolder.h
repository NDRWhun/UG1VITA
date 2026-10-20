#ifndef shfolder_H_
#define shfolder_H_

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace shfolder
{

// Shell folders. CSIDL_COMMON_APPDATA, CSIDL_APPDATA and CSIDL_PERSONAL are the guest folder C:\save (host
// <data>/save), where the game keeps its "NFS Underground" save folder; other folders are not found.
HRESULT SHGetFolderPathA(WinApplication* app, x86::CPU& cpu, HWND hwndOwner, int nFolder, HANDLE hToken,
                         DWORD dwFlags, LPSTR pszPath);

// Guest path of the save root.
extern const char* const SaveRoot;

}}

#endif
