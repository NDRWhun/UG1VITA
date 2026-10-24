#ifndef NFSU_BENCH_H_
#define NFSU_BENCH_H_

#include <lib/winapp.h>

// Race benchmark (bench=1): the game boots straight into a skip-front-end race on a fixed track with the player's
// car driven by the AI, and logs frame time statistics over the race. Settings: bench_track=<track number>
// (default 1001), bench_laps (default 2), bench_opponents (default 3), bench_seconds=<n> ends the measurement
// early, bench_exit=1 quits the game after the summary.

namespace nfsu { namespace bench {

// True when bench=1 (read by configure).
bool enabled();

// Before guest code runs: sets the game's skip-front-end race settings and hooks the track table loader.
void configure(win32::WinApplication& app);

// Game thread, at each pad poll: starts and ends the measurement, quits after the summary with bench_exit=1.
void poll();

}}

#endif /* !NFSU_BENCH_H_ */
