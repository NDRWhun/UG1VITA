// Native setup functions of the draw queue flush (draw.h): world transforms 0x410400, blend states 0x410550, alpha
// test 0x410650, packet and node release 0x410700 / 0x40ed10, effect group switch 0x40e6d0, bone palette 0x4102f0,
// model texture and texture offset 0x40f3d0, light context 0x40f7c0, reflection matrices 0x4100a0 and material
// 0x40fae0. Guest functions they call that have no native version (0x40ec20, 0x40ec90, 0x546180, 0x565020, 0x40f6e0,
// 0x40bc10 and the effect group functions) run as guest code with the stack and registers the guest gives them.

#include "draw.h"
#include "retstate.h"

#include <winapi/d3d9_native.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <recomp.h>
#include <vector>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_410400_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_410550_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_410650_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_410700_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40ed10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40e6d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4102f0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40f3d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40f7c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4100a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40fae0_guest(win32::WinApplication* app, x86::CPU& cpu);
// Guest functions the native code calls (generated).
void sub_40ec20(win32::WinApplication* app, x86::CPU& cpu);
void sub_40ec90(win32::WinApplication* app, x86::CPU& cpu);
void sub_546180(win32::WinApplication* app, x86::CPU& cpu);
void sub_565020(win32::WinApplication* app, x86::CPU& cpu);
void sub_40f6e0(win32::WinApplication* app, x86::CPU& cpu);
void sub_40bc10(win32::WinApplication* app, x86::CPU& cpu);

namespace draw
{

namespace
{

using win32::DWORD;
using win32::UINT;
using win32::WinApplication;
using x86::reg8;
using x86::reg16;
using x86::reg32;
using x86::reg64;
using x86::sreg8;
using x86::sreg16;
using x86::sreg32;
using x86::sreg64;
namespace bridge = win32::d3d9::bridge;

// Guest data.
const reg32 CurrentGroup = 0x73640c;    // the effect group being drawn
const reg32 DevicePointer = 0x73636c;   // IDirect3DDevice9
const reg32 PacketPool = 0x7363d8;      // slot pool of the draw packets: free list +0x10, used count +0x18
const reg32 ViewRecords = 0x740460;     // 0x60 bytes per view: view matrix, projection, view projection
const reg32 CurrentView = 0x736458;     // view index at +4
const reg32 LastView = 0x7010b8;
const reg32 LastGroup = 0x78eb7c;
const reg32 LastWorld = 0x78eb80;
const reg32 LastModel = 0x736404;
const reg32 LastModelGroup = 0x736408;
const reg32 AlphaRef = 0x7070c0;        // alpha reference, alpha test, blend destination, blend source, blend on
const reg32 AlphaTestOn = 0x7070c4;
const reg32 BlendDestination = 0x7070c8;
const reg32 BlendSource = 0x7070cc;
const reg32 BlendOn = 0x7070d0;
const reg32 TransformsOn = 0x736414;    // fixed-function transforms
const reg32 EffectMatricesOn = 0x736418;
const reg32 SharedWorld = 0x78bcc0;     // the world matrix of untransformed models
const reg32 Zero = 0x6cc7a4;            // float constants in .rdata
const reg32 Half = 0x6cc7dc;
const reg32 ByteScale = 0x6cc974;       // 255

// Draw statistics the guest counts.
const reg32 WorldCalls = 0x71b1f4;
const reg32 WorldChanges = 0x71b1f8;
const reg32 ViewChanges = 0x71b1fc;
const reg32 SharedWorldCalls = 0x71b200;

// D3DRS values.
enum : reg32
{
    RsZWriteEnable = 14, RsAlphaTestEnable = 15, RsSrcBlend = 19, RsDestBlend = 20, RsCullMode = 22,
    RsAlphaRef = 24, RsAlphaBlendEnable = 27, RsTextureFactor = 60
};

const uint32_t SampleMask = 63;         // one call in 64 is timed
const uint32_t ShadowLogLimit = 20;

inline double ld(reg32 address) { return double(rc::ldf32(address)); }
inline sreg32 ld16s(reg32 address) { return sreg32(sreg16(rc::ld16(address))); }
inline reg32 bits(float value) { reg32 r; memcpy(&r, &value, 4); return r; }
inline float asFloat(reg32 value) { float f; memcpy(&f, &value, 4); return f; }
inline const float* guestFloats(reg32 address) { return reinterpret_cast<const float*>(rc::host(address)); }
inline const DWORD* guestWords(reg32 address) { return reinterpret_cast<const DWORD*>(rc::host(address)); }
inline void loadMatrix(reg32 address, float* m) { memcpy(m, rc::host(address), 64); }

struct Settings
{
    bool native;
    bool shadow;
    bool defer;     // matrix products multiplied by the render thread
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("draw_native", false), pf::Config::getBool("shadow_draw", false),
                             pf::Config::getBool("matrices_on_render_thread", true) };
        pf::logInfo("draw: setup functions %s%s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "",
                    r.native && r.defer ? ", matrix products on the render thread" : "");
        return r;
    }();
    return s;
}

// _ftol2 at a guest call: the value goes to the x87 slot above the stack, edx:eax gets it truncated to an integer.
inline reg32 ftol2(x86::CPU& cpu, reg32 depth, double value)
{
    cpu.fpu.regs[(depth + 1) & 7] = value;
    cpu.edx_eax = reg64(x86::truncInteger64(value));
    return cpu.eax;
}

// The result the guest function left on the x87 stack, popped.
inline double popResult(x86::CPU& cpu, reg32 depth)
{
    const double value = cpu.fpu.regs[(depth + 1) & 7];
    cpu.fpu.count = depth;
    return value;
}

// Registers callee-saved by a guest function, restored at its return.
struct Saved
{
    reg32 ebx, ebp, esi, edi;

    explicit Saved(const x86::CPU& cpu) : ebx(cpu.ebx), ebp(cpu.ebp), esi(cpu.esi), edi(cpu.edi) {}
    void restore(x86::CPU& cpu) const
    {
        cpu.ebx = ebx;
        cpu.ebp = ebp;
        cpu.esi = esi;
        cpu.edi = edi;
    }
};

// ---- 0x410650, 0x410550: alpha test and blend states ----

