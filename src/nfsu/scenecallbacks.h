#ifndef NFSU_SCENECALLBACKS_H_
#define NFSU_SCENECALLBACKS_H_

#include <lib/winapp.h>

namespace nfsu { namespace callbacks
{

// Native versions of the guest functions the native light flares (flares.cpp) and particles (particles.cpp) call,
// with the guest's calling convention (cpu.esp at the return address slot). scene_callbacks_native=off runs the
// guest code; shadow_scene_callbacks=on runs each call as guest code first, then natively against it
// (sceneshadow.h), and logs the differences.

// 0x403a00: eax = a particle system; the camera positions of views 2, 3 and 1 at +0x10, +0x20, +0x30.
void cameraPositions(win32::WinApplication* app, x86::CPU& cpu);
// 0x401f30: edx = a random state, [esp + 4] = a float range; pushes a random value in [0, range).
void random(win32::WinApplication* app, x86::CPU& cpu);
// 0x401e10: eax = a particle group; its counters cleared and its particles back to the particle pool.
void releaseGroup(win32::WinApplication* app, x86::CPU& cpu);
// 0x402d30: [esp + 4] = a particle system, [esp + 8] = an emitter, [esp + 0xc] = dt, [esp + 0x10] = the group flag;
// the particles the emitter spawns in dt. ret 0x10.
void spawn(win32::WinApplication* app, x86::CPU& cpu);
// 0x446fd0: eax = the region list, [esp + 4] = a position; the region holding it, moved to the front. ret 4.
void region(win32::WinApplication* app, x86::CPU& cpu);
// 0x4406d0 and 0x440710: a topology coordinate (eax) set up empty; its cached polygons released.
void coordinateInit(win32::WinApplication* app, x86::CPU& cpu);
void coordinateRelease(win32::WinApplication* app, x86::CPU& cpu);

// At the end of a benchmark: the shadow compare totals.
void logShadowTotal();

}}

#endif
