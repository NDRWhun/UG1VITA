#ifndef NFSU_PREP_H_
#define NFSU_PREP_H_

#include <lib/winapp.h>

namespace nfsu { namespace prep
{

// Guest 0x446730 StuffScenery: for a view it fills the light context of every visible instance that has one, then
// submits the instance for rendering. Each instance's light-context fill runs on the job workers; the game thread
// keeps the bump allocation and the submission in the guest's order. Settings: prep_native=0 runs the guest code,
// prep_parallel=0 keeps the fills on the game thread, shadow_prep=1 runs both and logs the first guest-memory write
// or return value that differs.
void stuffScenery(win32::WinApplication* app, x86::CPU& cpu);

// Guest 0x40a580, the frame render: runs the guest code and logs the render-preparation, math-helper and flare perf
// lines (perf builds, every ReportFrames frames).
void frame(win32::WinApplication* app, x86::CPU& cpu);

}}

#endif
