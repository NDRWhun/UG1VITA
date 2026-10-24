// Native draw queue (draw.h): the model submit 0x40c910, the packet enqueue 0x40f1e0 with the slot-pool pops of
// 0x567160, and the flush 0x40ed50 with direct device and effect calls (d3d9_native.h) and direct calls of the
// setup functions (draw.cpp). The guest functions they call (the effect group functions, the slot-pool allocation
// of an empty pool, the texture swaps 0x53f540 / 0x53f500) run as guest code on the caller's CPU state, with the
// registers and stack arguments the guest gives them.

#include "draw.h"
#include "retstate.h"
#include "phases.h"

#include <winapi/d3d9_native.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>

#include <cstdio>
#include <cstring>
#include <recomp.h>
#include <vector>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_40c910_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40f1e0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40ed50_guest(win32::WinApplication* app, x86::CPU& cpu);
// Guest functions the native code calls (generated).
void sub_567160(win32::WinApplication* app, x86::CPU& cpu);
void sub_53f540(win32::WinApplication* app, x86::CPU& cpu);
void sub_53f500(win32::WinApplication* app, x86::CPU& cpu);
void sub_40db20(win32::WinApplication* app, x86::CPU& cpu);

namespace draw
{

namespace
{

using win32::UINT;
using win32::WinApplication;
using x86::reg8;
using x86::reg32;
using x86::reg64;
using x86::sreg8;
using x86::sreg32;
namespace bridge = win32::d3d9::bridge;

// Guest data.
const reg32 CurrentGroup = 0x73640c;    // the effect group being drawn
const reg32 DevicePointer = 0x73636c;   // IDirect3DDevice9
const reg32 CurrentView = 0x736458;     // view index at +4
const reg32 NodePool = 0x7363d4;        // slot pools: free list +0x10, flags +0x14, used +0x18, maximum +0x1c,
const reg32 PacketPool = 0x7363d8;      // slot size +0x28
const reg32 VertexPool = 0x7363b8;
const reg32 LastStream = 0x7363e8;
const reg32 LastIndices = 0x7363ec;
const reg32 QueuedNodes = 0x7363fc;
const reg32 Heads = 0x78a4e8;           // 112 list heads of 8 bytes; the second table starts at SecondHeads
const reg32 SecondHeads = 0x78a6a8;
const reg32 HeadsEnd = 0x78a868;
const reg32 Groups = 0x71abb8;          // 7 effect groups of 0xe0 bytes
const reg32 GroupSize = 0xe0;
const reg32 GroupCount = 7;
const reg32 ReflectionView = 0x740760;
const reg32 SkippedModel = 0x78a4e0;
const reg32 LastView = 0x7010b8;
const reg32 LastGroup = 0x78eb7c;
const reg32 LastModel = 0x736404;
const reg32 AlphaRef = 0x7070c0;

// Draw statistics the guest counts.
const reg32 NodesDrawn = 0x71b1d8;
const reg32 Flushes = 0x71b1dc;
const reg32 IndexedPackets = 0x71b1e0;
const reg32 UserPackets = 0x71b1e4;
const reg32 StreamChanges = 0x71b1e8;
const reg32 IndexChanges = 0x71b1ec;
const reg32 Triangles = 0x71b1f0;
const reg32 GroupChanges = 0x71b204;
const reg32 CountersEnd = 0x71b208;

const uint32_t ShadowLogLimit = 20;
const uint32_t ListLimit = 100000;

struct Settings
{
    bool native;
    bool shadow;
    bridge::DeferMode async;    // the native flush's device and effect calls
};

const Settings& settings()
{
    static const Settings s = []
    {
        Settings r = { pf::Config::getBool("flush_native", false), shadowCompare(), bridge::DeferOff };
        const int async = pf::Config::getInt("flush_async", 0);
        if (r.native && !r.shadow && (async == 1 || async == 2))
            r.async = async == 1 ? bridge::DeferThread : bridge::DeferInline;
        const int defaultCore = pf::cores::hasCore3() ? 3 : 2;
        int core = pf::Config::getInt("flush_async_core", defaultCore);
        if (core < 1 || core > 3 || (core == 3 && !pf::cores::hasCore3()))
            core = defaultCore;
        int priority = pf::Config::getInt("flush_async_priority", pf::Thread::PriorityHigh + 6);
        if (priority < pf::Thread::PriorityHigh || priority > pf::Thread::PriorityLow)
            priority = pf::Thread::PriorityHigh + 6;
        bridge::configureDeferral(r.async, pf::Config::getBool("flush_async_frame", true), core, priority);
        pf::logInfo("draw: queue %s%s%s", r.native ? "native" : "guest", r.native && r.shadow ? ", shadow compare" : "",
                    r.async == bridge::DeferThread ? ", device calls of the flush on the queue thread (flush_async=1)"
                    : r.async == bridge::DeferInline ? ", device calls of the flush queued, run at the next drain"
                                                       " (flush_async=2)" : "");
        return r;
    }();
    return s;
}

QueueUsage s_usage = {};

inline void increment(reg32 address)
{
    store(address, rc::ld32(address) + 1);
}

// The registers the translated guest code holds.
struct Regs
{
    reg32 eax, ecx, edx, ebx, esi, edi;

