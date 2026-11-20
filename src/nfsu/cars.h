#ifndef NFSU_CARS_H_
#define NFSU_CARS_H_

#include <lib/winapp.h>

namespace nfsu { namespace cars
{

// Native car rendering functions, bit-exact with the translated guest code: x87 chains in double in the guest's
// order with floats where the guest stores floats, the same guest memory stores, the same calls of other guest
// functions with the same stack and the registers, flags and x87 state the callers read. Settings: cars_native=on
// (default) runs them natively, off runs the guest code; shadow_cars=on runs the guest code first with its device and
// effect calls and its calls of other functions recorded, then the native code against the recording, compares the
// stores, registers and calls, and logs the first differences.
//
// Ported so far: CarPartCuller::CullParts 0x569460, the car shadow's convex hull 0x56cf10 (with its sort 0x56ce10,
// the CRT qsort 0x672b40 and the comparators 0x56cd90 and 0x56cdf0), its corner smoothing 0x56d0c0 and its setup
// 0x56d700; RenderCars 0x571050 with 0x570b90, 0x4280a0, 0x571440, 0x42a120, 0x401cd0, 0x429ba0 and the camera
// movers' OutsidePOV (other targets run the guest code), calling CarRenderInfo::Render 0x56dc30;
// RenderCarReflections 0x570db0 with 0x41ff50, 0x421c40 and 0x565020; the cars' RenderFlares 0x570d00 with
// 0x570c20, CarRenderInfo::RenderFlare 0x56d8d0, GetPixelSize 0x545180 and 0x5693b0, calling 0x5447c0; the cars of
// the render list 0x570950 with 0x5708f0, calling the model submit 0x40c910 or CarRenderInfo::Render.

enum Port
{
    PartCull,       // 0x569460
    ShadowHull,     // 0x56cf10
    ShadowCorners,  // 0x56d0c0
    ShadowSetup,    // 0x56d700
    RenderCarsPort, // 0x571050
    Reflections,    // 0x570db0
    CarFlares,      // 0x570d00
    RenderList,     // 0x570950
    PortCount
};

// The override of a ported function: native, guest or shadow compare as the settings say.
void run(Port port, win32::WinApplication* app, x86::CPU& cpu);

// Called first by the overrides of the functions the ported code calls (0x440a40, 0x56dc30, 0x5447c0, 0x40c910): true when a shadow
// compare recorded the call (the call ran) and the caller must return.
extern bool g_tap;
bool tapSlow(x86::reg32 address, win32::WinApplication* app, x86::CPU& cpu,
             void (*function)(win32::WinApplication*, x86::CPU&));
inline bool tap(x86::reg32 address, win32::WinApplication* app, x86::CPU& cpu,
                void (*function)(win32::WinApplication*, x86::CPU&))
{
    return __builtin_expect(g_tap, 0) && tapSlow(address, app, cpu, function);
}

// With shadow_cars=on: logs the calls compared and differing so far as a "shadow_cars:" line.
void logShadowTotal();

}}

#endif
