#ifndef NFSU_DRAW_H_
#define NFSU_DRAW_H_

#include <lib/winapp.h>
#include <winapi/d3d9_native.h>
#include <recomp.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nfsu { namespace draw
{

// Native setup functions of the deferred draw queue's flush, bit-exact with the translated guest code: the same
// guest memory stores, the same device and effect calls in the same order (d3d9_native.h), x87 chains in double in
// the guest's order with floats where the guest stores floats, and the registers, flags and x87 state the callers
// read. Settings: draw_native=on (default) runs them natively, off runs the guest code; shadow_draw=on runs the guest code
// with its device and effect calls recorded, then the native code against the recording, and logs the first
// difference.
//
// The draw queue itself (flush.cpp): model submit 0x40c910, packet enqueue 0x40f1e0 and the flush 0x40ed50.
// flush_native=on (default) runs them natively; with shadow_draw=on each runs as guest code first, recorded, then
// natively against the recording.
//
// flush_async (with flush_native=on, without shadow_draw): 1 (default) queues the native flush's device and effect
// calls (d3d9_native.h deferral) and a queue thread runs them while the game thread goes on; 2 queues them and the
// next drain runs them on the game thread; 0 calls them at once. flush_async_frame=1 (default) keeps queuing the
// game thread's device and effect calls after the first flush, 0 only those of flushes. flush_async_core (1-3,
// default 3 with core 3, else 2) and flush_async_priority (default 70) place the queue thread.

// Perf builds: the call counts and time of the setup functions and of the queue since the last call, logged as a
// "draw:" line; frames is the number of frames they cover.
void logUsage(unsigned frames);

// ---- Shared by draw.cpp and flush.cpp ----

enum Leaf
{
    LeafWorld, LeafBlend, LeafAlphaTest, LeafPackets, LeafNode, LeafGroup, LeafBones, LeafModel, LeafLights,
    LeafReflection, LeafMaterial, LeafCount
};

// Runs a setup function as a native caller does: the native body with draw_native=on, else the guest body; counted
// for the perf line. cpu.esp points at the return address slot.
void runLeaf(Leaf leaf, win32::WinApplication* app, x86::CPU& cpu);

bool nativeLeaves();
bool shadowCompare();

// Guest stores of the native code; shadow compares log their addresses.
extern bool g_logStores;
extern std::vector<x86::reg32> g_stores;

inline void store(x86::reg32 address, x86::reg32 value)
{
    if (__builtin_expect(g_logStores, 0))
        g_stores.push_back(address);
    rc::st32(address, value);
}

struct Span
{
    x86::reg32 address;
    x86::reg32 size;
};

enum : uint8_t
{
    RegEax = 1, RegEcx = 2, RegEdx = 4, RegEbx = 8, RegEsp = 0x10, RegEbp = 0x20, RegEsi = 0x40, RegEdi = 0x80
};

void snapshot(const std::vector<Span>& spans, std::vector<x86::reg8>& bytes);
void restoreSpans(const std::vector<Span>& spans, const std::vector<x86::reg8>& bytes);
// False with a text when a register of the mask that the guest function stores at its return, the x87 depth or a
// flag read at the return (retstate.h) differs.
bool sameState(x86::reg32 function, uint8_t registers, const x86::CPU& g, const x86::CPU& n, char* text,
               size_t size);
// False with a text when the replay of a capture found a difference or did not use every recorded call.
bool sameCalls(const win32::d3d9::bridge::Capture& c, char* text, size_t size);
// False with a text at the first byte of the spans that differs, or the first logged store outside them.
bool sameMemory(const std::vector<Span>& spans, const std::vector<x86::reg8>& guest,
                const std::vector<x86::reg8>& native, char* text, size_t size);

// Queue counters of flush.cpp for the perf line, reset by the call.
struct QueueUsage
{
    uint32_t flushes, packets, draws, nodes, enqueues, submits;
    uint64_t flushUs;
    uint32_t compared, differing, skipped;
};
QueueUsage takeQueueUsage();
bool nativeQueue();
// Perf builds: the "flush_async:" line of the flush's deferred device calls, when they are on.
void logDeferral(unsigned frames);

}}

#endif