    explicit Regs(const x86::CPU& cpu)
        :   eax(cpu.eax), ecx(cpu.ecx), edx(cpu.edx), ebx(cpu.ebx), esi(cpu.esi), edi(cpu.edi) {}
    void to(x86::CPU& cpu) const
    {
        cpu.eax = eax;
        cpu.ecx = ecx;
        cpu.edx = edx;
        cpu.ebx = ebx;
        cpu.esi = esi;
        cpu.edi = edi;
    }
    void from(const x86::CPU& cpu)
    {
        eax = cpu.eax;
        ecx = cpu.ecx;
        edx = cpu.edx;
        ebx = cpu.ebx;
        esi = cpu.esi;
        edi = cpu.edi;
    }
};

// A setup function called with the guest's registers; esp is the slot of the return address.
inline bool leafCall(Leaf leaf, WinApplication* app, x86::CPU& cpu, Regs& r, reg32 esp)
{
    r.to(cpu);
    cpu.esp = esp;
    runLeaf(leaf, app, cpu);
    if (cpu.terminate)
        return false;
    r.from(cpu);
    return true;
}

// A guest function called with the guest's registers through a pointer (an effect group function).
inline bool guestCall(WinApplication* app, x86::CPU& cpu, Regs& r, reg32 esp, reg32 target)
{
    r.to(cpu);
    cpu.esp = esp;
    bridge::callGuest(app, cpu, target);
    if (cpu.terminate)
        return false;
    r.from(cpu);
    return true;
}

// ---- 0x567160: slot-pool allocation ----

// eax = the pool, with a slot on its free list: pops it, raises the used count and its maximum, zero-fills the slot
// when flag 2 is set.
reg32 popSlot(const x86::CPU& cpu, reg32 pool)
{
    const reg32 used = rc::ld32(pool + 0x18) + 1;
    const reg32 slot = rc::ld32(pool + 0x10);
    const reg32 next = rc::ld32(slot);
    if (sreg32(used) > sreg32(rc::ld32(pool + 0x1c)))
        store(pool + 0x1c, used);
    store(pool + 0x18, used);
    store(pool + 0x10, next);
    if (rc::ld8(pool + 0x14) & 2)
    {
        const sreg32 words = sreg32(rc::ld32(pool + 0x28)) >> 2;
        const reg32 step = cpu.flags.df ? 0u - 4u : 4u;
        reg32 at = slot;
        for (sreg32 n = 0; n < words; ++n, at += step)
            store(cpu.ees + at, 0);
    }
    return slot;
}

// A slot of the pool: popped natively, or by the guest function when the free list is empty (it expands the pool).
// esp is the guest's esp at the call. The registers are the guest's (r).
reg32 allocate(WinApplication* app, x86::CPU& cpu, const Regs& r, reg32 pool, reg32 esp)
{
    if (rc::ld32(pool + 0x10) != 0)
        return popSlot(cpu, pool);
    r.to(cpu);
    cpu.eax = pool;
    cpu.esp = esp - 4;
    sub_567160(app, cpu);
    return cpu.eax;
}

// ---- 0x40f1e0: enqueue ----

// eax = the model; stack arguments 1..8: mesh, holder, flags, group key, world, light context, material, bone
// palette. The packet goes under the model's node, a new node from the node pool when the cached one belongs to
// another group key; a new node is appended to its list head.
void enqueueNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const auto arg = [entry](reg32 n) { return rc::ld32(entry + 4 * n); };
    const reg32 ebp0 = cpu.ebp, esi0 = cpu.esi, edi0 = cpu.edi;
    const reg32 key = arg(4);
    const reg32 model = cpu.eax;
    rc::st32(entry - 4, ebp0);
    rc::st32(entry - 8, esi0);
    rc::st32(entry - 12, edi0);
    const reg32 holder = rc::ld32(model);
    Regs r(cpu);
    r.esi = model;
    r.edi = holder;
    cpu.ebp = key;
    reg32 node = rc::ld32(holder + 0x1c);
    if (node == 0 || rc::ld32(node + 0x14) != key)
    {
        r.eax = rc::ld32(NodePool);
        node = allocate(app, cpu, r, r.eax, entry - 12);
        if (cpu.terminate)
            return;
        if (node != 0)
        {
            store(node + 0x18, node + 0x18);
            store(node + 0x1c, node + 0x18);
            store(node + 0x10, model);
            store(node + 8, 0);
            store(node + 0xc, 0);
        }
        store(holder + 0x1c, node);
        const reg8 second = rc::ld8(model + 0x54);
        const reg32 index = reg32(sreg32(sreg8(rc::ld8(model + 0x51)))) * 7 + rc::ld32(key);
        const reg32 head = index * 8 + (second ? SecondHeads : Heads);
        const reg32 tail = rc::ld32(head + 4);
        store(tail, node);
        store(head + 4, node);
        store(node + 4, tail);
        store(node, head);
        increment(QueuedNodes);
        r.ecx = head;
        r.edx = tail;
    }
    r.eax = rc::ld32(PacketPool);
    const reg32 owner = rc::ld32(holder + 0x1c);
    r.esi = owner;
    const reg32 packet = allocate(app, cpu, r, r.eax, entry - 12);
    if (cpu.terminate)
        return;
    if (packet != 0)
    {
        store(packet + 8, arg(1));
        store(packet + 0x10, arg(2));
        store(packet + 0x14, arg(3));
        store(packet + 0x1c, arg(6));
        store(packet + 0x20, arg(7));
        store(packet + 0xc, 0);
        store(packet + 0x24, arg(5));
        store(packet + 0x18, key);
        store(packet + 0x28, arg(8));
    }
    const reg32 tail = rc::ld32(owner + 0x1c);
    store(tail, packet);
    store(owner + 0x1c, packet);
    store(packet + 4, tail);
    store(packet, owner + 0x18);
    const reg32 count = rc::ld32(owner + 8) + 1;
    const reg32 vertices = rc::ld32(owner + 0xc);
    store(owner + 8, count);
    store(owner + 0x14, key);
    const reg32 mesh = arg(1);
    const reg32 added = rc::ld32(mesh + 0xc);
    store(owner + 0xc, vertices + added);
    ++s_usage.enqueues;
    cpu.eax = added;
    cpu.ecx = vertices + added;
    cpu.edx = mesh;
    cpu.ebp = ebp0;
    cpu.esi = esi0;
    cpu.edi = edi0;
    cpu.esp = entry + 4;
}

// ---- 0x40ed50: flush ----

// The flush keeps the guest's stack frame: f = esp after its prologue, f + 0xc the node, f + 0x10 the mesh,
// f + 0x14 the list head, f + 0x18 the group + 0x18, f + 0x1c Begin's pass count.

// Indexed packet: bone palette, stream and indices when they changed, a DrawIndexedPrimitive per pass.
bool drawIndexed(WinApplication* app, x86::CPU& cpu, Regs& r, reg32 f)
{
    r.eax = rc::ld32(r.esi + 0x28);
    r.edi = rc::ld32(r.esi + 8);
    rc::st32(f + 0x10, r.edi);
    if (r.eax != 0 && (rc::ld8(r.esi + 0x14) & 1))
    {
        rc::st32(f - 4, r.eax);
        r.eax = r.edi;
        if (!leafCall(LeafBones, app, cpu, r, f - 8))
            return false;
    }
    r.edx = rc::ld32(r.esi + 0x10);
    r.ebx = rc::ld32(r.edx);
    r.eax = rc::ld32(r.ebx + 0x2c);
    if (r.eax != rc::ld32(LastStream))
    {
        r.edi = rc::ld32(rc::ld32(CurrentGroup) + 8);
        r.ecx = rc::ld32(DevicePointer);
        r.edx = rc::ld32(r.ecx);
        rc::st32(f - 4, r.edi);
        rc::st32(f - 8, 0);
        rc::st32(f - 12, r.eax);
        rc::st32(f - 16, 0);
        rc::st32(f - 20, r.ecx);
        r.eax = bridge::setStreamSource(app, cpu, r.ecx, 0, r.eax, 0, r.edi);
        r.eax = rc::ld32(r.ebx + 0x2c);
        r.edi = rc::ld32(f + 0x10);
        store(LastStream, r.eax);
        increment(StreamChanges);
    }
    r.ecx = rc::ld32(r.ebx + 0x20);
    if (r.ecx != rc::ld32(LastIndices))
    {
        r.eax = rc::ld32(DevicePointer);
        r.edx = rc::ld32(r.eax);
        rc::st32(f - 4, r.ecx);
        rc::st32(f - 8, r.eax);
        r.eax = bridge::setIndices(app, cpu, r.eax, r.ecx);
        r.eax = rc::ld32(r.ebx + 0x20);
        store(LastIndices, r.eax);
        increment(IndexChanges);
    }
    r.eax = rc::ld32(r.edi + 0xc);
    if (r.eax == 0 || rc::ld32(r.ebx + 0x34) >= 0xffffu)
        return true;
    r.eax = rc::ld32(r.esi + 0x18);
    r.ecx = rc::ld32(r.eax + 0x14);
    r.edi = 0;
    if (r.ecx != 0)
    {
        do
        {
            r.eax = rc::ld32(r.eax + 0x18);
            r.ecx = rc::ld32(r.eax);
            rc::st32(f - 4, r.edi);
            rc::st32(f - 8, r.eax);
            const reg32 effect = r.eax, pass = r.edi;
            r.edx = rc::ld32(DevicePointer);
            r.ecx = rc::ld32(r.edx);
            r.edx = rc::ld32(f + 0x10);
            const reg32 primitives = reg32((reg64(0xaaaaaaabu) * rc::ld32(r.edx + 0xc)) >> 32) >> 1;
            r.eax = rc::ld32(f + 0x10);
            rc::st32(f - 4, primitives);
            const reg32 start = reg32(sreg32(rc::ld32(r.eax + 0x34) - rc::ld32(r.ebx + 0x18)) >> 1);
            r.eax = rc::ld32(r.ebx + 0x34);
            rc::st32(f - 8, start);
            r.edx = rc::ld32(DevicePointer);
            rc::st32(f - 12, r.eax);
            rc::st32(f - 16, 0);
            rc::st32(f - 20, 0);
            rc::st32(f - 24, 4);
            rc::st32(f - 28, r.edx);
            r.eax = bridge::passDrawIndexed(app, cpu, effect, pass, r.edx, 4, 0, 0, r.eax, start, primitives);
            ++s_usage.draws;
            r.eax = rc::ld32(r.esi + 0x18);
            r.ecx = rc::ld32(r.eax + 0x14);
            ++r.edi;
        } while (r.edi < r.ecx);
    }
    increment(IndexedPackets);
    r.edx = reg32((reg64(0xaaaaaaabu) * rc::ld32(r.ebx + 0x34)) >> 32) >> 1;
    r.eax = rc::ld32(Triangles) + r.edx;
    store(Triangles, r.eax);
    return true;
}

// User-pointer packet: z writes off, a DrawPrimitiveUP strip per pass, z writes on, the vertex block back to its
// pool.
bool drawUser(WinApplication* app, x86::CPU& cpu, Regs& r, reg32 f)
{
    r.eax = rc::ld32(DevicePointer);
    r.ecx = rc::ld32(r.eax);
    r.ebx = rc::ld32(r.esi + 8);
    r.edi = 0;
    rc::st32(f - 4, 0);
    rc::st32(f - 8, 0xe);
    rc::st32(f - 12, r.eax);
    r.eax = bridge::setRenderState(app, cpu, r.eax, 14, 0);
    r.eax = rc::ld32(r.esi + 0x18);
    r.ecx = rc::ld32(r.eax + 0x14);
    store(LastStream, r.edi);
    if (r.ecx != 0)
    {
        do
        {
            r.eax = rc::ld32(r.eax + 0x18);
            r.edx = rc::ld32(r.eax);
            rc::st32(f - 4, r.edi);
            rc::st32(f - 8, r.eax);
            r.eax = bridge::effectPass(app, cpu, r.eax, r.edi);
            r.eax = rc::ld32(DevicePointer);
            r.edx = rc::ld32(r.eax);
            rc::st32(f - 4, 0x18);
            r.ecx = r.ebx + 4;
            rc::st32(f - 8, r.ecx);
            r.ecx = rc::ld32(r.ebx) - 2;
            rc::st32(f - 12, r.ecx);
            rc::st32(f - 16, 5);
            rc::st32(f - 20, r.eax);
            r.eax = bridge::drawPrimitiveUP(app, cpu, r.eax, 5, r.ecx, rc::host(r.ebx + 4), 0x18);
            ++s_usage.draws;
            r.eax = rc::ld32(r.esi + 0x18);
            r.ecx = rc::ld32(r.eax + 0x14);
            ++r.edi;
        } while (r.edi < r.ecx);
    }
    r.eax = rc::ld32(DevicePointer);
    r.edx = rc::ld32(r.eax);
    rc::st32(f - 4, 1);
    rc::st32(f - 8, 0xe);
    rc::st32(f - 12, r.eax);
    r.eax = bridge::setRenderState(app, cpu, r.eax, 14, 1);
    increment(UserPackets);
    r.eax = rc::ld32(VertexPool);
    r.ecx = rc::ld32(r.eax + 0x18);
    r.edx = rc::ld32(r.eax + 0x10);
    store(r.ebx, r.edx);
    --r.ecx;
    store(r.eax + 0x18, r.ecx);
    store(r.eax + 0x10, r.ebx);
    return true;
}

// One packet (eax = its list position): unlinked, its setup functions, its draws, back to the packet pool.
bool drawPacket(WinApplication* app, x86::CPU& cpu, Regs& r, reg32 f)
{
    app->allowPreemption(cpu);
    r.esi = rc::ld32(r.eax);
    r.eax = rc::ld32(r.esi);
    r.ecx = rc::ld32(r.esi + 4);
    store(r.ecx, r.eax);
    store(r.eax + 4, r.ecx);
    r.eax = rc::ld32(r.esi + 0x24);
    r.ecx = rc::ld32(CurrentView);
    r.edi = rc::ld32(r.ecx + 4);
    rc::st32(f - 4, r.eax);
    if (!leafCall(LeafWorld, app, cpu, r, f - 8))
        return false;
    r.edx = rc::ld32(r.esi + 0x18);
    r.eax = rc::ld32(r.edx + 0x44);
    if (r.eax != 0)
    {
        r.ecx = rc::ld32(r.esi + 0x24);
        r.eax = ReflectionView;
        if (!leafCall(LeafReflection, app, cpu, r, f - 4))
            return false;
    }
    r.eax = rc::ld32(r.esi + 0x1c);
    if (r.eax != 0)
    {
        r.ecx = rc::ld32(r.esi + 0x24);
        rc::st32(f - 4, r.ecx);
        if (!leafCall(LeafLights, app, cpu, r, f - 8))
            return false;
    }
    r.ecx = rc::ld32(r.esi + 0x20);
    if (r.ecx != 0)
    {
        r.eax = rc::ld32(r.esi + 0x14);
        if (!leafCall(LeafMaterial, app, cpu, r, f - 4))
            return false;
    }
    r.eax = rc::ld32(r.esi + 0xc);
    if (!(r.eax == 0 ? drawIndexed(app, cpu, r, f) : drawUser(app, cpu, r, f)))
        return false;
    r.eax = rc::ld32(PacketPool);
    r.ecx = rc::ld32(r.eax + 0x18);
    r.edx = rc::ld32(r.eax + 0x10);
    --r.ecx;
    store(r.esi, r.edx);
    store(r.eax + 0x18, r.ecx);
    store(r.eax + 0x10, r.esi);
    ++s_usage.packets;
    return true;
}

// One node (esi): unlinked, its model's states, its packets, back to the node pool.
bool drawNode(WinApplication* app, x86::CPU& cpu, Regs& r, reg32 f)
{
    app->allowPreemption(cpu);
    r.ecx = rc::ld32(r.esi + 4);
    r.eax = rc::ld32(r.esi);
    store(r.ecx, r.eax);
    store(r.eax + 4, r.ecx);
    increment(NodesDrawn);
    r.edx = rc::ld32(r.esi + 0x10);
    rc::st32(f - 4, r.edx);
    if (!leafCall(LeafModel, app, cpu, r, f - 8))
        return false;
    r.ecx = rc::ld32(r.esi + 0x18);
    r.eax = r.esi + 0x18;
    if (r.ecx != r.eax)
    {
        do
        {
            if (!drawPacket(app, cpu, r, f))
                return false;
            r.eax = rc::ld32(f + 0xc);
            r.ecx = rc::ld32(r.eax + 0x18);
            r.eax += 0x18;
        } while (r.ecx != r.eax);
        r.ebx = rc::ld32(f + 0x18);
        r.edi = rc::ld32(f + 0x14);
        r.esi = rc::ld32(f + 0xc);
    }
    r.eax = rc::ld32(r.esi + 0x10);
    r.ecx = rc::ld32(r.eax);
    r.eax = r.esi;
    store(r.ecx + 0x1c, 0);
    if (!leafCall(LeafNode, app, cpu, r, f - 4))
        return false;
    r.eax = rc::ld32(NodePool);
    r.ecx = rc::ld32(r.eax + 0x18);
    r.edx = rc::ld32(r.eax + 0x10);
    store(r.esi, r.edx);
    --r.ecx;
    store(r.eax + 0x18, r.ecx);
    store(r.eax + 0x10, r.esi);
    ++s_usage.nodes;
    return true;
}

// A list head (edi) with nodes, ebx = its group + 0x18: the effect group switch when the group is not the current
// one (End, the old group's end function, the new group's start function, vertex declaration, Begin), then every
// node.
bool drawBucket(WinApplication* app, x86::CPU& cpu, Regs& r, reg32 f)
{
    r.ecx = r.ebx - 0x18;
    if (r.ecx != r.eax)
    {
        if (r.eax != 0)
        {
            r.eax = rc::ld32(r.eax + 0x18);
            r.edx = rc::ld32(r.eax);
            rc::st32(f - 4, r.eax);
            app->allowPreemption(cpu);
            r.eax = bridge::effectEnd(app, cpu, r.eax);
            r.eax = rc::ld32(CurrentGroup);
            if (!guestCall(app, cpu, r, f - 4, rc::ld32(r.eax + 0xdc)))
                return false;
        }
        r.ecx = r.ebx - 0x18;
        store(CurrentGroup, r.ecx);
        if (!guestCall(app, cpu, r, f - 4, rc::ld32(r.ebx + 0xc0)))
            return false;
        r.ecx = rc::ld32(r.ebx - 0x14);
        r.eax = rc::ld32(DevicePointer);
        r.edx = rc::ld32(r.eax);
        rc::st32(f - 4, r.ecx);
        rc::st32(f - 8, r.eax);
        r.eax = bridge::setVertexDeclaration(app, cpu, r.eax, r.ecx);
        r.eax = rc::ld32(r.ebx);
        r.edx = rc::ld32(r.eax);
        rc::st32(f - 4, 0);
        r.ecx = f + 0x1c;
        rc::st32(f - 8, r.ecx);
        rc::st32(f - 12, r.eax);
        r.eax = bridge::effectBegin(app, cpu, r.eax, reinterpret_cast<UINT*>(rc::host(r.ecx)), 0);
        increment(GroupChanges);
        r.eax = rc::ld32(CurrentGroup);
    }
    r.esi = rc::ld32(r.edi);
    rc::st32(f + 0xc, r.esi);
    if (r.esi == r.edi)
        return true;
    for (;;)
    {
        if (!drawNode(app, cpu, r, f))
            return false;
        r.eax = rc::ld32(r.edi);
        rc::st32(f + 0xc, r.eax);
        if (r.eax == r.edi)
            break;
        r.esi = r.eax;
    }
    r.eax = rc::ld32(CurrentGroup);
    return true;
}

// Every list head in order: the 7 groups of each of the 16 levels.
void flushNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 f = entry - 0x20;
    Regs r(cpu);
    rc::st32(f + 8, cpu.ebx);
    rc::st32(f + 4, cpu.esi);
    rc::st32(f, cpu.edi);
    increment(Flushes);
    r.eax = rc::ld32(CurrentGroup);
    r.edi = Heads;
    const reg32 groupsEnd = Groups + 0x18 + GroupCount * GroupSize;
    do
    {
        r.ebx = Groups + 0x18;
        rc::st32(f + 0x18, r.ebx);
        rc::st32(f + 0x14, r.edi);
        do
        {
            if (rc::ld32(r.edi) != r.edi && !drawBucket(app, cpu, r, f))
                return;
            r.ebx += GroupSize;
            r.edi += 8;
            rc::st32(f + 0x14, r.edi);
            rc::st32(f + 0x18, r.ebx);
        } while (sreg32(r.ebx) < sreg32(groupsEnd));
    } while (sreg32(r.edi) < sreg32(HeadsEnd));
    store(QueuedNodes, 0);
    cpu.eax = r.eax;
    cpu.ecx = r.ecx;
    cpu.edx = r.edx;
    cpu.edi = rc::ld32(f);
    cpu.esi = rc::ld32(f + 4);
    cpu.ebx = rc::ld32(f + 8);
    cpu.esp = entry + 4;
}

