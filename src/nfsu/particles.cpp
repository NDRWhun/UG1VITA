// Native particle system draw (particles.h), guest 0x404110. The x87 arithmetic is kept in double in the guest's
// order and rounded to float where the guest stores a float; the matrix copies of 0x401cd0 pass elements 1, 2 of
// each row through the x87 stack and copy the others as integers.

#include "particles.h"
#include "phases.h"

#include <platform/config.h>
#include <platform/jobs.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <initializer_list>
#include <recomp.h>
#include <vector>
#include <winapi/d3d9_internal.h>
#include <winapi/d3d9_native.h>

namespace nfsu
{

// Generated functions: the guest body (overridden), the group switch, the transforms and the texture states.
void sub_404110_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40e6d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_410400(win32::WinApplication* app, x86::CPU& cpu);
void sub_40f3d0(win32::WinApplication* app, x86::CPU& cpu);

namespace particles
{

namespace
{

using x86::reg8;
using x86::reg32;
using x86::sreg16;
using x86::sreg32;

// Guest data.
const reg32 DeviceSlot = 0x73636c;      // IDirect3DDevice9*
const reg32 VertexBuffers = 0x718738;   // 30 vertex buffers
const reg32 BufferIndex = 0x7187b4;     // the buffer in use
const reg32 IndexBuffer = 0x7187b0;     // quad indices
const reg32 SkipName = 0x6cc594;        // systems whose name hash at +8 matches are not drawn here
const reg32 One = 0x6cc7bc, AgeScale = 0x6cc8e4, Half = 0x6cc7dc, Zero = 0x6cc7a4, Top = 0x6cc974;
const reg32 Group0 = 0x71abb8;          // render group of the particles
const reg32 Identity = 0x78bcc0;        // world matrix argument of 0x410400
const reg32 Modes = 0x4049d4;           // byte per draw mode 1..26: 0, 1, 2 the cases below, 3 drawn

const uint32_t BatchParticles = 300;
const uint32_t BufferCount = 30;
const uint32_t QuadWords = 24;          // 4 vertices: x, y, z, colour, u, v
const reg32 VertexStride = 0x18;
const int BatchItems = 32;

// IDirect3DDevice9 / IDirect3DVertexBuffer9 vtable offsets.
const reg32 SetRenderState = 0xe4, SetStreamSource = 0x190, SetIndices = 0x1a0, DrawIndexedPrimitive = 0x148;
const reg32 Lock = 0x2c, Unlock = 0x30;
const reg32 ZWriteEnable = 14, AlphaBlendEnable = 27, LockDiscard = 0x2000;

struct Settings
{
    bool native;
    bool parallel;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("particles_native", false),
                             pf::Config::getBool("particles_parallel", true),
                             pf::Config::getBool("shadow_particles", false) };
        pf::logInfo("particles: particle draw %s%s%s", r.shadow ? "guest with shadow compare" : r.native ? "native"
                                                                                                         : "guest",
                    !r.shadow && r.native && r.parallel ? ", quads on the job workers" : "",
                    r.shadow ? " (shadow_particles)" : "");
        return r;
    }();
    return s;
}

inline float pass(float value)
{
    return float(double(value));
}

inline reg32 bits(float value)
{
    reg32 b;
    memcpy(&b, &value, sizeof(b));
    return b;
}

inline float floatOf(reg32 b)
{
    float value;
    memcpy(&value, &b, sizeof(value));
    return value;
}

// _ftol2: truncation to 64 bits, the low half.
inline reg32 truncate(double value)
{
    return reg32(x86::fistInteger<x86::sreg64>(std::trunc(value)));
}

// The 0x401cd0 copy of a matrix.
void copyMatrix(reg32 source, float* out)
{
    for (int row = 0; row < 4; ++row)
    {
        const reg32 r = source + 16 * row;
        out[4 * row] = floatOf(rc::ld32(r));
        out[4 * row + 1] = pass(rc::ldf32(r + 4));
        out[4 * row + 2] = pass(rc::ldf32(r + 8));
        out[4 * row + 3] = floatOf(rc::ld32(r + 12));
    }
}

// Inputs shared by every system of a call.
struct View
{
    float right[3], up[3];      // columns 0 and 1 of the view matrix copy
    double one, ageScale, half, zero, top;
};
View s_view;

// A system that is drawn and the inputs of its quads.
struct System
{
    reg32 node, system, texture;
    float colour[16], size[16];     // curves at +0x100 and +0x140
    float invLife;                  // 1 / [system+0x38]
    float cellU, cellV;             // texture cell size
    reg32 maskU, maskV, shiftU;
    uint32_t first, count;          // particles in s_particles
};

struct Quad
{
    reg32 keep;
    reg32 words[QuadWords];
};

std::vector<System> s_systems;
std::vector<reg32> s_particles;
std::vector<uint32_t> s_owner;      // system of each particle
std::vector<Quad> s_quads;

// One particle's colour and quad (guest 0x404410-0x40482b).
void computeQuad(const System& s, reg32 p, Quad& q)
{
    const double age = double(sreg32(sreg16(rc::ld16(p + 8))));
    const float t = float(s_view.one - age * s_view.ageScale * double(s.invLife));
    const float px = pass(rc::ldf32(p + 0x10)), py = pass(rc::ldf32(p + 0x14)), pz = pass(rc::ldf32(p + 0x18));
    const double t2 = double(t) * double(t);
    const float curve[4] = { float(double(t) * t2), float(t2), t, 1.0f };

    float size[4];
    win32::d3d9::bridge::vec4Transform(curve, s.size, size);
    const double h = double(size[0]) * s_view.half;
    float rh[3], uh[3];
    for (int i = 0; i < 3; ++i)
    {
        rh[i] = float(double(s_view.right[i]) * h);
        uh[i] = float(double(s_view.up[i]) * h);
    }

    float colour[4];
    win32::d3d9::bridge::vec4Transform(curve, s.colour, colour);
    for (float& c : colour)
        if (double(c) < s_view.zero)
            c = floatOf(0);
    for (float& c : colour)
        if (double(c) > s_view.top)
            c = floatOf(0x437f0000);
    const reg32 alpha = truncate(double(colour[3]) * s_view.half);
    if (alpha == 0)
    {
        q.keep = 0;
        return;
    }
    reg32 argb = alpha;
    argb = (argb << 8) | truncate(double(colour[0]));
    argb = (argb << 8) | truncate(double(colour[1]));
    argb = (argb << 8) | truncate(double(colour[2]));

    const reg32 cell = rc::ld8(p + 0x1f);
    const sreg32 cu = sreg32(cell & s.maskU);
    const sreg32 cv = sreg32(cell >> (s.shiftU & 31)) & sreg32(s.maskV);
    const double u0 = double(cu) * double(s.cellU);
    const double v0 = double(cv) * double(s.cellV);
    const double u1 = u0 + double(s.cellU);
    const float v1 = float(v0 + double(s.cellV));

    const double p3[3] = { double(px), double(py), double(pz) };
    reg32* w = q.words;
    for (int i = 0; i < 3; ++i)
    {
        const double r = double(rh[i]), u = double(uh[i]);
        w[i] = bits(float((u + r) + p3[i]));
        w[6 + i] = bits(float((p3[i] - r) + u));
        w[12 + i] = bits(float((p3[i] - r) - u));
        w[18 + i] = bits(float((r + p3[i]) - u));
    }
    w[3] = w[9] = w[15] = w[21] = argb;
    w[4] = bits(float(u0));
    w[5] = bits(float(v0));
    w[10] = bits(float(u1));
    w[11] = bits(float(v0));
    w[16] = bits(float(u1));
    w[17] = bits(v1);
    w[22] = bits(float(u0));
    w[23] = bits(v1);
    q.keep = 1;
}

void quadBatch(void*, int begin, int end, int)
{
    for (int i = begin; i < end; ++i)
        computeQuad(s_systems[s_owner[i]], s_particles[i], s_quads[i]);
}

// The hash of the name at SkipName (guest 0x4041f0-0x404218); the guest takes its first character as a constant.
reg32 skipHash()
{
    reg32 h = 0xffffffffu;
    reg32 c = 0x58;
    for (reg32 a = SkipName; ; )
    {
        h = h * 0x21 + c;
        c = rc::ld8(++a);
        if (c == 0)
            break;
    }
    return h;
}

// Whether a system node is drawn for the view (guest 0x404240-0x40430e), with its inputs.
bool drawnSystem(reg32 node, reg32 view, reg32 hash, reg32 viewCar, reg32 filter, System& s)
{
    const reg32 system = rc::ld32(node + 8);
    if (rc::ld32(system + 8) == hash)
        return false;
    const reg32 car = rc::ld32(node + 0x10);
    if (car != 0)
    {
        const reg32 viewId = rc::ld32(view + 4);
        if (((1u << (viewId & 31)) & rc::ld32(car + 0x534)) == 0)
            return false;
        const reg32 mode = rc::ld32(system + 0x58) - 1;
        const reg32 kind = mode > 0x19 ? 3 : rc::ld8(Modes + mode);
        if (kind == 2)          // 0x40429d
        {
            if (viewId == 3 || viewCar == car)
                return false;
        }
        else if (kind == 1)     // 0x4042b1
        {
            if (viewId != 3 && viewCar == car)
                return false;
        }
        else if (kind == 0)     // 0x4042c2
        {
            if (viewCar == car && rc::ld32(rc::ld32(view + 0x40) + 0x284) != 0)
                return false;
        }
    }
    const float invLife = float(s_view.one / double(rc::ldf32(system + 0x38)));
    const reg32 texture = rc::ld32(system + 0x14);
    if (texture == 0)
        return false;
    if (filter != 0 && rc::ld32(node + 0xc) != filter)
        return false;
    s.node = node;
    s.system = system;
    s.texture = texture;
    s.invLife = invLife;
    copyMatrix(system + 0x100, s.colour);
    copyMatrix(system + 0x140, s.size);
    const reg32 shiftU = reg32(sreg32(int8_t(rc::ld8(system + 0x19))));
    const reg32 shiftV = reg32(sreg32(int8_t(rc::ld8(system + 0x1a))));
    s.maskU = ~(0xffffffffu << (shiftU & 31));
    s.maskV = ~(0xffffffffu << (shiftV & 31));
    s.shiftU = shiftU;
    s.cellU = float(s_view.one / double(sreg32(1u << (shiftU & 31))));
    s.cellV = float(s_view.one / double(sreg32(1u << (shiftV & 31))));
    return true;
}

// The drawn systems of a list and their particles, in the guest's order, and every quad.
void prepare(reg32 list, reg32 view)
{
    s_systems.clear();
    s_particles.clear();
    s_owner.clear();
    float m[16];
    copyMatrix(rc::ld32(view), m);
    s_view.right[0] = m[0], s_view.right[1] = m[4], s_view.right[2] = m[8];
    s_view.up[0] = m[1], s_view.up[1] = m[5], s_view.up[2] = m[9];
    s_view.one = double(rc::ldf32(One));
    s_view.ageScale = double(rc::ldf32(AgeScale));
    s_view.half = double(rc::ldf32(Half));
    s_view.zero = double(rc::ldf32(Zero));
    s_view.top = double(rc::ldf32(Top));
    const reg32 viewCar = rc::ld32(rc::ld32(view + 0x40) + 0x280);
    const reg32 filter = rc::ld8(list + 0x4d) ? rc::ld32(list + 0x40) : 0;
    const reg32 hash = skipHash();
    for (reg32 node = rc::ld32(list); node != list; node = rc::ld32(node))
    {
        System s;
        if (!drawnSystem(node, view, hash, viewCar, filter, s))
            continue;
        s.first = uint32_t(s_particles.size());
        const reg32 head = node + 0x14;
        for (reg32 p = rc::ld32(head); p != head; p = rc::ld32(p))
        {
            s_particles.push_back(p);
            s_owner.push_back(uint32_t(s_systems.size()));
        }
        s.count = uint32_t(s_particles.size()) - s.first;
        s_systems.push_back(s);
    }
    s_quads.resize(s_particles.size());
    const int count = int(s_particles.size());
    if (settings().parallel)
        pf::jobs::run(count, BatchItems, &quadBatch, nullptr);
    else
        quadBatch(nullptr, 0, count, 0);
}

// A stdcall COM method of object at the vtable offset, with the arguments after this, called as the guest calls
// it from returnAddress. False when the game is ending.
bool com(win32::WinApplication* app, x86::CPU& c, reg32 object, reg32 offset, std::initializer_list<reg32> args,
         reg32 returnAddress)
{
    const reg32 base = c.esp - 4 * reg32(args.size() + 1);
    rc::st32(base, object);
    reg32 slot = base + 4;
    for (reg32 a : args)
    {
        rc::st32(slot, a);
        slot += 4;
    }
    c.esp = base - 4;
    rc::st32(c.esp, returnAddress);
    c.ip = rc::ld32(rc::ld32(object) + offset);
    app->dynamic_call(c.ip, c);
    return !c.terminate;
}

// A guest function called with esp at the return address slot; false when the game is ending.
bool call(win32::WinApplication* app, x86::CPU& c, void (*function)(win32::WinApplication*, x86::CPU&),
          reg32 returnAddress)
{
    c.esp -= 4;
    rc::st32(c.esp, returnAddress);
    function(app, c);
    return !c.terminate;
}

reg32 currentBuffer()
{
    return rc::ld32(VertexBuffers + 4 * rc::ld32(BufferIndex));
}

void advanceBuffer()
{
    rc::st32(BufferIndex, reg32((sreg32(rc::ld32(BufferIndex)) + 1) % sreg32(BufferCount)));
}

// Return addresses of the four calls of a batch draw: a full batch, the last batch of a system.
const reg32 FullBatchCalls[4] = { 0x404872, 0x404881, 0x40488e, 0x4048a9 };
const reg32 LastBatchCalls[4] = { 0x404926, 0x404935, 0x404942, 0x40495f };

// The draw of the buffer in use with count quads (guest 0x404856-0x4048c2 and 0x404906-0x404973).
bool drawBatch(win32::WinApplication* app, x86::CPU& c, reg32 device, uint32_t count, const reg32* at)
{
    const reg32 buffer = currentBuffer();
    return com(app, c, device, SetStreamSource, { 0, buffer, 0, VertexStride }, at[0])
        && com(app, c, device, SetIndices, { rc::ld32(IndexBuffer) }, at[1])
        && com(app, c, device, SetRenderState, { AlphaBlendEnable, 1 }, at[2])
        && com(app, c, device, DrawIndexedPrimitive, { 4, 0, 0, count * 4, 0, count * 2 }, at[3]);
}

struct PerfSums
{
    uint64_t us = 0;
    uint32_t calls = 0, systems = 0, particles = 0, quads = 0;
};
PerfSums s_perf;

// The native body; cpu as at the guest's entry.
void drawNative(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 list = rc::ld32(cpu.esp + 4), view = rc::ld32(cpu.esp + 8);
    const reg32 ebp = cpu.esp - 4;
    const reg32 esp0 = ((ebp & ~15u) - 0x1d4) - 12;
    rc::st32(ebp, entry.ebp);
    rc::st32(esp0 + 8, entry.ebx);
    rc::st32(esp0 + 4, entry.esi);
    rc::st32(esp0, entry.edi);
    if (rc::ld32(view + 4) == 1)
    {
        rc::st32(list + 0x58, 0);
        rc::st32(list + 0x54, 0);
    }
    if (rc::ld32(list) == list)
    {
        cpu.eax = list;
        cpu.ecx = view;
        cpu.esp = entry.esp + 12;
        cpu.flags.cf = false;
        cpu.flags.zf = true;
        return;
    }

    const reg32 device = rc::ld32(DeviceSlot);
    x86::CPU c = entry;
    c.ebx = list;
    c.esi = device;
    c.edi = 0;
    c.ebp = ebp;
    c.esp = esp0;
    if (!com(app, c, device, SetRenderState, { ZWriteEnable, 0 }, 0x4041f0))
    {
        cpu = c;
        return;
    }
    prepare(list, view);
    s_perf.systems += uint32_t(s_systems.size());
    s_perf.particles += uint32_t(s_particles.size());

    reg32 total = 0;
    for (const System& s : s_systems)
    {
        // Group switch, transforms and texture states (guest 0x404314-0x40433a).
        c.eax = Group0;
        c.ebx = s.system;
        c.ecx = view;
        c.edx = s.texture;
        c.edi = 0;
        c.esp = esp0;
        if (!call(app, c, &sub_40e6d0, 0x404320))
            break;
        c.ecx = view;
        c.edi = rc::ld32(view + 4);
        c.esp = esp0 - 4;
        rc::st32(c.esp, Identity);
        if (!call(app, c, &sub_410400, 0x404330))
            break;
        c.edx = s.texture;
        c.esp = esp0 - 8;
        rc::st32(c.esp, s.texture);
        if (!call(app, c, &sub_40f3d0, 0x40433a))
            break;
        c.esp = esp0;
        if (!com(app, c, currentBuffer(), Lock, { 0, 0, esp0 + 0xc, LockDiscard }, 0x4043ea))
            break;
        reg32 write = rc::ld32(esp0 + 0xc);
        uint32_t batch = 0, drawn = 0;
        bool running = true;
        for (uint32_t i = s.first; running && i < s.first + s.count; ++i)
        {
            const Quad& q = s_quads[i];
            if (!q.keep)
                continue;
            memcpy(rc::host(write), q.words, sizeof(q.words));
            write += sizeof(q.words);
            ++s_perf.quads;
            if (++batch != BatchParticles)
                continue;
            running = com(app, c, currentBuffer(), Unlock, {}, 0x404856)
                   && drawBatch(app, c, device, BatchParticles, FullBatchCalls);
            if (!running)
                break;
            advanceBuffer();
            running = com(app, c, currentBuffer(), Lock, { 0, 0, esp0 + 0xc, 0 }, 0x4048d5);
            write = rc::ld32(esp0 + 0xc);
            drawn += batch;
            batch = 0;
        }
        if (!running || !com(app, c, currentBuffer(), Unlock, {}, 0x404906))
            break;
        if (batch)
        {
            if (!drawBatch(app, c, device, batch, LastBatchCalls))
                break;
            advanceBuffer();
        }
        total += drawn + batch;
    }
    if (c.terminate)
    {
        cpu = c;
        return;
    }
    rc::st32(list + 0x54, rc::ld32(list + 0x54) + total);
    c.esp = esp0;
    com(app, c, device, SetRenderState, { ZWriteEnable, 1 }, 0x4049bb);
    cpu.eax = c.eax;
    cpu.ecx = c.ecx;
    cpu.edx = c.edx;
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = entry.esp + 12;
    cpu.terminate = c.terminate;
}

// Shadow: the native quads of the call against the vertices the guest body wrote into the vertex buffers, and the
// count, the buffer index and the list fields the guest updates.
struct Shadow
{
    uint32_t calls = 0, differing = 0, logged = 0, unchecked = 0;
};
Shadow s_shadow;
const uint32_t ShadowLogLimit = 20;

void drawShadow(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 list = rc::ld32(cpu.esp + 4), view = rc::ld32(cpu.esp + 8);
    const bool reset = rc::ld32(view + 4) == 1;
    const reg32 count54 = reset ? 0 : rc::ld32(list + 0x54), count58 = reset ? 0 : rc::ld32(list + 0x58);
    const reg32 index0 = rc::ld32(BufferIndex);
    const bool empty = rc::ld32(list) == list;
    std::vector<std::vector<reg32>> batches;    // expected words of each drawn batch
    reg32 total = 0;
    if (!empty)
    {
        prepare(list, view);
        for (const System& s : s_systems)
        {
            std::vector<reg32> current;
            uint32_t batch = 0;
            for (uint32_t i = s.first; i < s.first + s.count; ++i)
            {
                const Quad& q = s_quads[i];
                if (!q.keep)
                    continue;
                current.insert(current.end(), q.words, q.words + QuadWords);
                ++total;
                if (++batch == BatchParticles)
                {
                    batches.push_back(std::move(current));
                    current.clear();
                    batch = 0;
                }
            }
            if (batch)
                batches.push_back(std::move(current));
        }
    }

    sub_404110_guest(app, cpu);
    if (cpu.terminate)
        return;
    ++s_shadow.calls;
    char text[200] = {};
    const reg32 index = reg32((index0 + batches.size()) % BufferCount);
    bool same = rc::ld32(list + 0x54) == count54 + total && rc::ld32(list + 0x58) == count58
                && rc::ld32(BufferIndex) == index;
    if (!same)
        snprintf(text, sizeof(text), "count 0x%08x/0x%08x field 0x58 0x%08x/0x%08x buffer %u/%u",
                 unsigned(rc::ld32(list + 0x54)), unsigned(count54 + total), unsigned(rc::ld32(list + 0x58)),
                 unsigned(count58), unsigned(rc::ld32(BufferIndex)), unsigned(index));
    // Batches whose buffer a later batch of the call reused are not checked.
    const size_t first = batches.size() > BufferCount ? batches.size() - BufferCount : 0;
    s_shadow.unchecked += uint32_t(first);
    for (size_t b = first; same && b < batches.size(); ++b)
    {
        const reg32 guest = rc::ld32(VertexBuffers + 4 * reg32((index0 + b) % BufferCount));
        win32::d3d9::Object* object = win32::d3d9::lookup(guest);
        if (!object || object->kind() != win32::d3d9::KindVertexBuffer)
        {
            ++s_shadow.unchecked;
            continue;
        }
        const reg32 data = static_cast<win32::d3d9::Buffer*>(object)->shadowAddress();
        const std::vector<reg32>& words = batches[b];
        for (size_t i = 0; same && i < words.size(); ++i)
        {
            const reg32 g = rc::ld32(data + reg32(4 * i));
            if (g != words[i])
            {
                same = false;
                snprintf(text, sizeof(text), "batch %u quad %u vertex %u word %u: guest 0x%08x native 0x%08x",
                         unsigned(b), unsigned(i / QuadWords), unsigned(i % QuadWords / 6), unsigned(i % 6),
                         unsigned(g), unsigned(words[i]));
            }
        }
    }
    if (!same)
    {
        ++s_shadow.differing;
        if (s_shadow.logged < ShadowLogLimit)
        {
            ++s_shadow.logged;
            pf::logInfo("shadow_particles: call %u: %s", unsigned(s_shadow.calls), text);
        }
    }
}

}