// Alpha test (guest 0x410650): render states 15 and 24, or BaseAlphaTestEnable and BaseAlphaRef of the effect.
// eax, ecx, edx as the guest leaves them.
void alphaTest(WinApplication* app, x86::CPU& cpu, reg32 enable, reg32 reference)
{
    const reg32 group = rc::ld32(CurrentGroup);
    const reg32 on = enable ? 1 : 0;
    if (group == 0)
        bridge::setRenderState(app, cpu, rc::ld32(DevicePointer), RsAlphaTestEnable, on);
    else
        bridge::effectSetInt(app, cpu, rc::ld32(group + 0x18), rc::ld32(group + 0xbc), sreg32(on));
    store(AlphaTestOn, enable);
    reg32 ecx = group, edx;
    if (group == 0)
    {
        const reg32 device = rc::ld32(DevicePointer);
        cpu.eax = bridge::setRenderState(app, cpu, device, RsAlphaRef, reference);
        edx = rc::ld32(device);
    }
    else
    {
        const reg32 effect = rc::ld32(group + 0x18);
        ecx = rc::ld32(group + 0xc0);
        cpu.eax = bridge::effectSetInt(app, cpu, effect, ecx, sreg32(reference));
        edx = rc::ld32(effect);
    }
    store(AlphaRef, reference);
    cpu.ecx = ecx;
    cpu.edx = edx;
}

// Blending (guest 0x410550): render states 27, 19, 20, or BaseAlphaBlendEnable, BaseSrcBlend and BaseDestBlend of the
// effect. eax, ecx, edx as the guest leaves them.
void blend(WinApplication* app, x86::CPU& cpu, reg32 on, reg32 source, reg32 destination)
{
    const reg32 group = rc::ld32(CurrentGroup);
    reg32 ecx, edx = cpu.edx;
    if (on == 0)
    {
        store(BlendSource, 0xffffffffu);
        store(BlendDestination, 0xffffffffu);
        if (group == 0)
        {
            const reg32 device = rc::ld32(DevicePointer);
            cpu.eax = bridge::setRenderState(app, cpu, device, RsAlphaBlendEnable, 0);
            ecx = rc::ld32(device);
        }
        else
        {
            const reg32 effect = rc::ld32(group + 0x18);
            ecx = rc::ld32(group + 0xc4);
            cpu.eax = bridge::effectSetInt(app, cpu, effect, ecx, 0);
            edx = rc::ld32(effect);
        }
        store(BlendOn, 0);
    }
    else
    {
        if (group == 0)
            bridge::setRenderState(app, cpu, rc::ld32(DevicePointer), RsAlphaBlendEnable, 1);
        else
            bridge::effectSetInt(app, cpu, rc::ld32(group + 0x18), rc::ld32(group + 0xc4), 1);
        store(BlendOn, on);
        if (group == 0)
        {
            const reg32 device = rc::ld32(DevicePointer);
            bridge::setRenderState(app, cpu, device, RsSrcBlend, source);
            cpu.eax = bridge::setRenderState(app, cpu, rc::ld32(DevicePointer), RsDestBlend, destination);
            ecx = edx = rc::ld32(rc::ld32(DevicePointer));
        }
        else
        {
            bridge::effectSetInt(app, cpu, rc::ld32(group + 0x18), rc::ld32(group + 0xc8), sreg32(source));
            const reg32 effect = rc::ld32(group + 0x18);
            ecx = rc::ld32(group + 0xcc);
            cpu.eax = bridge::effectSetInt(app, cpu, effect, ecx, sreg32(destination));
            edx = rc::ld32(effect);
        }
        store(BlendSource, source);
        store(BlendDestination, destination);
    }
    cpu.ecx = ecx;
    cpu.edx = edx;
}

void alphaTestNative(WinApplication* app, x86::CPU& cpu)
{
    alphaTest(app, cpu, cpu.edi, cpu.esi);
    cpu.esp += 4;
}

void blendNative(WinApplication* app, x86::CPU& cpu)
{
    blend(app, cpu, rc::ld32(cpu.esp + 4), cpu.edi, cpu.esi);
    cpu.esp += 4;
}

// ---- 0x410400: world, view and projection ----

// eax, ecx, edx as the guest leaves them; edi = the view, [esp + 4] = the world matrix.
void worldNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 world = rc::ld32(entry + 4);
    const reg32 view = cpu.edi;
    cpu.esp = entry + 4;
    store(WorldCalls, rc::ld32(WorldCalls) + 1);
    if (world == SharedWorld)
        store(SharedWorldCalls, rc::ld32(SharedWorldCalls) + 1);
    const reg32 group = rc::ld32(CurrentGroup);
    const reg32 lastView = rc::ld32(LastView);
    if (world == rc::ld32(LastWorld) && group == rc::ld32(LastGroup) && lastView == view)
    {
        cpu.eax = lastView;
        cpu.ecx = group;
        return;
    }
    const reg32 record = rc::ld32(ViewRecords + view * 0x60);
    reg32 edx = record + 0x80;
    if (view != lastView)
    {
        if (rc::ld32(TransformsOn))
        {
            const reg32 device = rc::ld32(DevicePointer);
            bridge::setTransform(app, cpu, device, 2, guestFloats(record));
            bridge::setTransform(app, cpu, rc::ld32(DevicePointer), 3, guestFloats(record + 0x40));
            edx = rc::ld32(rc::ld32(DevicePointer));
        }
        store(LastView, view);
        store(ViewChanges, rc::ld32(ViewChanges) + 1);
    }
    store(WorldChanges, rc::ld32(WorldChanges) + 1);
    const reg32 kind = rc::ld32(group);
    store(LastGroup, group);
    store(LastWorld, world);
    const bool defer = settings().defer;
    float a[16], b[16], m[16];
    if (kind == 5 && rc::ld32(EffectMatricesOn))
    {
        loadMatrix(world, a);
        loadMatrix(record, b);
        const reg32 g = rc::ld32(CurrentGroup);
        if (defer)
        {
            bridge::effectSetMatrixProduct(app, cpu, rc::ld32(g + 0x18), rc::ld32(g + 0x24), a, b, nullptr);
        }
        else
        {
            bridge::matrixMultiply(a, b, m);
            bridge::effectSetMatrix(app, cpu, rc::ld32(g + 0x18), rc::ld32(g + 0x24), m);
        }
    }
    if (rc::ld32(TransformsOn))
    {
        const reg32 device = rc::ld32(DevicePointer);
        bridge::setTransform(app, cpu, device, 0x100, guestFloats(world));
        edx = rc::ld32(device);
    }
    const reg32 on = rc::ld32(EffectMatricesOn);
    reg32 eax = on, ecx = group;
    if (on)
    {
        loadMatrix(world, a);
        loadMatrix(record + 0x80, b);
        const reg32 g = rc::ld32(CurrentGroup);
        const reg32 effect = rc::ld32(g + 0x18);
        ecx = rc::ld32(g + 0x1c);
        if (defer)
        {
            eax = bridge::effectSetMatrixProduct(app, cpu, effect, ecx, a, b, nullptr);
        }
        else
        {
            bridge::matrixMultiply(a, b, m);
            eax = bridge::effectSetMatrix(app, cpu, effect, ecx, m);
        }
        edx = rc::ld32(effect);
    }
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.edx = edx;
}