// ---- 0x40c910: model submit ----

// A recorded enqueue of the guest submit (shadow compare): eax, the 8 stack arguments and the CPU state after it.
struct EnqueueCall
{
    reg32 words[9];
    x86::CPU after;
};

struct SubmitReplay
{
    std::vector<EnqueueCall> calls;
    uint32_t next = 0;
    bool differs = false;
    char text[160] = {};
};

// Guest run of a submit shadow: its enqueues are recorded here. Native run: they are compared from here.
std::vector<EnqueueCall>* s_recording = nullptr;
SubmitReplay* s_replaying = nullptr;

void enqueueShadow(WinApplication* app, x86::CPU& cpu);

// The enqueue of the native submit (cpu.esp at the return address slot).
void enqueueFromSubmit(WinApplication* app, x86::CPU& cpu)
{
    SubmitReplay* replay = s_replaying;
    if (!replay)
    {
        enqueueNative(app, cpu);
        return;
    }
    reg32 words[9];
    words[0] = cpu.eax;
    for (reg32 i = 1; i <= 8; ++i)
        words[i] = rc::ld32(cpu.esp + 4 * i);
    if (replay->differs)
    {
        cpu.esp += 4;
        return;
    }
    if (replay->next >= replay->calls.size())
    {
        replay->differs = true;
        snprintf(replay->text, sizeof(replay->text), "enqueue %u: the guest made %u", unsigned(replay->next),
                 unsigned(replay->calls.size()));
        cpu.esp += 4;
        return;
    }
    const EnqueueCall& call = replay->calls[replay->next++];
    for (uint32_t i = 0; i < 9; ++i)
    {
        if (call.words[i] != words[i])
        {
            replay->differs = true;
            snprintf(replay->text, sizeof(replay->text), "enqueue %u word %u guest 0x%08x native 0x%08x",
                     unsigned(replay->next - 1), unsigned(i), unsigned(call.words[i]), unsigned(words[i]));
            cpu.esp += 4;
            return;
        }
    }
    cpu = call.after;
}

