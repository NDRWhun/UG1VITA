#ifndef NFSU_PARTICLES_H_
#define NFSU_PARTICLES_H_

#include <lib/winapp.h>

namespace nfsu { namespace particles
{

// Guest 0x404110, the draw of a particle system list for a view (stdcall: the list, the view): for each system the
// group switch, transforms and texture states, then every live particle as a camera-facing quad of 4 vertices
// (position, packed colour, texture cell) into 30 rotating vertex buffers, drawn every 300 particles. The native
// version computes each particle's colour and quad from its age, the system's colour and size curves and the view
// axes on the job workers, then locks, copies and draws in the guest's order with the guest's Direct3D calls.
// Settings: particles_native=1 runs the native version (default 0: the guest code), particles_parallel=0 keeps the
// per-particle work on the game thread, shadow_particles=1 runs the guest code and compares the vertices it wrote
// and its guest-memory results against the native computation.
void drawList(win32::WinApplication* app, x86::CPU& cpu);

// Guest 0x403a60, the particle system update (stdcall: the system, the time step): the emitters' spawns and timers
// in order with the guest's calls, then every particle's age, animation frame, velocity with drag and gravity,
// position and packed direction; expired particles and empty groups go back to their pools. The native version
// computes the particles' new state on the job workers and stores it, removing the expired ones, in the guest's
// order. In the front end (game flow state 3) the guest body runs. particles_native=1 turns it on with the draw,
// particles_update=0 keeps the guest update; shadow_particles=1 also compares each particle the guest body updated
// with the native step computed from its state at entry.
void update(win32::WinApplication* app, x86::CPU& cpu);

// Perf builds: logs the particle line for the last frames and resets the sums.
void report(uint32_t frames);

}}

#endif