// ---- 0x410700, 0x40ed10: packets back to their pool ----

// Unlinks every packet of the list at head and pushes it onto the packet pool's free list; edx as the guest leaves it.
void freePackets(WinApplication* app, x86::CPU& cpu, reg32 head)
{
    reg32 node = rc::ld32(head);
    if (node == head)
        return;
    const reg32 pool = rc::ld32(PacketPool);
    for (;;)
    {
        const reg32 next = rc::ld32(node), previous = rc::ld32(node + 4);
        store(previous, next);
        store(next + 4, previous);
        const reg32 used = rc::ld32(pool + 0x18) - 1;
        store(node, rc::ld32(pool + 0x10));
        store(pool + 0x18, used);
        store(pool + 0x10, node);
        cpu.edx = used;
        node = rc::ld32(head);
        if (node == head)
            break;
        app->allowPreemption(cpu);
    }
}

void packetsNative(WinApplication* app, x86::CPU& cpu)
{
    freePackets(app, cpu, cpu.edi);
    cpu.esp += 4;
}

void nodeNative(WinApplication* app, x86::CPU& cpu)
{
    freePackets(app, cpu, cpu.eax + 0x18);
    cpu.esp += 4;
}

// ---- 0x40e6d0: effect group switch ----

// eax = the group, edi = the pass to start or -1. The group functions run as guest code.
void groupNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 group = cpu.eax;
    const reg32 entryEcx = cpu.ecx, entryEsi = cpu.esi;
    const reg32 current = rc::ld32(CurrentGroup);
    if (current != 0)
    {
        if (current == group)
        {
            cpu.esp = entry + 4;
            return;
        }
        const reg32 effect = rc::ld32(current + 0x18);
        cpu.ecx = rc::ld32(effect);
        cpu.esi = group;
        cpu.eax = bridge::effectEnd(app, cpu, effect);
        cpu.edx = rc::ld32(CurrentGroup);
        cpu.esp = entry - 12;
        rc::st32(entry - 12, 0x0040e6f9);
        bridge::callGuest(app, cpu, rc::ld32(cpu.edx + 0xdc));
        if (cpu.terminate)
            return;
    }
    else
    {
        cpu.eax = 0;
        cpu.esi = group;
    }
    const reg32 esi = cpu.esi;
    store(CurrentGroup, esi);
    cpu.esp = entry - 12;
    rc::st32(entry - 12, 0x0040e705);
    bridge::callGuest(app, cpu, rc::ld32(esi + 0xd8));
    if (cpu.terminate)
        return;
    const reg32 next = cpu.esi;
    bridge::setVertexDeclaration(app, cpu, rc::ld32(DevicePointer), rc::ld32(next + 4));
    UINT passes = entryEcx;
    cpu.eax = bridge::effectBegin(app, cpu, rc::ld32(next + 0x18), &passes, 0);
    const reg32 pass = cpu.edi;
    if (pass != 0xffffffffu)
        cpu.eax = bridge::effectPass(app, cpu, rc::ld32(next + 0x18), pass);
    cpu.ecx = passes;
    cpu.edx = entry - 4;
    cpu.esi = entryEsi;
    cpu.esp = entry + 4;
}

// ---- 0x4102f0: bone palette ----

// eax = the mesh (palette indices at +0x21), [esp + 4] = the palette of 64-byte matrices.
void bonesNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 palette = rc::ld32(entry + 4);
    const reg32 indices = cpu.eax + 0x21;
    float matrices[16 * 16];
    for (reg32 i = 0; i < 16; ++i)
        memcpy(matrices + 16 * i, rc::host(palette + (reg32(rc::ld8(indices + i)) << 6)), 64);
    app->allowPreemption(cpu);
    const reg32 group = rc::ld32(CurrentGroup);
    const reg32 effect = rc::ld32(group + 0x18);
    const reg32 handle = rc::ld32(group + 0x34);
    cpu.eax = bridge::effectSetMatrixArray(app, cpu, effect, handle, matrices, 16);
    cpu.ecx = handle;
    cpu.edx = rc::ld32(effect);
    cpu.ebx = palette;
    cpu.esp = entry + 4;
}

// ---- 0x40f3d0: model texture, states and texture offset ----

