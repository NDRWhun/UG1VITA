#ifndef NFSU_CULL_H_
#define NFSU_CULL_H_

#include <lib/winapp.h>

namespace nfsu { namespace cull
{

// Native scenery culler, guest 0x446620 (esi = the culler object): for every view it builds the list of visible
// scenery models. The visibility of each (view, section) is computed on the job workers; the game thread then
// appends the list entries and makes the frame allocations in the guest's order. Settings: cull_native=0 runs the
// guest code instead, cull_parallel=0 keeps everything on the game thread, shadow_cull=1 runs both and compares.
void doCulling(win32::WinApplication* app, x86::CPU& cpu);

// Guest 0x40db20: box (edx = min, ecx = max, eax = optional matrix) against the six planes of a view (stack
// argument): 0 outside, 1 partial, 2 inside.
void getVisibleState(win32::WinApplication* app, x86::CPU& cpu);

}}

#endif
