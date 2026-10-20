#ifndef LIB_FPUCONTROL_H_
#define LIB_FPUCONTROL_H_

#include <cpu.h>

// x87 control word events the port reproduces.
//
// The game unmasks invalid, zero-divide, overflow and denormal with _controlfp(3, 0x8001F) during init and then
// creates its Direct3D 9 device without D3DCREATE_FPU_PRESERVE, which on Windows sets the control word to single
// precision, round to nearest, every exception masked (0x007F) for the rest of the run. The port keeps that
// visible state: guest code that reads the control word (fnstcw, _controlfp, the CRT's libm checks against
// 0x027F) sees the PC's values. Arithmetic itself is not narrowed: the recompiled x87 computes in double
// precision whatever the precision field says, and no x87 exception ever traps (as with all exceptions masked).
namespace win32 { namespace fpucontrol
{

// Call from IDirect3D9::CreateDevice with its behaviour flags.
void deviceCreated(x86::CPU& cpu, x86::reg32 behaviorFlags);

}}

#endif
