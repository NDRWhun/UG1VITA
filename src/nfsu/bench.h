#ifndef NFSU_BENCH_H_
#define NFSU_BENCH_H_

#include <lib/winapp.h>

// Race benchmark (benchmark=on): the game boots straight into a skip-front-end race on a fixed track with a fixed
// random seed, and logs frame time statistics over the race. Settings: benchmark_mode=circuit|sprint|drag|knockout
// (default circuit), benchmark_track=<track number> (default per mode: 1001, 1102, 1201, 1001), benchmark_car=<car
// name> (default: the game's, skyline), benchmark_driver=ai|throttle (ai: the AI drives the player's car; throttle,
// drag only: the race's own drag controls with the throttle held and automatic gears), benchmark_laps (circuit,
// default 2), benchmark_opponents (default 3), benchmark_seconds=<n> ends the measurement early, benchmark_exit=on
// quits the game after the summary.

namespace nfsu { namespace bench {

// True when benchmark=on (read by configure).
bool enabled();

// Game thread, with or without the benchmark: a race is running (race coordinator state 4 and race stage 4, the
// state the benchmark measures in).
bool racing();

// Before guest code runs: sets the game's skip-front-end race settings and hooks the track table loader.
void configure(win32::WinApplication& app);

// Game thread, at each pad poll: starts and ends the measurement, quits after the summary with benchmark_exit=on.
void poll();

// Game thread, at each pad poll: joystick buttons the benchmark holds (the throttle with benchmark_driver=throttle
// while racing), 0 otherwise.
x86::reg32 heldButtons();

}}

#endif /* !NFSU_BENCH_H_ */