// [esp + 4] = the view, [esp + 8] = the model, [esp + 0xc] = the world, [esp + 0x10] = the light context,
// [esp + 0x14] = the bone palette, eax = the submit flags. Each mesh part not culled (flag 4 or part flag 1 skip
// the test) and not of the skipped model is enqueued with its group key: the model's, or the first group for
// blended parts of a key that asks for it. ret 0x14.
void submitNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 f = entry - 0x20;
    const reg32 esi0 = cpu.esi, edi0 = cpu.edi, ebx0 = cpu.ebx, ebp0 = cpu.ebp;
    Regs r(cpu);
    r.ecx = rc::ld32(entry + 8);
    rc::st32(entry - 0x14, esi0);
    rc::st32(entry - 0x18, edi0);
    r.edi = rc::ld32(r.ecx + 0xc);
    r.esi = r.eax;
    if (r.edi == 0)
    {
        cpu.ecx = r.ecx;
        cpu.esp = entry + 0x18;
        return;
    }
    rc::st32(entry - 0x1c, ebx0);
    rc::st32(entry - 0x20, ebp0);
    const reg32 ebp = rc::ld32(r.edi);
    cpu.ebp = ebp;
    if ((r.esi & 0x200) && rc::ld32(0x700e80) != 0)
        store(ebp + 0x38, rc::ld32(0x700e90) != 0 ? 0x706798u : 0x706818u);
    else if (rc::ld32(0x700e90) == 0)
        store(ebp + 0x38, 0x706898u);
    r.eax = rc::ld32(ebp + 0x38);
    r.ebx = rc::ld32(ebp + 0x3c);
    rc::st32(f + 0x10, r.ebx);
    if (r.eax != 0)
    {
        r.edx = rc::ld32(r.ebx);
        r.eax = rc::ld32(r.eax + r.edx * 4);
        if (r.eax != 0)
        {
            rc::st32(f + 0x10, r.eax);
            r.ebx = r.eax;
        }
        store(ebp + 0x38, 0);
    }
    r.eax = rc::ld32(r.ecx + 0x10);
    if (r.eax != 0)
    {
        r.eax = r.ecx;
        r.to(cpu);
        cpu.esp = f - 4;
        sub_53f540(app, cpu);
        if (cpu.terminate)
            return;
        r.from(cpu);
        r.ecx = rc::ld32(f + 0x28);
    }
    if (rc::ld32(r.ebx + 0x10) != 0)
    {
        r.eax = rc::ld32(cpu.ebp + 0x10);
        rc::st32(f + 0x18, 0);
        if (sreg32(r.eax) > 0)
        {
            r.ebx = r.esi & 4;
            rc::st32(f + 0x1c, r.ebx);
            rc::st32(f + 0x14, 0);
            do
            {
                app->allowPreemption(cpu);
                r.esi = rc::ld32(cpu.ebp + 0x14) + rc::ld32(f + 0x14);
                bool visible = true;
                if (r.ebx == 0 && !(rc::ld8(r.esi + 0x38) & 1))
                {
                    r.eax = rc::ld32(f + 0x24);
                    rc::st32(f - 4, r.eax);
                    r.eax = rc::ld32(f + 0x2c);
                    r.ecx = r.esi + 0x10;
                    r.edx = r.esi;
                    r.to(cpu);
                    cpu.esp = f - 8;
                    sub_40db20(app, cpu);
                    if (cpu.terminate)
                        return;
                    r.from(cpu);
                    visible = r.eax != 0;
                }
                if (visible)
                {
                    r.ecx = rc::ld8(r.esi + 0x1c);
                    r.edx = rc::ld32(r.edi + 0x2c);
                    r.eax = rc::ld32(r.edx + r.ecx * 8 + 4);
                    r.ecx = rc::ld32(r.eax + 0x24);
                    if (r.ecx != rc::ld32(SkippedModel))
                    {
                        const reg8 material = rc::ld8(r.esi + 0x20);
                        r.ecx = (r.ecx & ~0xffu) | material;
                        r.edx = 0;
                        if (material != 0xff)
                        {
                            r.ecx = rc::ld32(r.edi + 0x3c);
                            r.edx = rc::ld32(r.ecx + reg32(material) * 8 + 4);
                        }
                        r.ecx = rc::ld32(f + 0x10);
                        if (rc::ld32(r.ecx) == 1 && rc::ld32(r.ecx + 0xc) > 1)
                        {
                            const reg8 blended = rc::ld8(r.eax + 0x56);
                            r.ebx = (r.ebx & ~0xffu) | blended;
                            if (blended != 0)
                                r.ecx = Groups;
                        }
                        r.ebx = rc::ld32(f + 0x34);
                        rc::st32(f - 4, r.ebx);
                        rc::st32(f - 8, r.edx);
                        r.edx = rc::ld32(f + 0x30);
                        rc::st32(f - 12, r.edx);
                        r.edx = rc::ld32(f + 0x2c);
                        rc::st32(f - 16, r.edx);
                        rc::st32(f - 20, r.ecx);
                        r.ecx = rc::ld32(r.esi + 0x38);
                        rc::st32(f - 24, r.ecx);
                        rc::st32(f - 28, r.edi);
                        rc::st32(f - 32, r.esi);
                        r.to(cpu);
                        cpu.esp = f - 36;
                        enqueueFromSubmit(app, cpu);
                        if (cpu.terminate)
                            return;
                        r.from(cpu);
                        r.ebx = rc::ld32(f + 0x1c);
                    }
                }
                r.eax = rc::ld32(f + 0x18) + 1;
                r.edx = rc::ld32(f + 0x14) + 0x3c;
                r.ecx = rc::ld32(cpu.ebp + 0x10);
                rc::st32(f + 0x18, r.eax);
                rc::st32(f + 0x14, r.edx);
            } while (sreg32(r.eax) < sreg32(r.ecx));
            r.ecx = rc::ld32(f + 0x28);
        }
    }
    r.eax = r.ecx;
    r.to(cpu);
    cpu.esp = f - 4;
    sub_53f500(app, cpu);
    if (cpu.terminate)
        return;
    ++s_usage.submits;
    cpu.ebp = rc::ld32(f);
    cpu.ebx = rc::ld32(f + 4);
    cpu.edi = rc::ld32(f + 8);
    cpu.esi = rc::ld32(f + 0xc);
    cpu.esp = entry + 0x18;
}

