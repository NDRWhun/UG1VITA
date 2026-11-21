#ifndef NFSU_VIEWS_H_
#define NFSU_VIEWS_H_

#include <lib/winapp.h>
#include <recomp.h>

namespace nfsu { namespace views
{

// Native world views of the frame render 0x409580, bit-exact with the translated guest code: the same calls of the
// functions it calls with the same registers and stack, the same device and effect calls in the same order, the same
// guest memory stores. Its view begin 0x40ABD0, the mirror car submit 0x571870 and the reflection scenery pass
// 0x4468A0 with its callback 0x409090 run inline.
//
// Settings: views_native=on (default) runs it natively, off runs the guest code; shadow_views=on runs the guest code
// with its device and effect calls and the calls of other functions recorded (each of those runs once), then the
// native code against the recording, compares, and logs the first differences and a total at the benchmark's end.

// The views (cpu.esp at the return address slot).
void run(win32::WinApplication* app, x86::CPU& cpu);

// With shadow_views=on: logs the calls compared and differing so far as a "shadow_views:" line.
void logShadowTotal();

}}

#endif
