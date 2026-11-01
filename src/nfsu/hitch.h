#ifndef NFSU_HITCH_H_
#define NFSU_HITCH_H_

#include <cstdint>

// Hitch log (hitch_log=on): one `hitch:` line for every presented frame whose interval from the previous present is
// over 41.7 ms (a missed vblank at 30 fps), with the race time (or the game flow state outside a race), the frame
// interval, the render thread's and display queue's time for that frame, and the game thread's work that produced
// it: wall time, phase times (timing_phases' phases, lock releases taken out), lock releases by reason and the event
// sums of all threads over that time (texture uploads and locks, file reads, JDLZ, texture creation and skin
// composition calls, Direct3D queue waits, frame slot and GPU fence waits, lock holds of other threads, preemption).
// Each line names the dominant cause: the part with the largest time over its running average, where a wait or load
// call inside a phase wins over the phase when it is at least half of that excess. A summary line (slow frames by
// dominant cause) follows the benchmark summary, or each race's end without the benchmark. While a race runs the
// lines are kept in memory and written when the race ends, at the summary or when MaxBuffered are kept; elsewhere
// they are written at once. Off: no clock reads and no work.

namespace nfsu { namespace hitch {

// Game thread, after each frame render (phases::frame), with hitch_log=on: the frame's phase sums in microseconds
// (phases::Count of them). Collects the frame, logs the slow presents of finished frames.
void frame(const uint32_t* phaseUs);

// Game thread: the summary counts start again (the benchmark's measurement start).
void resetSummary();

// Game thread: writes the kept lines and logs the summary of the slow frames since the last reset, then resets.
void logSummary(const char* label);

}}

#endif