// [esp + 4] = the model. ebx, esi, edi, ebp are saved; edx and the x87 status as the guest leaves them.
void modelNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 depth = cpu.fpu.count;
    const Saved saved(cpu);
    const reg32 frame = (entry - 4) & ~15u;
    const reg32 s = frame - 0x70;
    const reg32 model = rc::ld32(entry + 4);
    const reg32 group = rc::ld32(CurrentGroup);
    cpu.esp = entry + 4;
    if (rc::ld32(LastModel) == model && rc::ld32(LastModelGroup) == group)
    {
        cpu.edx = group;
        return;
    }
    const reg32 texture = rc::ld32(rc::ld32(model) + 0x18);
    store(LastModel, model);
    if (texture == 0)
    {
        cpu.edx = group;
        return;
    }
    if (group != 0)
        bridge::effectSetTexture(app, cpu, rc::ld32(group + 0x18), rc::ld32(group + 0x40), texture);
    else
        bridge::setTexture(app, cpu, rc::ld32(DevicePointer), 0, texture);

    const bool alpha = rc::ld8(model + 0x55) == 1;
    alphaTest(app, cpu, alpha ? 1 : 0, alpha ? 0xaa : 0);
    reg32 edx = cpu.edx;
    const sreg8 mode = sreg8(rc::ld8(model + 0x56));
    if (mode == 0)
    {
        const reg32 g = rc::ld32(CurrentGroup);
        store(BlendSource, 0xffffffffu);
        store(BlendDestination, 0xffffffffu);
        if (g == 0)
        {
            bridge::setRenderState(app, cpu, rc::ld32(DevicePointer), RsAlphaBlendEnable, 0);
        }
        else
        {
            const reg32 effect = rc::ld32(g + 0x18);
            bridge::effectSetInt(app, cpu, effect, rc::ld32(g + 0xc4), 0);
            edx = rc::ld32(effect);
        }
        store(BlendOn, 0);
    }
    else
    {
        blend(app, cpu, 1, 5, mode == 2 ? 2 : 6);
        edx = cpu.edx;
    }

    const bool kind5 = rc::ld32(rc::ld32(CurrentGroup)) == 5;
    if (mode == 0)
    {
        if (kind5)
            bridge::setRenderState(app, cpu, rc::ld32(DevicePointer), RsCullMode, 1);
        const reg32 device = rc::ld32(DevicePointer);
        bridge::setRenderState(app, cpu, device, RsZWriteEnable, 1);
        edx = rc::ld32(device);
    }
    else if (kind5)
    {
        bridge::setRenderState(app, cpu, rc::ld32(DevicePointer), RsCullMode, 2);
        const reg32 device = rc::ld32(DevicePointer);
        bridge::setRenderState(app, cpu, device, RsZWriteEnable, 1);
        edx = rc::ld32(device);
    }
    else
    {
        bridge::setRenderState(app, cpu, rc::ld32(DevicePointer), RsZWriteEnable, 0);
    }

    const reg32 g = rc::ld32(CurrentGroup);
    if (rc::ld32(g + 0xd0) == 0)
    {
        cpu.edx = edx;
        return;
    }
    reg32 u = 0, v = 0;
    const reg32 kind = rc::ld32(g);
    const reg8 offset = rc::ld8(model + 0x52);
    if (rc::ld32(0x700ea4) != 0 && (kind == 0 || kind == 1) && offset != 0)
    {
        const reg32 t = bits(float(double(sreg32(rc::ld32(0x73ad34))) * ld(0x6cca38)));
        if (offset == 3)
        {
            u = bits(float(double(ld16s(model + 0x5e)) * ld(0x6ccba0)));
            v = bits(float(double(ld16s(model + 0x60)) * ld(0x6ccba0)));
        }
        else
        {
            double a = ld(0x7363e0), b = ld(Zero);
            cpu.fpu.compare(a, b);
            bool still = a == b;
            if (still)
            {
                a = ld(0x7363e4);
                b = ld(Zero);
                cpu.fpu.compare(a, b);
                still = a == b;
            }
            cpu.ebx = model;
            cpu.ebp = entry - 4;
            if (still)
            {
                cpu.ecx = t;
                cpu.esi = model;
                cpu.edi = g;
                cpu.fpu.count = depth;
                rc::st32(s - 4, t);
                rc::st32(s - 8, 0x0040f617);
                cpu.esp = s - 8;
                sub_40ec20(app, cpu);
                if (cpu.terminate)
                    return;
                u = bits(float(popResult(cpu, depth)));
                rc::st32(s - 4, t);
                rc::st32(s - 8, 0x0040f625);
                cpu.esp = s - 8;
                sub_40ec90(app, cpu);
                if (cpu.terminate)
                    return;
                v = bits(float(popResult(cpu, depth)));
            }
            else
            {
                const reg32 scale = rc::ld32(0x701094);
                const sreg32 x = ld16s(model + 0x44);
                rc::st32(s - 4, scale);
                rc::st32(s - 8, bits(float(ld(0x7363e0) / double(x))));
                rc::st32(s - 12, t);
                rc::st32(s - 16, model);
                rc::st32(s - 20, 0x0040f654);
                cpu.eax = rc::ld32(0x701090);
                cpu.esi = scale;
                cpu.edi = t;
                cpu.esp = s - 20;
                cpu.fpu.count = depth;
                sub_546180(app, cpu);
                if (cpu.terminate)
                    return;
                u = bits(float(popResult(cpu, depth)));
                const sreg32 y = ld16s(model + 0x46);
                rc::st32(s - 4, scale);
                rc::st32(s - 8, bits(float(ld(0x7363e4) / double(y))));
                rc::st32(s - 12, t);
                rc::st32(s - 16, model);
                rc::st32(s - 20, 0x0040f67b);
                cpu.eax = rc::ld32(0x701090);
                cpu.ecx = reg32(y);
                cpu.esp = s - 20;
                sub_546180(app, cpu);
                if (cpu.terminate)
                    return;
                v = bits(float(popResult(cpu, depth)));
            }
        }
    }
    cpu.eax = s + 0x30;
    cpu.edi = rc::ld32(CurrentGroup);
    cpu.ebp = entry - 4;
    cpu.fpu.count = depth;
    rc::st32(s - 4, 0x0040f68e);
    cpu.esp = s - 4;
    sub_565020(app, cpu);
    if (cpu.terminate)
        return;
    reg32 matrix[16];
    memcpy(matrix, rc::host(s + 0x30), 64);
    matrix[8] = u;
    matrix[9] = v;
    const reg32 vector[4] = { u, v, 0, 0 };
    const reg32 g2 = rc::ld32(CurrentGroup);
    bridge::effectSetMatrix(app, cpu, rc::ld32(g2 + 0x18), rc::ld32(g2 + 0xd4),
                            reinterpret_cast<const float*>(matrix));
    const reg32 g3 = rc::ld32(CurrentGroup);
    const reg32 effect = rc::ld32(g3 + 0x18);
    bridge::effectSetVector(app, cpu, effect, rc::ld32(g3 + 0xd0), reinterpret_cast<const float*>(vector));
    cpu.edx = rc::ld32(effect);
    saved.restore(cpu);
    cpu.esp = entry + 4;
    cpu.fpu.count = depth;
}

// ---- 0x40f7c0: light context ----

