#ifndef NFSU_LIGHTS_H_
#define NFSU_LIGHTS_H_

#include <lib/winapp.h>

namespace nfsu { namespace lights
{

// Native light contexts, bit-exact with the translated guest code: elSetupLightContext 0x5437f0 (the 0x1c0-byte
// context of a scenery instance, car or world model), elSetupLights 0x542e40 (directional lights of the light table,
// the light database query 0x5413e0, the spherical harmonic coefficients), elSetupEnvMap 0x542c20, elCloneLightContext
// 0x544390 and the context transform 0x542400 (with the coefficient rotation 0x541c90). No guest code runs on the
// native paths. The guest code runs instead when [0x6f3edc] is 0 (the second fill path, which nothing selects) or
// when the light query returns more than 10 lights (the guest then reads past its light list).
// Settings: lights_native=off runs the guest code, shadow_lights=on runs both, compares the context bytes and the
// registers the callers read, and logs the first differences.

// Kernels for native callers, with the guest functions' arguments (guest addresses). Each writes the context in guest
// memory and returns false, without writing anything, when the native path does not cover the call; the caller then
// runs the guest function.
bool fillContext(x86::reg32 context, x86::reg32 matrix, x86::reg32 envMatrix, x86::reg32 target, x86::reg32 view);
bool setupLights(x86::reg32 context, x86::reg32 position, x86::reg32 matrix, x86::reg32 envMatrix, x86::reg32 view);
void setupEnvMap(x86::reg32 context, x86::reg32 target, x86::reg32 matrix, x86::reg32 envMatrix);
void transformContext(x86::reg32 destination, x86::reg32 source, x86::reg32 matrix);
bool cloneContext(x86::reg32 destination, x86::reg32 matrix, x86::reg32 envMatrix, x86::reg32 target,
                  x86::reg32 view, x86::reg32 source);

// The guest-convention entries of 0x5437f0 and 0x544390 (phases.cpp times them).
void fillEntry(win32::WinApplication* app, x86::CPU& cpu);
void cloneEntry(win32::WinApplication* app, x86::CPU& cpu);

// Perf builds: logs the light line for the last frames and resets the sums.
void report(uint32_t frames);
// The shadow total (benchmark end).
void logShadowTotal();

}}

#endif
