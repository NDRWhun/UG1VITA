#ifndef NFSU_SCENESHADOW_H_
#define NFSU_SCENESHADOW_H_

#include "draw.h"

#include <lib/winapp.h>
#include <recomp.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nfsu { namespace sceneshadow
{

// Shadow compare of a native scene port against its guest body, call by call:
//   1. the guest body runs with its device and effect calls recorded (d3d9_native.h Capture) and with its calls of
//      the recorded callees (below) recorded: entry registers, stack arguments and the CPU at the return;
//   2. the spans (guest memory the function may store to besides its stack) are put back to their entry bytes;
//   3. the native body runs from the entry CPU with its device calls compared against the recording, its callee
//      calls compared and answered from the recording (they do not run again) and its stores outside the stack
//      logged (store()); at a recorded callee the spans must hold what the guest run had at that call, and take what
//      the callee left there;
//   4. the guest's spans and CPU are put back; device calls, callee calls, the registers the callers read
//      (retstate.h), the x87 depth and the spans are compared.
//
// Recorded callees are guest functions with effects outside the compared spans: the model submit 0x40c910 (draw
// queue) and the car flare 0x56d8d0. Their overrides call record() first.

typedef void (*Function)(win32::WinApplication*, x86::CPU&);

enum Mode
{
    Off,
    Recording,      // the guest run of a compare
    Replaying       // the native run of a compare
};
extern Mode g_mode;
// Set for the whole of a compare, also while a recorded callee runs.
extern bool g_active;

// A callee override's first line: while the guest run of a compare records, runs function (the override itself)
// with the recording paused, records the call and returns true.
bool recordSlow(x86::reg32 address, uint32_t arguments, uint8_t registers, win32::WinApplication* app,
                x86::CPU& cpu, Function function);
inline bool record(x86::reg32 address, uint32_t arguments, uint8_t registers, win32::WinApplication* app,
                   x86::CPU& cpu, Function function)
{
    return __builtin_expect(g_mode == Recording, 0) && recordSlow(address, arguments, registers, app, cpu, function);
}

// A native port's call of a recorded callee (cpu.esp at the return address slot, arguments and the registers of
// the mask set): runs function, or in the native run of a compare checks the call against the next recorded one
// and takes the CPU it returned with. False when the game is ending.
bool call(x86::reg32 address, uint32_t arguments, uint8_t registers, win32::WinApplication* app, x86::CPU& cpu,
          Function function);

// True while a compare runs (the ports' overrides then run their guest bodies).
inline bool comparing()
{
    return g_active;
}

// A native store outside the guest stack: logged in the native run of a compare (draw.h g_stores).
inline void store(x86::reg32 address, x86::reg32 value)
{
    draw::store(address, value);
}

struct Stats
{
    uint32_t calls = 0, differing = 0, logged = 0;
};

// Runs a compare of one call (cpu as at the guest's entry, esp at the return address slot); cpu ends as the guest
// left it. registers: the registers the native leaves for its callers. tag: the prefix of the difference lines.
// x87: the x87 condition codes and the top of the stack are compared too.
void compare(const char* tag, const char* name, x86::reg32 function, uint8_t registers, win32::WinApplication* app,
             x86::CPU& cpu, Function guest, Function native, const std::vector<draw::Span>& spans, Stats& stats,
             bool x87 = false);

// Spans every compare of the frame arena needs: its pointer and failure words, and the next bytes it hands out.
void arenaSpans(std::vector<draw::Span>& spans);

}}

#endif