// eax = the light context, [esp + 4] = the world matrix. Fixed-function lights (group kind 5 with lights) or the
// effect's harmonic coefficients. ebx, esi, edi, ebp are saved; edx and the x87 status as the guest leaves them.
void lightsNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 depth = cpu.fpu.count;
    const Saved saved(cpu);
    const reg32 light = cpu.eax;
    const reg32 world = rc::ld32(entry + 4);
    const reg32 frame = (entry - 4) & ~15u;
    const reg32 s = frame - 0x100;
    const reg32 group = rc::ld32(CurrentGroup);
    const reg32 lights = rc::ld32(group) == 5 ? rc::ld32(group + 0xc) : 0;
    if (lights != 0)
    {
        const double k = ld(0x701098);
        for (reg32 i = 0; i < 4; ++i)
            rc::stf32(s + 0xc + 4 * i, float(k * ld(light + 0x120 + 4 * i)));
        cpu.eax = lights;
        cpu.ecx = s + 0xc;
        cpu.esi = light;
        cpu.ebp = entry - 4;
        cpu.fpu.count = depth;
        rc::st32(s - 4, 0x0040f834);
        cpu.esp = s - 4;
        sub_40f6e0(app, cpu);
        if (cpu.terminate)
            return;
        const double scale = ld(ByteScale);
        reg32 color = ftol2(cpu, depth, ld(s + 0x18) * scale) << 8;
        color = (color | ftol2(cpu, depth, ld(s + 0x14) * scale)) << 8;
        color = (color | ftol2(cpu, depth, ld(s + 0x10) * scale)) << 8;
        color |= ftol2(cpu, depth, ld(s + 0xc) * scale);
        const reg32 alpha = rc::ld32(s + 0x18);

        // Directions transposed (w = 0) and the colours.
        reg32 rowWords[3][4];
        for (reg32 r = 0; r < 3; ++r)
        {
            rowWords[r][0] = rc::ld32(light + 0x40 + 4 * r);
            rowWords[r][1] = bits(float(ld(light + 0x50 + 4 * r)));
            rowWords[r][2] = bits(float(ld(light + 0x60 + 4 * r)));
            rowWords[r][3] = 0;
        }
        float rows[3][4];
        memcpy(rows, rowWords, sizeof(rows));
        const double kc = ld(0x6cc8f0);
        float colours[3][3];
        for (reg32 r = 0; r < 3; ++r)
            for (reg32 j = 0; j < 3; ++j)
                colours[r][j] = float(ld(light + 0x10 * r + 4 * j) * kc);
        const double kd = ld(0x70109c), ks = ld(0x7010a0);
        float m[16];
        loadMatrix(world, m);

        // D3DLIGHT9; the fields not set here keep the content of the guest's stack slots.
        DWORD d3dLight[26];
        memcpy(d3dLight, rc::host(s + 0x98), sizeof(d3dLight));
        for (reg32 i = 0; i < 3; ++i)
        {
            d3dLight[0] = 3;
            d3dLight[1] = bits(float(kd * double(colours[i][0])));
            d3dLight[2] = bits(float(kd * double(colours[i][1])));
            d3dLight[3] = bits(float(kd * double(colours[i][2])));
            d3dLight[5] = bits(float(ks * double(colours[i][0])));
            d3dLight[6] = bits(float(ks * double(colours[i][1])));
            d3dLight[7] = bits(float(ks * double(colours[i][2])));
            d3dLight[9] = 0;
            d3dLight[10] = 0;
            d3dLight[11] = 0;
            d3dLight[12] = alpha;
            float direction[4];
            bridge::vec4Transform(rows[i], m, direction);
            d3dLight[16] = bits(float(-double(direction[0])));
            d3dLight[17] = bits(float(-double(direction[1])));
            d3dLight[18] = bits(float(-double(direction[2])));
            bridge::setLight(app, cpu, rc::ld32(DevicePointer), i, d3dLight);
            bridge::lightEnable(app, cpu, rc::ld32(DevicePointer), i, 1);
            if (i < 2)
                app->allowPreemption(cpu);
        }
        const reg32 g = rc::ld32(CurrentGroup);
        const reg32 effect = rc::ld32(g + 0x18);
        cpu.eax = bridge::effectSetInt(app, cpu, effect, rc::ld32(g + 0x4c), sreg32(color));
        cpu.edx = rc::ld32(effect);
    }
    else
    {
        bridge::effectSetVectorArray(app, cpu, rc::ld32(group + 0x18), rc::ld32(group + 0x48),
                                     guestFloats(light + 0x120), 10);
        const reg32 g = rc::ld32(CurrentGroup);
        const reg32 effect = rc::ld32(g + 0x18);
        cpu.eax = bridge::effectSetVector(app, cpu, effect, rc::ld32(g + 0x3c), guestFloats(light + 0xc0));
        cpu.edx = rc::ld32(effect);
    }
    saved.restore(cpu);
    cpu.esp = entry + 4;
    cpu.fpu.count = depth;
}

// ---- 0x4100a0: reflection matrices ----

// eax = the reflection view (its view record pointer at +0), ecx = the world matrix. ebx, esi, edi, ebp are saved;
// edx as the guest leaves it.
void reflectionNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 depth = cpu.fpu.count;
    const Saved saved(cpu);
    const reg32 world = cpu.ecx;
    const reg32 viewProjection = rc::ld32(cpu.eax) + 0x80;
    const reg32 frame = (entry - 4) & ~15u;
    const reg32 s = frame - 0x120;
    const reg32 one = 0x3f800000u, half = 0x3f000000u;
    const reg32 biasWords[16] = { one, 0, 0, 0, 0, one, 0, 0, 0, 0, one, 0, half, half, 0, one };
    float bias[16];
    memcpy(bias, biasWords, sizeof(bias));
    float a[16], b[16], m[16];
    loadMatrix(world, a);
    loadMatrix(viewProjection, b);
    const reg32 group = rc::ld32(CurrentGroup);
    if (rc::ld32(group + 0xc) == 0)
    {
        if (settings().defer)
        {
            bridge::effectSetMatrixProduct(app, cpu, rc::ld32(group + 0x18), rc::ld32(group + 0x38), a, b, bias);
        }
        else
        {
            bridge::matrixMultiply(a, b, m);
            bridge::matrixMultiply(m, bias, m);
            bridge::effectSetMatrix(app, cpu, rc::ld32(group + 0x18), rc::ld32(group + 0x38), m);
        }
    }
    else
    {
        const reg32 view = rc::ld32(rc::ld32(CurrentView) + 4);
        cpu.eax = rc::ld32(ViewRecords + view * 0x60);
        cpu.esi = s + 0xe0;
        cpu.edi = viewProjection;
        cpu.ebp = entry - 4;
        cpu.fpu.count = depth;
        rc::st32(s - 4, 0x004101a3);
        cpu.esp = s - 4;
        sub_40bc10(app, cpu);
        if (cpu.terminate)
            return;
        float r[16], p[16];
        loadMatrix(s + 0xe0, r);
        loadMatrix(viewProjection, b);
        bridge::matrixMultiply(b, bias, p);
        bridge::matrixMultiply(r, p, p);
        const reg32 g = rc::ld32(CurrentGroup);
        bridge::effectSetMatrix(app, cpu, rc::ld32(g + 0x18), rc::ld32(g + 0x38), p);
        const double k = ld(0x6ccc34);
        float t[16];
        memset(t, 0, sizeof(t));
        t[0] = float(double(p[3]) * k);
        t[4] = float(double(p[7]) * k);
        t[5] = asFloat(one);
        t[8] = float(double(p[11]) * k);
        t[10] = asFloat(one);
        t[12] = float(double(p[15]) * k + ld(Half));
        t[15] = asFloat(one);
        const reg32 g2 = rc::ld32(CurrentGroup);
        bridge::effectSetMatrix(app, cpu, rc::ld32(g2 + 0x18), rc::ld32(g2 + 0xb8), t);
    }
    const reg32 vector[4] = { rc::ld32(0x7010ac), rc::ld32(0x7010b0), rc::ld32(0x7010b4), 0 };
    const reg32 g = rc::ld32(CurrentGroup);
    const reg32 effect = rc::ld32(g + 0x18);
    cpu.eax = bridge::effectSetVector(app, cpu, effect, rc::ld32(g + 0x70), reinterpret_cast<const float*>(vector));
    cpu.edx = rc::ld32(effect);
    saved.restore(cpu);
    cpu.esp = entry + 4;
    cpu.fpu.count = depth;
}

