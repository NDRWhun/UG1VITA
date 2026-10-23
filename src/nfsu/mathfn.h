#ifndef NFSU_MATHFN_H_
#define NFSU_MATHFN_H_

#include <x86.h>

namespace nfsu { namespace mathfn
{

// Native math helpers of the game (trigonometry by 16-bit angle, table and graph lookups, vector normalisation,
// bounding boxes), bit-exact with the translated guest code: every x87 chain in double in the guest's order, floats
// where the guest stores floats. The overrides (generate.py NATIVE_OVERRIDES) also leave the registers, flags and
// x87 state the translated code leaves. Settings: math_native=0 runs the guest code, shadow_math=1 runs both and
// logs the first differences.

// Kernels for other native code: results only, no CPU state.

// Sine of a 16-bit angle (guest 0x564b10), the x87 result in double.
double sin16(x86::reg32 angle);
// Sine and cosine of a 16-bit angle, rounded to float as the guest stores them (guest 0x564bd0).
void sinCos16(x86::reg32 angle, float& sine, float& cosine);

// Perf builds: the helpers' calls and their estimated time since the last call; for the prep perf line.
struct Usage
{
    unsigned calls;
    double ms;
};
Usage takeUsage();

}}

#endif
