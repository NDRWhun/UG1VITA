#ifndef NFSU_DRAW_H_
#define NFSU_DRAW_H_

#include <lib/winapp.h>

namespace nfsu { namespace draw
{

// Native setup functions of the deferred draw queue's flush, bit-exact with the translated guest code: the same
// guest memory stores, the same device and effect calls in the same order (d3d9_native.h), x87 chains in double in
// the guest's order with floats where the guest stores floats, and the registers, flags and x87 state the callers
// read. Settings: draw_native=1 runs them natively (default 0: the guest code); shadow_draw=1 runs the guest code
// with its device and effect calls recorded, then the native code against the recording, and logs the first
// difference.

// Perf builds: the call counts and time of the setup functions since the last call, logged as a "draw:" line; frames
// is the number of frames they cover.
void logUsage(unsigned frames);

}}

#endif