// ---- 0x40fae0: material ----

// ecx = the material, eax = the packet flags. The fixed-function material and texture factor (group kind 5 with
// lights) or the effect's diffuse, specular and envmap terms. ebx, esi, edi, ebp are saved; edx and the x87 status
// as the guest leaves them.
void materialNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 depth = cpu.fpu.count;
    const Saved saved(cpu);
    const reg32 material = cpu.ecx;
    const reg32 flags = cpu.eax;
    const auto m = [&](reg32 offset) { return ld(material + offset); };
    // The six vectors at the guest's stack +0x10, +0x40, +0x20, +0x60, +0x30, +0x50.
    float d0[4], d1[4], d2[4], d3[4], d4[4], d5[4];
    for (reg32 j = 0; j < 3; ++j)
        d0[j] = float(m(0x34 + 4 * j) * m(0x30));
    d0[3] = float(m(0x50));
    for (reg32 j = 0; j < 3; ++j)
        d1[j] = float(m(0x44 + 4 * j) * m(0x40) - double(d0[j]));
    d1[3] = float(m(0x54) - double(d0[3]));
    for (reg32 j = 0; j < 3; ++j)
        d2[j] = float(m(0x60 + 4 * j) * m(0x5c));
    d2[3] = 0;
    for (reg32 j = 0; j < 3; ++j)
        d3[j] = float(m(0x70 + 4 * j) * m(0x6c) - double(d2[j]));
    d3[3] = 0;
    const double half = ld(Half);
    for (reg32 j = 0; j < 3; ++j)
        d4[j] = float(m(0x84 + 4 * j) * m(0x80) * half);
    d4[3] = 0;
    for (reg32 j = 0; j < 3; ++j)
        d5[j] = float(m(0x94 + 4 * j) * m(0x90) * half - double(d4[j]));
    d5[3] = 0;
    if (!(flags & 0x100) || rc::ld32(0x700e74) == 0)
    {
        memset(d4, 0, sizeof(d4));
        memset(d5, 0, sizeof(d5));
    }
    else if (!(flags & 0x80))
    {
        memset(d0, 0, sizeof(d0));
        memset(d1, 0, sizeof(d1));
        memset(d2, 0, sizeof(d2));
        memset(d3, 0, sizeof(d3));
    }

    const reg32 group = rc::ld32(CurrentGroup);
    if (rc::ld32(group) == 5 && rc::ld32(group + 0xc) != 0)
    {
        const reg32 one = 0x3f800000u;
        DWORD d3dMaterial[17];
        d3dMaterial[0] = bits(float(double(d1[0]) * half + double(d0[0])));
        d3dMaterial[1] = bits(float(double(d1[1]) * half + double(d0[1])));
        d3dMaterial[2] = bits(float(double(d1[2]) * half + double(d0[2])));
        d3dMaterial[3] = one;
        d3dMaterial[4] = one;
        d3dMaterial[5] = one;
        d3dMaterial[6] = one;
        d3dMaterial[7] = one;
        d3dMaterial[8] = bits(float(double(d3[0]) * half + double(d2[0])));
        d3dMaterial[9] = bits(float(double(d3[1]) * half + double(d2[1])));
        d3dMaterial[10] = bits(float(double(d3[2]) * half + double(d2[2])));
        d3dMaterial[11] = one;
        d3dMaterial[12] = 0;
        d3dMaterial[13] = 0;
        d3dMaterial[14] = 0;
        d3dMaterial[15] = one;
        const float e0 = float(double(d5[0]) * half + double(d4[0]));
        const float e1 = float(double(d5[1]) * half + double(d4[1]));
        const float e2 = float(double(d5[2]) * half + double(d4[2]));
        const double power = ld(0x7010a8) * m(0x58);
        const double minimum = ld(0x6cc7b8);
        d3dMaterial[16] = bits(float(power));
        cpu.fpu.compare(power, minimum);
        if (power < minimum)
            d3dMaterial[16] = 0x3dcccccdu;
        bridge::setMaterial(app, cpu, rc::ld32(DevicePointer), d3dMaterial);

        const double scale = ld(ByteScale);
        const reg32 device = rc::ld32(DevicePointer);
        reg32 factor = ftol2(cpu, depth, double(d1[3]) * ld(0x6cc7c4) + double(d0[3]) * scale) << 8;
        factor = (factor | ftol2(cpu, depth, double(e2) * scale)) << 8;
        factor = (factor | ftol2(cpu, depth, double(e1) * scale)) << 8;
        factor |= ftol2(cpu, depth, double(e0) * scale);
        cpu.eax = bridge::setRenderState(app, cpu, device, RsTextureFactor, factor);
    }
    else
    {
        const auto setVector = [&](reg32 handle, const float* vector)
        {
            const reg32 g = rc::ld32(CurrentGroup);
            bridge::effectSetVector(app, cpu, rc::ld32(g + 0x18), rc::ld32(g + handle), vector);
        };
        setVector(0x50, d0);
        setVector(0x54, d1);
        setVector(0x58, d2);
        setVector(0x5c, d3);
        setVector(0x64, d4);
        setVector(0x68, d5);
        const reg32 g = rc::ld32(CurrentGroup);
        bridge::effectSetFloat(app, cpu, rc::ld32(g + 0x18), rc::ld32(g + 0x60), float(ld(0x7070d4) * m(0x58)));
        const reg32 g2 = rc::ld32(CurrentGroup);
        const reg32 effect = rc::ld32(g2 + 0x18);
        cpu.eax = bridge::effectSetFloat(app, cpu, effect, rc::ld32(g2 + 0x6c), float(m(0x7c)));
        cpu.edx = rc::ld32(effect);
    }
    saved.restore(cpu);
    cpu.esp = entry + 4;
    cpu.fpu.count = depth;
}