// ---- Shadow compares ----

// One per function: the submit compare runs the enqueue compares inside its guest run.
struct Shadow
{
    bridge::Capture capture;
    std::vector<Span> spans;
    std::vector<reg8> before, guestAfter, nativeAfter;
};

Shadow s_enqueueShadow, s_submitShadow, s_flushShadow;
uint32_t s_logged = 0;

void report(const char* what, const x86::CPU& entry, bool same, const char* text)
{
    ++s_usage.compared;
    if (same)
        return;
    ++s_usage.differing;
    if (s_logged < ShadowLogLimit)
    {
        ++s_logged;
        pf::logInfo("shadow_draw: %s call %u (esp 0x%08x): %s", what, unsigned(s_usage.compared),
                    unsigned(entry.esp), text);
    }
}

// Guest memory the enqueue may store to (false: an allocation would expand a pool, not compared).
bool enqueueSpans(const x86::CPU& cpu, std::vector<Span>& spans)
{
    spans.clear();
    const reg32 entry = cpu.esp;
    const reg32 model = cpu.eax;
    const reg32 key = rc::ld32(entry + 0x10);
    const reg32 holder = rc::ld32(model);
    const reg32 nodePool = rc::ld32(NodePool), packetPool = rc::ld32(PacketPool);
    if (nodePool == packetPool)
        return false;
    const auto slotSize = [](reg32 pool, reg32 size)
    {
        if (!(rc::ld8(pool + 0x14) & 2))
            return size;
        const sreg32 words = sreg32(rc::ld32(pool + 0x28)) >> 2;
        return words > 0 && reg32(words) * 4 > size ? reg32(words) * 4 : size;
    };
    spans.push_back(Span{holder + 0x1c, 4});
    spans.push_back(Span{QueuedNodes, 4});
    spans.push_back(Span{nodePool + 0x10, 0x10});
    spans.push_back(Span{packetPool + 0x10, 0x10});
    const reg32 node = rc::ld32(holder + 0x1c);
    if (node == 0 || rc::ld32(node + 0x14) != key)
    {
        const reg32 slot = rc::ld32(nodePool + 0x10);
        if (slot == 0)
            return false;
        spans.push_back(Span{slot, slotSize(nodePool, 0x20)});
        const reg32 index = reg32(sreg32(sreg8(rc::ld8(model + 0x51)))) * 7 + rc::ld32(key);
        const reg32 head = index * 8 + (rc::ld8(model + 0x54) ? SecondHeads : Heads);
        spans.push_back(Span{head, 8});
        spans.push_back(Span{rc::ld32(head + 4), 8});
    }
    else
    {
        spans.push_back(Span{node, 0x20});
        spans.push_back(Span{rc::ld32(node + 0x1c), 8});
    }
    const reg32 packet = rc::ld32(packetPool + 0x10);
    if (packet == 0)
        return false;
    spans.push_back(Span{packet, slotSize(packetPool, 0x2c)});
    return true;
}

