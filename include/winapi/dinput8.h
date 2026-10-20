#ifndef dinput8_H_
#define dinput8_H_

#include <x86.h>
#include <lib/winapp.h>
#include <winapi/types.h>

namespace win32 { namespace dinput8
{

// IDirectInput8A (ANSI) for DirectInput version 0x800; other interfaces fail with DIERR_NOINTERFACE.
HRESULT DirectInput8Create(WinApplication* app, x86::CPU& cpu,
                           HINSTANCE hinst, DWORD dwVersion, const GUID* riidltf, x86::reg32* ppvOut,
                           LPVOID punkOuter);

}}

#endif
