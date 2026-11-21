#ifndef NFSU_GROUPS_H_
#define NFSU_GROUPS_H_

#include "draw.h"

#include <lib/winapp.h>
#include <recomp.h>

#include <vector>

namespace nfsu { namespace groups
{

// Native effect group functions (the start and end functions of the 7 effect groups at 0x71ABB8: 0x40E740,
// 0x40E7B0, 0x40E900, 0x40EB30, 0x40EB60 and the empty 0x40A880), the texture lookup by name hash 0x5461C0 they
// use, and the model texture swaps of the model submit (0x53F540 puts a model's replacement textures into its solid,
// 0x53F500 puts the solid's own back). Bit-exact with the translated guest code: the same guest memory stores, the
// same effect and device calls in the same order, the registers the callers read.
//
// Settings: groups_native=on (default) runs them natively, off runs the guest code; shadow_groups=on runs the guest
// code with its device and effect calls recorded, then the native code against the recording, compares memory,
// registers and calls, and logs the first differences and a total at the benchmark's end.

// Calls the group function at target (cpu.esp at the return address slot) as a call instruction does: known
// functions natively, others (and any call under a capture) as guest code.
void call(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 target);

// The texture of a name hash as 0x5461C0 (hash, 1, 0) finds it (eax), with ecx and edx as it leaves them; sp is the
// guest esp of the call (its return address slot), used when the guest function has to run.
void lookup(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 hash, x86::reg32 sp, x86::reg32& eax,
            x86::reg32& ecx, x86::reg32& edx);

// The guest memory lookup() of a hash stores to (the texture pack list links and the texture's used flag), added to
// spans for shadow compares.
void textureSpans(x86::reg32 hash, std::vector<draw::Span>& spans);

// With shadow_groups=on: logs the calls compared and differing so far as a "shadow_groups:" line.
void logShadowTotal();

}}

#endif
