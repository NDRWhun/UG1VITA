#ifndef NFSU_WORLDMODELS_H_
#define NFSU_WORLDMODELS_H_

#include <lib/winapp.h>

#include <cstdint>
#include <vector>

namespace nfsu { namespace worldmodels
{

// Native scene parts of the world views, bit-exact with the translated guest code: RenderWorldModels 0x592100 with
// WorldModel::Render 0x591e40 and SpaceNode::Update 0x5791c0, RenderClans 0x58a240 with the skid render 0x589da0
// and 0x589810, the textured quad submit 0x40c300 (skids, light flares), MiniWorld::Render 0x4ae650,
// MiniWorld::RenderFlares 0x4ae7d0 (the light flare 0x5447c0 through its entry) and the FE flares 0x570c70 (the car
// flare 0x56d8d0 they call stays guest code). world_models_native=off runs the guest code;
// shadow_world_models=on runs each call as guest code first, then natively against it (sceneshadow.h), and logs
// the differences.

// The guest-convention entries of 0x592100, 0x591e40, 0x58a240 and 0x4ae650 (phases.cpp times them).
void worldModels(win32::WinApplication* app, x86::CPU& cpu);
void worldModel(win32::WinApplication* app, x86::CPU& cpu);
void clans(win32::WinApplication* app, x86::CPU& cpu);
void miniWorld(win32::WinApplication* app, x86::CPU& cpu);

// The scene snapshot's update of the space nodes before the views (PARALLEL_SCENE.md decision (b)): the marked
// space nodes of the world models RenderWorldModels draws in view with the render flags 0, updated as
// WorldModel::Render 0x591e40 would (SpaceNode::Update 0x5791c0 is lazy and depends only on the node, its parents
// and their local matrices), in the same order. Returns the number updated. With dry, the nodes are listed there
// instead of updated.
uint32_t updateNodes(x86::reg32 view, std::vector<x86::reg32>* dry = nullptr);
// While nodes is set, the space nodes WorldModel::Render updates (native path) are appended to it.
void listViewUpdates(std::vector<x86::reg32>* nodes);
// The space node updates WorldModel::Render made itself since the last call (native path).
uint32_t takeViewUpdates();

// Whether RenderClans 0x58a240 can draw the clan's skids in the view: its box is more than 10 pixels (0x545210).
bool clanInView(x86::reg32 clan, x86::reg32 view);

// Perf builds: the calls per frame since the last call; frames is the number of frames they cover.
void logUsage(uint32_t frames);
// At the end of a benchmark: the shadow compare totals.
void logShadowTotal();

}}

#endif
