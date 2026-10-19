#ifndef PLATFORM_CORES_H_
#define PLATFORM_CORES_H_

// CPU core layout. The Vita gives applications cores 0-2; CapUnlocker also opens core 3 (the system core). Each
// kind of thread has a role and runs on that role's core (a pf::Thread::Cpu mask).
//
//   role     without core 3   with core 3   threads
//   Game     0                0             guest game thread
//   Render   1                1             GXM render thread
//   Main     1                1             process main thread (guest message loop)
//   Audio    2                2             SceAudioOut mixer, timer service (guest timer callbacks)
//   Helper   2                3             other guest threads
//   Io       2                3             guest file reading threads

namespace pf { namespace cores
{

enum Role
{
    Game,
    Render,
    Main,
    Audio,
    Helper,
    Io,
    RoleCount
};

// Probes core 3 (unless the setting core3=0 turns it off), moves the calling (main) thread to the Main core and
// logs the layout. Call once at start-up, before threads with a role are created.
void init();

// True when user threads run on core 3.
bool hasCore3();
// Core number (0-3) and pf::Thread::Cpu mask of a role.
int core(Role role);
int mask(Role role);
const char* name(Role role);

// The GXM render thread's affinity.
inline int renderCore() { return core(Render); }
inline int renderMask() { return mask(Render); }

}}

#endif
