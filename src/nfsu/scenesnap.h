#ifndef NFSU_SCENESNAP_H_
#define NFSU_SCENESNAP_H_

#include <lib/winapp.h>
#include <recomp.h>

#include <cstdint>

namespace nfsu { namespace scenesnap
{

// The scene build's snapshot at the handoff (docs PARALLEL_SCENE.md section 3 and its phase 2): just before the
// world views 0x409580, the game thread
//   1. updates the space nodes of the world models the views will draw (decision (b): the views then update none);
//   2. copies, on whole 64-byte lines, everything the views read that the rest of the frame (filter and FE pass, Present and the loop tail,
//      the loop top, the step, and up to the track streamer the cameras and sound) writes: the snapshot. Its ranges
//      are built per frame by walking the owners (cars, world models and their space nodes, particle lists, clans
//      and skids, the region and texture pack lists, texture animations, eViews and their cameras, the timers,
//      counters and render-state words);
//   3. runs the views, still on the game thread over live memory.
//
// Settings: scene_snapshot=serial does the above every frame (off: the views alone); shadow_scene_snapshot=on also
// checks every second frame that the views update exactly the space nodes listed before them (the nodes are then
// not updated first), and on the other frames that the snapshot is complete: the views run with their device calls
// recorded, and at the join (after the step, or at the track streamer call with shadow_scene_snapshot_join=streamer)
// the views run a second time with the snapshot copied over live memory and their device calls compared with the
// recording instead of running. The second run sees the topology caches as at the handoff (the stores of the
// topology queries since then undone through the coll.h journal; shadow_scene_snapshot_topology=live leaves them as
// the step left them). Then live memory is put back. The first differences and a total are logged.

// The views call of the frame render (phases.cpp sub_409580): cpu.esp at the return address slot.
void views(win32::WinApplication* app, x86::CPU& cpu, void (*run)(win32::WinApplication*, x86::CPU&));
// After World::DoTimestep 0x420b40 returned and at the entry of TrackStreamer::Service 0x445050: the join points of
// the shadow check.
void afterStep(win32::WinApplication* app, x86::CPU& cpu);
void beforeStreamer(win32::WinApplication* app, x86::CPU& cpu);
// A SpaceNode::Update 0x5791c0 call of node (guest path); counted when the views make it after the snapshot updated
// the nodes, listed on the shadow's node check frames.
void spaceNodeUpdate(x86::reg32 node);
// The shadow check's record run: StuffScenery's read of an instance's flags word +0x2e (cleared after its submit,
// set by the reflection pass; render state carried from frame to frame). Its value is put back for the second run.
extern bool g_noteFlags;
void noteFlagSlow(x86::reg32 address);
inline void noteFlag(x86::reg32 address)
{
    if (__builtin_expect(g_noteFlags, 0))
        noteFlagSlow(address);
}

// At the benchmark's end: the snapshot sizes and times, and the shadow totals.
void logTotal();

}}

#endif
