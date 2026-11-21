#ifndef NFSU_CULL_H_
#define NFSU_CULL_H_

#include <lib/winapp.h>

namespace nfsu { namespace cull
{

// Native scenery culler, guest 0x446620 (esi = the culler object): for every view it builds the list of visible
// scenery models. The visibility of each (view, section) is computed on the job workers; the game thread then
// appends the list entries and makes the frame allocations in the guest's order. Settings: cull_native=off runs the
// guest code instead, cull_parallel=off keeps everything on the game thread, shadow_cull=on runs both and compares.
void doCulling(win32::WinApplication* app, x86::CPU& cpu);

// Guest 0x40db20: box (edx = min, ecx = max, eax = optional matrix) against the six planes of a view (stack
// argument): 0 outside, 1 partial, 2 inside.
void getVisibleState(win32::WinApplication* app, x86::CPU& cpu);

// The test of guest 0x40db20 without a matrix, for a box as the guest holds it after loading (min, max): 0
// outside, 1 partial, 2 inside. compareA and compareB get the operands of its last x87 compare. Any thread.
int boxState(const float mn[3], const float mx[3], x86::reg32 view, double& compareA, double& compareB);

// Guest 0x446fd0 on live memory (eax = the region list, [esp + 4] = the position; ret 4): the region whose polygon
// holds the position, moved to the front of the list, with the registers and the last x87 compare the guest
// leaves. False, with nothing done, for a list other than the one the culler reads.
bool findDrivableSection(x86::CPU& cpu);

}}

#endif
