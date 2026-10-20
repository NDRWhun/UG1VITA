// Native replacements of guest functions (tools/recompiler/generate.py NATIVE_OVERRIDES). Each one does what the
// guest function does, including its ret: on entry cpu.esp points at the return address slot.

#include "cull.h"
#include "movie_yuv.h"

#include <lib/winapp.h>

#include <cmath>
#include <fpu.h>
#include <native.h>
#include <recomp.h>
#include <winapi/d3dx9.h>

namespace nfsu
{

// The translated guest body of an overridden function (generated).
void sub_647cb0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

// Stack argument n (0 = the first) of a cdecl function.
inline x86::reg32 argument(x86::CPU& cpu, int n)
{
    return rc::ld32(cpu.esp + 4 + 4 * n);
}

// st0 to a 64-bit integer, truncating toward zero; NaN and values out of range give the integer indefinite.
void floatToInteger(x86::CPU& cpu)
{
    const double value = double(cpu.fpu.pop());
    cpu.edx_eax = x86::reg64(x86::fistInteger<x86::sreg64>(std::trunc(value)));
    cpu.esp += 4;
}

}

// _ftol: fistp with the rounding control set to truncate, then the caller's control word again.
void sub_672fc8(win32::WinApplication*, x86::CPU& cpu)
{
    floatToInteger(cpu);
}

// _ftol2: rounds to nearest and corrects toward zero by the sign of the difference, which is truncation.
void sub_674898(win32::WinApplication*, x86::CPU& cpu)
{
    floatToInteger(cpu);
}

// _chkstk: moves esp down by eax bytes below the return address and returns there with eax = the dword in the
// return address slot (the guest pushes it again before its ret). The guest touches each page on the way; guest
// stacks here are committed memory.
void sub_6734a0(win32::WinApplication*, x86::CPU& cpu)
{
    const x86::reg32 slot = cpu.esp;
    const x86::reg32 size = cpu.eax;
    cpu.eax = rc::ld32(slot);
    cpu.esp = slot + 4 - size;
}

// Movie YUV420 -> 32-bit converter (Y, U, V, Y stride, width, height, dst, dst pitch, mode). The guest returns 0
// at once when the EA MMX flag [0x70FDD8] is clear and has only MMX kernels; mode 1 (A8R8G8B8, the only mode the
// game uses, 0x0041325D) runs natively whatever the flag says. Other modes keep the guest code.
void sub_647cb0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (argument(cpu, 8) != 1)
    {
        sub_647cb0_guest(app, cpu);
        return;
    }
    static const movie::Tables tables = {0x6e8130, 0x6e8930, 0x6e9130, 0x712018};
    cpu.eax = movie::convertFrame(win32::g_guestBase, tables, argument(cpu, 0), argument(cpu, 1), argument(cpu, 2),
                                  x86::sreg32(argument(cpu, 3)), x86::sreg32(argument(cpu, 4)),
                                  x86::sreg32(argument(cpu, 5)), argument(cpu, 6), x86::sreg32(argument(cpu, 7)));
    cpu.esp += 4;
}

// D3DX: the port's effect layer and matrix helpers (src/lib/winapi/d3dx9), stdcall like the guest's.
void sub_5af9e5(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::CreateEffectPool(app, cpu);
}

void sub_5afb2f(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::CreateEffect(app, cpu);
}

void sub_5ac69c(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::MatrixMultiply(app, cpu);
}

void sub_5abdaf(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::Vec3Transform(app, cpu);
}

void sub_5ac443(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::Vec4Transform(app, cpu);
}

void sub_5ac8e5(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::d3dx9::MatrixTranspose(app, cpu);
}

// Scenery culler and box against frustum test (cull.cpp).
void sub_446620(win32::WinApplication* app, x86::CPU& cpu)
{
    cull::doCulling(app, cpu);
}

void sub_40db20(win32::WinApplication* app, x86::CPU& cpu)
{
    cull::getVisibleState(app, cpu);
}

}
}