// ---- dispatch, shadow compare and perf ----

typedef void (*Function)(WinApplication*, x86::CPU&);

// The registers the natives set for their callers; the shadow compares those the guest stores at the return
// (retstate.h).
struct LeafInfo
{
    const char* name;
    Function guest;
    Function native;
    uint8_t registers;
    reg32 address;
};

const LeafInfo s_leaves[LeafCount] = {
    { "world 0x410400", &sub_410400_guest, &worldNative, 0xff, 0x410400 },
    { "blend 0x410550", &sub_410550_guest, &blendNative, 0xff, 0x410550 },
    { "alpha test 0x410650", &sub_410650_guest, &alphaTestNative, 0xff, 0x410650 },
    { "packets 0x410700", &sub_410700_guest, &packetsNative, 0x7c, 0x410700 },
    { "node 0x40ed10", &sub_40ed10_guest, &nodeNative, 0xfc, 0x40ed10 },
    { "group 0x40e6d0", &sub_40e6d0_guest, &groupNative, 0xfe, 0x40e6d0 },
    { "bones 0x4102f0", &sub_4102f0_guest, &bonesNative, 0xf0, 0x4102f0 },
    { "model 0x40f3d0", &sub_40f3d0_guest, &modelNative, 0xfc, 0x40f3d0 },
    { "lights 0x40f7c0", &sub_40f7c0_guest, &lightsNative, 0xfc, 0x40f7c0 },
    { "reflection 0x4100a0", &sub_4100a0_guest, &reflectionNative, 0xfc, 0x4100a0 },
    { "material 0x40fae0", &sub_40fae0_guest, &materialNative, 0xfc, 0x40fae0 },
};

// Guest memory each function may store to, besides its stack frame.
void regions(Leaf leaf, const x86::CPU& cpu, std::vector<Span>& spans)
{
    const auto list = [&](reg32 head)
    {
        const reg32 pool = rc::ld32(PacketPool);
        spans.push_back(Span{pool + 0x10, 4});
        spans.push_back(Span{pool + 0x18, 4});
        spans.push_back(Span{head, 8});
        reg32 node = rc::ld32(head);
        for (uint32_t n = 0; node != head && n < 100000; ++n, node = rc::ld32(node))
            spans.push_back(Span{node, 8});
    };
    switch (leaf)
    {
    case LeafWorld:
        spans.push_back(Span{WorldCalls, 16});
        spans.push_back(Span{LastView, 4});
        spans.push_back(Span{LastGroup, 8});
        break;
    case LeafBlend:
        spans.push_back(Span{BlendDestination, 12});
        break;
    case LeafAlphaTest:
        spans.push_back(Span{AlphaRef, 8});
        break;
    case LeafPackets:
        list(cpu.edi);
        break;
    case LeafNode:
        list(cpu.eax + 0x18);
        break;
    case LeafGroup:
        spans.push_back(Span{CurrentGroup, 4});
        break;
    case LeafModel:
        spans.push_back(Span{LastModel, 4});
        spans.push_back(Span{AlphaRef, 20});
        break;
    default:
        break;
    }
}

struct Shadow
{
    bridge::Capture capture;
    std::vector<Span> spans;
    std::vector<reg8> before, guestAfter, nativeAfter;
    uint32_t calls = 0, differing = 0, logged = 0;
};

Shadow s_shadow;

void shadow(Leaf leaf, WinApplication* app, x86::CPU& cpu)
{
    Shadow& s = s_shadow;
    const LeafInfo& info = s_leaves[leaf];
    const x86::CPU entry = cpu;
    s.spans.clear();
    regions(leaf, entry, s.spans);
    snapshot(s.spans, s.before);

    // The group switch's guest calls go through the callouts while the guest code runs.
    reg32 slots[bridge::CalloutCount] = {}, targets[bridge::CalloutCount] = {};
    if (leaf == LeafGroup)
    {
        const reg32 current = rc::ld32(CurrentGroup);
        if (current != entry.eax)
        {
            if (current)
                slots[0] = current + 0xdc;
            slots[1] = entry.eax + 0xd8;
        }
        for (uint32_t i = 0; i < bridge::CalloutCount; ++i)
        {
            if (!slots[i])
                continue;
            targets[i] = rc::ld32(slots[i]);
            bridge::setCalloutTarget(i, targets[i]);
            rc::st32(slots[i], bridge::calloutAddress(i));
        }
    }
    s.capture.clear();
    bridge::begin(s.capture, bridge::Capture::Record);
    info.guest(app, cpu);
    bridge::end();
    for (uint32_t i = 0; i < bridge::CalloutCount; ++i)
    {
        if (slots[i])
            rc::st32(slots[i], targets[i]);
    }
    if (cpu.terminate)
        return;
    const x86::CPU guest = cpu;
    snapshot(s.spans, s.guestAfter);

    restoreSpans(s.spans, s.before);
    cpu = entry;
    g_stores.clear();
    g_logStores = true;
    bridge::begin(s.capture, bridge::Capture::Replay);
    info.native(app, cpu);
    bridge::end();
    g_logStores = false;
    const x86::CPU native = cpu;
    snapshot(s.spans, s.nativeAfter);
    restoreSpans(s.spans, s.guestAfter);
    cpu = guest;

    ++s.calls;
    char text[200] = {};
    const bool same = sameCalls(s.capture, text, sizeof(text))
                      && sameState(info.address, info.registers, guest, native, text, sizeof(text))
                      && sameMemory(s.spans, s.guestAfter, s.nativeAfter, text, sizeof(text));
    if (!same)
    {
        ++s.differing;
        if (s.logged < ShadowLogLimit)
        {
            ++s.logged;
            pf::logInfo("shadow_draw: %s call %u (caller 0x%08x): %s", info.name, unsigned(s.calls),
                        unsigned(rc::ld32(entry.esp)), text);
        }
    }
}

uint32_t s_calls[LeafCount] = {};
uint32_t s_counted = 0;
uint64_t s_sampledUs = 0;
uint32_t s_depth = 0;