void drawList(win32::WinApplication* app, x86::CPU& cpu)
{
    phases::Scope scope(phases::ParticleDraw);
    const Settings& s = settings();
    if (s.shadow)
    {
        drawShadow(app, cpu);
        return;
    }
    if (!s.native)
    {
        sub_404110_guest(app, cpu);
        return;
    }
    const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
    drawNative(app, cpu);
    if (pf::perf::enabled())
    {
        s_perf.us += pf::perf::now() - start;
        ++s_perf.calls;
    }
}

void report(uint32_t frames)
{
    const Settings& s = settings();
    if (s.shadow)
    {
        pf::logInfo("shadow_particles: %u calls compared, %u differ, %u batches not checked", unsigned(s_shadow.calls),
                    unsigned(s_shadow.differing), unsigned(s_shadow.unchecked));
        return;
    }
    if (!s.native || !frames)
        return;
    pf::logInfo("particles: native %.2f ms/frame, %u calls %u systems %u particles %u quads per frame",
                double(s_perf.us) / 1000.0 / frames, unsigned(s_perf.calls / frames), unsigned(s_perf.systems / frames),
                unsigned(s_perf.particles / frames), unsigned(s_perf.quads / frames));
    s_perf = PerfSums();
}

}

namespace native
{

void sub_404110(win32::WinApplication* app, x86::CPU& cpu)
{
    particles::drawList(app, cpu);
}

}
}