// Guest memory the flush may store to: its counters and caches, the setup functions' caches, the list heads,
// every node and packet, the pools, the vertex blocks of user-pointer packets and the models' node pointers.
void flushSpans(std::vector<Span>& spans)
{
    spans.clear();
    spans.push_back(Span{NodesDrawn, CountersEnd - NodesDrawn});
    spans.push_back(Span{CurrentGroup, 4});
    spans.push_back(Span{LastStream, 8});
    spans.push_back(Span{QueuedNodes, 4});
    spans.push_back(Span{Heads, HeadsEnd - Heads});
    spans.push_back(Span{LastView, 4});
    spans.push_back(Span{LastGroup, 8});
    spans.push_back(Span{LastModel, 4});
    spans.push_back(Span{AlphaRef, 20});
    for (const reg32 pool : { NodePool, PacketPool, VertexPool })
    {
        const reg32 p = rc::ld32(pool);
        spans.push_back(Span{p + 0x10, 4});
        spans.push_back(Span{p + 0x18, 4});
    }
    for (reg32 head = Heads; head < HeadsEnd; head += 8)
    {
        reg32 node = rc::ld32(head);
        for (uint32_t n = 0; node != head && n < ListLimit; ++n, node = rc::ld32(node))
        {
            spans.push_back(Span{node, 0x20});
            spans.push_back(Span{rc::ld32(rc::ld32(node + 0x10)) + 0x1c, 4});
            const reg32 list = node + 0x18;
            reg32 packet = rc::ld32(list);
            for (uint32_t k = 0; packet != list && k < ListLimit; ++k, packet = rc::ld32(packet))
            {
                spans.push_back(Span{packet, 8});
                if (rc::ld32(packet + 0xc) != 0)
                    spans.push_back(Span{rc::ld32(packet + 8), 4});
            }
        }
    }
}

