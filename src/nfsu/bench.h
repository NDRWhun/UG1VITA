#ifndef NFSU_BENCH_H_
#define NFSU_BENCH_H_

#include <lib/winapp.h>

// Race benchmark (benchmark=on): the game boots straight into a skip-front-end race on a fixed track with a fixed
// random seed, and logs frame time statistics over the race. Settings: benchmark_mode=circuit|sprint|drag|knockout
// (default circuit), benchmark_track=<track number> (default per mode: 1001, 1102, 1201, 1001), benchmark_car=<car
// name> (default: the game's, skyline), benchmark_driver=ai|throttle (ai: the AI drives the player's car; throttle,
// drag only: the race's own drag controls with the throttle held and automatic gears), benchmark_laps (circuit,
// default 2), benchmark_opponents (default 3), benchmark_seconds=<n> ends the measurement early, benchmark_exit=on
// quits the game after the summary, benchmark_tuned=on fits the player's and the opponents' cars with the top physics
// upgrades and the top level part of each slot (body kit, neon, ...), benchmark_crashes=on steers the player's car
// into the side of the road for 2.5 s of every 8 s (AI driver), benchmark_light_trails=on|off and
// benchmark_motion_blur=on|off|enhanced set those display settings before the race,
// benchmark_player_physics=full|simplified (default full: the player's car moves with the full physics of a
// human-driven car while the AI drives it).

namespace nfsu { namespace bench {

// True when benchmark=on (read by configure).
bool enabled();

// True with benchmark=on and benchmark_player_physics=full (default): the AI-driven player car keeps full physics.
bool playerFullPhysics();

// True with benchmark=on and benchmark_tuned=on.
bool tuned();

// Game thread, from the driver adding override (0x004322A0, edi = the driver list entry): fits a human or AI driver's
// ride for benchmark_tuned=on. Registers are kept; cpu.terminate is set if the game quit inside a guest call.
void addDriver(win32::WinApplication* app, x86::CPU& cpu);

// Game thread, with or without the benchmark: a race is running (race coordinator state 4 and race stage 4, the
// state the benchmark measures in).
bool racing();

// Game thread: the race coordinator's state and the current race's stage (3 the countdown, 4 racing), ~0 without a
// coordinator or race.
void raceState(x86::reg32* coordinatorState, x86::reg32* raceStage);

// Before guest code runs: sets the game's skip-front-end race settings and hooks the track table loader.
void configure(win32::WinApplication& app);

// Game thread, at each pad poll: starts and ends the measurement, quits after the summary with benchmark_exit=on.
void poll();

// Game thread, at each pad poll: joystick buttons the benchmark holds (the throttle with benchmark_driver=throttle
// while racing), 0 otherwise.
x86::reg32 heldButtons();

}}

#endif /* !NFSU_BENCH_H_ */
