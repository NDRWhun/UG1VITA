#ifndef NFSU_FLARES_H_
#define NFSU_FLARES_H_

#include <lib/winapp.h>

namespace nfsu { namespace flares
{

// Native light flares: guest 0x544f30 (the world flares of a view, with 0x544e90 per flare pack and 0x540c70),
// 0x5447c0 (one flare: intensity, visibility, the flare quads) and 0x5443f0 (one quad, built and submitted through
// guest 0x40c300). In the world pass the game thread updates the flare records and pack flags in the guest's order,
// the job workers compute each flare's visibility and quad inputs, and the game thread then builds and submits the
// quads in the guest's order. Settings: flares_native=off runs the guest code, flares_parallel=off keeps the per-flare
// work on the game thread, shadow_flares=on runs native and guest code and logs the first difference.
void worldFlares(win32::WinApplication* app, x86::CPU& cpu);
void lightFlare(win32::WinApplication* app, x86::CPU& cpu);
void flareQuad(win32::WinApplication* app, x86::CPU& cpu);

// Perf builds: logs the flare line for the last frames and resets the sums.
void report(uint32_t frames);

}}

#endif