// Guest memory the submit itself stores to (its enqueues are compared as calls).
void submitSpans(const x86::CPU& cpu, std::vector<Span>& spans)
{
    spans.clear();
    const reg32 holder = rc::ld32(rc::ld32(cpu.esp + 8) + 0xc);
    if (holder != 0)
        spans.push_back(Span{rc::ld32(holder) + 0x38, 4});
}

// The guest run of a compare: memory restored to the entry state for the native run, which runs with its stores
// logged; then the guest's result is put back and both are compared.
struct Compare
{
    const char* what;
    Shadow& s;
    x86::CPU entry, guest, native;

    Compare(const char* name, Shadow& shadow, const x86::CPU& cpu) : what(name), s(shadow), entry(cpu) {}

    void runNative(WinApplication* app, x86::CPU& cpu, void (*run)(WinApplication*, x86::CPU&), bool replay)
    {
        guest = cpu;
        snapshot(s.spans, s.guestAfter);
        restoreSpans(s.spans, s.before);
        cpu = entry;
        g_stores.clear();
        g_logStores = true;
        if (replay)
            bridge::begin(s.capture, bridge::Capture::Replay);
        run(app, cpu);
        if (replay)
            bridge::end();
        g_logStores = false;
        native = cpu;
        snapshot(s.spans, s.nativeAfter);
        restoreSpans(s.spans, s.guestAfter);
        cpu = guest;
    }
};

void enqueueShadow(WinApplication* app, x86::CPU& cpu)
{
    Shadow& s = s_enqueueShadow;
    if (!enqueueSpans(cpu, s.spans))
    {
        ++s_usage.skipped;
        sub_40f1e0_guest(app, cpu);
        return;
    }
    Compare c("enqueue 0x40f1e0", s, cpu);
    snapshot(s.spans, s.before);
    sub_40f1e0_guest(app, cpu);
    if (cpu.terminate)
        return;
    const uint32_t enqueues = s_usage.enqueues;
    c.runNative(app, cpu, &enqueueNative, false);
    s_usage.enqueues = enqueues;
    char text[200] = {};
    const bool same = sameState(0x40f1e0, RegEbx | RegEsp | RegEbp | RegEsi | RegEdi, c.guest, c.native, text,
                                sizeof(text))
                      && sameMemory(s.spans, s.guestAfter, s.nativeAfter, text, sizeof(text));
    report(c.what, c.entry, same, text);
}

void submitShadow(WinApplication* app, x86::CPU& cpu)
{
    Shadow& s = s_submitShadow;
    submitSpans(cpu, s.spans);
    Compare c("submit 0x40c910", s, cpu);
    snapshot(s.spans, s.before);
    std::vector<EnqueueCall> calls;
    s_recording = &calls;
    sub_40c910_guest(app, cpu);
    s_recording = nullptr;
    if (cpu.terminate)
        return;
    SubmitReplay replay;
    replay.calls.swap(calls);
    s_replaying = &replay;
    const uint32_t submits = s_usage.submits;
    c.runNative(app, cpu, &submitNative, false);
    s_usage.submits = submits;
    s_replaying = nullptr;
    char text[200] = {};
    bool same = !replay.differs;
    if (!same)
        snprintf(text, sizeof(text), "%s", replay.text);
    else if (replay.next != replay.calls.size())
    {
        same = false;
        snprintf(text, sizeof(text), "enqueues guest %u native %u", unsigned(replay.calls.size()),
                 unsigned(replay.next));
    }
    same = same && sameState(0x40c910, RegEcx | RegEbx | RegEsp | RegEbp | RegEsi | RegEdi, c.guest, c.native,
                             text, sizeof(text))
           && sameMemory(s.spans, s.guestAfter, s.nativeAfter, text, sizeof(text));
    report(c.what, c.entry, same, text);
}

