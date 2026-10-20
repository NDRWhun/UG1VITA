#ifndef WINAPI_D3D9_H_
#define WINAPI_D3D9_H_

// d3d9.dll: Direct3D 9 over the frame model of platform/render.h (src/lib/winapi/d3d9*.cpp).

#include <x86.h>
#include <winapi/types.h>

namespace win32
{

class WinApplication;

}

namespace win32 { namespace d3d9
{

// The IDirect3D9 object, or 0 for an SDK version other than 31 (D3D_SDK_VERSION of the 9.0 runtime).
x86::reg32 Direct3DCreate9(WinApplication* app, x86::CPU& cpu, UINT SDKVersion);

}}

#endif