void dispatch(Leaf leaf, WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    const LeafInfo& info = s_leaves[leaf];
    if (s_depth != 0 || bridge::capturing())
    {
        info.guest(app, cpu);
        return;
    }
    ++s_calls[leaf];
    const bool native = s.native;
    if (native && __builtin_expect(s.shadow, 0))
    {
        ++s_depth;
        shadow(leaf, app, cpu);
        --s_depth;
        return;
    }
    const Function f = native ? info.native : info.guest;
    if (__builtin_expect(pf::perf::enabled(), 0) && (++s_counted & SampleMask) == 0)
    {
        ++s_depth;
        const uint32_t start = pf::perf::now();
        f(app, cpu);
        s_sampledUs += pf::perf::now() - start;
        --s_depth;
        return;
    }
    if (native)
    {
        f(app, cpu);
        return;
    }
    ++s_depth;
    f(app, cpu);
    --s_depth;
}

}

bool g_logStores = false;
std::vector<reg32> g_stores;

void runLeaf(Leaf leaf, WinApplication* app, x86::CPU& cpu)
{
    const LeafInfo& info = s_leaves[leaf];
    ++s_calls[leaf];
    if (settings().native)
    {
        info.native(app, cpu);
        return;
    }
    ++s_depth;
    info.guest(app, cpu);
    --s_depth;
}

bool nativeLeaves()
{
    return settings().native;
}

bool shadowCompare()
{
    return settings().shadow;
}

void snapshot(const std::vector<Span>& spans, std::vector<reg8>& bytes)
{
    bytes.clear();
    for (const Span& s : spans)
        bytes.insert(bytes.end(), rc::host(s.address), rc::host(s.address) + s.size);
}

void restoreSpans(const std::vector<Span>& spans, const std::vector<reg8>& bytes)
{
    size_t at = 0;
    for (const Span& s : spans)
    {
        memcpy(rc::host(s.address), bytes.data() + at, s.size);
        at += s.size;
    }
}

bool sameState(reg32 function, uint8_t registers, const x86::CPU& g, const x86::CPU& n, char* text, size_t size)
{
    if (!retstate::sameRegisters(registers & retstate::registers(function), g, n, text, size))
        return false;
    if (g.fpu.count != n.fpu.count)
    {
        snprintf(text, size, "x87 depth guest %u native %u", unsigned(g.fpu.count), unsigned(n.fpu.count));
        return false;
    }
    return retstate::sameFlags(retstate::flags(function), g, n, text, size);
}

bool sameCalls(const bridge::Capture& c, char* text, size_t size)
{
    if (c.differs)
    {
        snprintf(text, size, "device call %u: guest %s, native %s, word %u guest 0x%08x native 0x%08x",
                 unsigned(c.differingCall), bridge::methodName(c.expected), bridge::methodName(c.got),
                 unsigned(c.differingWord), unsigned(c.expectedWord), unsigned(c.gotWord));
        return false;
    }
    if (c.replayed != c.calls.size())
    {
        snprintf(text, size, "device calls guest %u native %u", unsigned(c.calls.size()), unsigned(c.replayed));
        return false;
    }
    return true;
}

bool sameMemory(const std::vector<Span>& spans, const std::vector<reg8>& guest, const std::vector<reg8>& native,
                char* text, size_t size)
{
    size_t at = 0;
    for (const Span& span : spans)
    {
        for (reg32 i = 0; i < span.size; ++i, ++at)
        {
            if (guest[at] != native[at])
            {
                snprintf(text, size, "byte at 0x%08x guest 0x%02x native 0x%02x", unsigned(span.address + i),
                         unsigned(guest[at]), unsigned(native[at]));
                return false;
            }
        }
    }
    for (const reg32 address : g_stores)
    {
        bool inside = false;
        for (const Span& span : spans)
            inside |= address - span.address < span.size && address + 4 - span.address <= span.size;
        if (!inside)
        {
            snprintf(text, size, "native stored at 0x%08x outside the guest's stores", unsigned(address));
            return false;
        }
    }
    return true;
}

void logUsage(unsigned frames)
{
    if (!frames)
        return;
    const Settings& s = settings();
    uint32_t total = 0;
    for (uint32_t n : s_calls)
        total += n;
    const QueueUsage q = takeQueueUsage();
    pf::logInfo("draw: %s setup functions %u calls/frame, %.2f ms/frame (1 in %u timed); world %u, blend %u, "
                "alpha test %u, packets %u, node %u, group %u, bones %u, model %u, lights %u, reflection %u, "
                "material %u; %s queue: flush %u calls/frame, %.2f ms/frame, %u packets, %u draws, %u nodes; "
                "enqueue %u, submit %u", !s.native ? "guest" : s.shadow ? "shadow" : "native", total / frames,
                double(s_sampledUs) * double(SampleMask + 1) / 1000.0 / frames, unsigned(SampleMask + 1),
                s_calls[LeafWorld] / frames, s_calls[LeafBlend] / frames, s_calls[LeafAlphaTest] / frames,
                s_calls[LeafPackets] / frames, s_calls[LeafNode] / frames, s_calls[LeafGroup] / frames,
                s_calls[LeafBones] / frames, s_calls[LeafModel] / frames, s_calls[LeafLights] / frames,
                s_calls[LeafReflection] / frames, s_calls[LeafMaterial] / frames,
                !nativeQueue() ? "guest" : s.shadow ? "shadow" : "native", q.flushes / frames,
                double(q.flushUs) / 1000.0 / frames, q.packets / frames, q.draws / frames, q.nodes / frames,
                q.enqueues / frames, q.submits / frames);
    if (s.native && s.shadow)
        pf::logInfo("shadow_draw: %u calls compared, %u differ", unsigned(s_shadow.calls),
                    unsigned(s_shadow.differing));
    if (nativeQueue() && s.shadow)
        pf::logInfo("shadow_draw: queue %u calls compared, %u differ, %u not compared", unsigned(q.compared),
                    unsigned(q.differing), unsigned(q.skipped));
    memset(s_calls, 0, sizeof(s_calls));
    s_sampledUs = 0;
}

}

namespace native
{

void sub_410400(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafWorld, app, cpu);
}

void sub_410550(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafBlend, app, cpu);
}

void sub_410650(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafAlphaTest, app, cpu);
}

void sub_410700(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafPackets, app, cpu);
}

void sub_40ed10(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafNode, app, cpu);
}

void sub_40e6d0(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafGroup, app, cpu);
}

void sub_4102f0(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafBones, app, cpu);
}

void sub_40f3d0(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafModel, app, cpu);
}

void sub_40f7c0(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafLights, app, cpu);
}

void sub_4100a0(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafReflection, app, cpu);
}

void sub_40fae0(win32::WinApplication* app, x86::CPU& cpu)
{
    draw::dispatch(draw::LeafMaterial, app, cpu);
}

}
}