void flushShadow(WinApplication* app, x86::CPU& cpu)
{
    Shadow& s = s_flushShadow;
    flushSpans(s.spans);
    Compare c("flush 0x40ed50", s, cpu);
    snapshot(s.spans, s.before);

    // The effect group functions go through the callouts while the guest code runs.
    reg32 slots[bridge::CalloutCount], targets[bridge::CalloutCount];
    uint32_t swapped = 0;
    const auto swap = [&](reg32 slot)
    {
        for (uint32_t i = 0; i < swapped; ++i)
            if (slots[i] == slot)
                return;
        if (swapped == bridge::CalloutCount)
            return;
        slots[swapped] = slot;
        targets[swapped] = rc::ld32(slot);
        bridge::setCalloutTarget(swapped, targets[swapped]);
        rc::st32(slot, bridge::calloutAddress(swapped));
        ++swapped;
    };
    for (reg32 k = 0; k < GroupCount; ++k)
    {
        swap(Groups + k * GroupSize + 0xdc);
        swap(Groups + k * GroupSize + 0xd8);
    }
    const reg32 current = rc::ld32(CurrentGroup);
    if (current != 0)
        swap(current + 0xdc);
    s.capture.clear();
    bridge::begin(s.capture, bridge::Capture::Record);
    sub_40ed50_guest(app, cpu);
    bridge::end();
    for (uint32_t i = swapped; i-- > 0;)
        rc::st32(slots[i], targets[i]);
    if (cpu.terminate)
        return;
    const QueueUsage counts = s_usage;
    c.runNative(app, cpu, &flushNative, true);
    s_usage.packets = counts.packets;
    s_usage.draws = counts.draws;
    s_usage.nodes = counts.nodes;
    char text[200] = {};
    const bool same = sameCalls(s.capture, text, sizeof(text))
                      && sameState(0x40ed50, 0xff & ~RegEbp, c.guest, c.native, text, sizeof(text))
                      && sameMemory(s.spans, s.guestAfter, s.nativeAfter, text, sizeof(text));
    report(c.what, c.entry, same, text);
}

// Perf builds, guest flush: its nodes and packets, counted before it runs.
void countQueue()
{
    for (reg32 head = Heads; head < HeadsEnd; head += 8)
    {
        reg32 node = rc::ld32(head);
        for (uint32_t n = 0; node != head && n < ListLimit; ++n, node = rc::ld32(node))
        {
            ++s_usage.nodes;
            const reg32 list = node + 0x18;
            reg32 packet = rc::ld32(list);
            for (uint32_t k = 0; packet != list && k < ListLimit; ++k, packet = rc::ld32(packet))
                ++s_usage.packets;
        }
    }
}

}

QueueUsage takeQueueUsage()
{
    const QueueUsage u = s_usage;
    s_usage = QueueUsage{};
    return u;
}

bool nativeQueue()
{
    return settings().native;
}

void logDeferral(unsigned frames)
{
    if (!frames || settings().async == bridge::DeferOff)
        return;
    const bridge::DeferStats d = bridge::takeDeferStats();
    char errors[512] = {};
    int at = 0;
    for (uint32_t op = 0; op < bridge::DeferOpCount && at < int(sizeof(errors)); ++op)
    {
        if (d.errors[op])
            at += snprintf(errors + at, sizeof(errors) - at, " %s %.1f/frame (0x%08x),", bridge::deferOpName(op),
                           double(d.errors[op]) / frames, unsigned(d.errorCodes[op]));
    }
    char sites[512] = {};
    int siteAt = 0;
    for (uint32_t i = 0; i < bridge::DeferSiteCount && siteAt < int(sizeof(sites)); ++i)
    {
        if (d.sites[i].count)
            siteAt += snprintf(sites + siteAt, sizeof(sites) - siteAt, " %s %.1f/frame %.2f ms/frame,",
                               bridge::drainSiteName(d.sites[i].address), double(d.sites[i].count) / frames,
                               double(d.sites[i].us) / 1000.0 / frames);
    }
    pf::logInfo("flush_async: drains that waited, by method:%s", siteAt ? sites : " none");
    pf::logInfo("flush_async: %u flushes/frame, %u queued calls/frame (%u KiB), %u ran at once; queue run %.2f ms/frame;"
                " drains that waited %u/frame (%u inside a flush), wait %.2f ms/frame; in these frames: results that"
                " differ from the returned ones %u, Begin pass counts that differ %u, late deletes %u; results not"
                " D3D_OK by method:%s", d.windows / frames, d.calls / frames, d.words / frames / 256,
                d.fallbacks / frames, double(d.runUs) / 1000.0 / frames, d.drains / frames, d.windowDrains / frames,
                double(d.waitUs) / 1000.0 / frames, unsigned(d.mismatches), unsigned(d.passes), unsigned(d.deletes),
                at ? errors : " none");
}

}

namespace native
{

void sub_40ed50(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::Flush);
    const draw::Settings& s = draw::settings();
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    if (!s.native || win32::d3d9::bridge::capturing())
    {
        if (timed)
            draw::countQueue();
        sub_40ed50_guest(app, cpu);
    }
    else if (s.shadow)
    {
        if (timed)
            draw::countQueue();
        draw::flushShadow(app, cpu);
    }
    else
    {
        const bool deferred = s.async != win32::d3d9::bridge::DeferOff
                              && win32::d3d9::bridge::deferBegin(app, cpu, rc::ld32(draw::DevicePointer));
        draw::flushNative(app, cpu);
        if (deferred)
            win32::d3d9::bridge::deferEnd();
    }
    ++draw::s_usage.flushes;
    if (timed)
        draw::s_usage.flushUs += pf::perf::now() - start;
}

void sub_40f1e0(win32::WinApplication* app, x86::CPU& cpu)
{
    const draw::Settings& s = draw::settings();
    if (draw::s_recording)
    {
        draw::EnqueueCall call;
        call.words[0] = cpu.eax;
        for (x86::reg32 i = 1; i <= 8; ++i)
            call.words[i] = rc::ld32(cpu.esp + 4 * i);
        draw::enqueueShadow(app, cpu);
        call.after = cpu;
        draw::s_recording->push_back(call);
        return;
    }
    if (!s.native)
    {
        ++draw::s_usage.enqueues;
        sub_40f1e0_guest(app, cpu);
    }
    else if (s.shadow && !win32::d3d9::bridge::capturing())
    {
        ++draw::s_usage.enqueues;
        draw::enqueueShadow(app, cpu);
    }
    else
    {
        draw::enqueueNative(app, cpu);
    }
}

void sub_40c910(win32::WinApplication* app, x86::CPU& cpu)
{
    const draw::Settings& s = draw::settings();
    if (!s.native)
    {
        ++draw::s_usage.submits;
        sub_40c910_guest(app, cpu);
    }
    else if (s.shadow && !win32::d3d9::bridge::capturing())
    {
        ++draw::s_usage.submits;
        draw::submitShadow(app, cpu);
    }
    else
    {
        draw::submitNative(app, cpu);
    }
}

}
}
