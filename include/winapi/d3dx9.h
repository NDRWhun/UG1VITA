#ifndef WINAPI_D3DX9_H_
#define WINAPI_D3DX9_H_

// Native replacements of the D3DX 9 functions linked into Speed.exe (tools/recompiler NATIVE_OVERRIDES,
// include/native.h): stdcall with the guest ABI, so a native::sub_<address> can call them directly.
//
//   0x005AF9E5  D3DXCreateEffectPool(ppPool)
//   0x005AFB2F  D3DXCreateEffect(device, src, length, defines, include, flags, pool, ppEffect, ppErrors)
//   0x005AC69C  D3DXMatrixMultiply(out, a, b)
//   0x005ABDAF  D3DXVec3Transform(out4, v3, m)
//   0x005AC443  D3DXVec4Transform(out4, v4, m)
//   0x005AC8E5  D3DXMatrixTranspose(out, m)
//
// The effects are the game's seven fx files, recognised by the SHA-256 of their decoded text and drawn with
// the port's programs (shaders/); technique 0 of each is valid, the fixed-function ones are not.

#include <x86.h>

namespace win32
{

class WinApplication;

}

namespace win32 { namespace d3dx9
{

void CreateEffectPool(WinApplication* app, x86::CPU& cpu);
void CreateEffect(WinApplication* app, x86::CPU& cpu);
void MatrixMultiply(WinApplication* app, x86::CPU& cpu);
void Vec3Transform(WinApplication* app, x86::CPU& cpu);
void Vec4Transform(WinApplication* app, x86::CPU& cpu);
void MatrixTranspose(WinApplication* app, x86::CPU& cpu);

// The arithmetic of Vec3Transform and Vec4Transform: r = v * m, m as 16 floats row by row (v[3] = 1 for Vec3).
void transform(const float v[4], const float m[16], float r[4]);

}}

#endif
