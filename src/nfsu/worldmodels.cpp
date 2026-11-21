// Native scene parts of the world views (worldmodels.h): RenderWorldModels 0x592100 with WorldModel::Render
// 0x591e40 and SpaceNode::Update 0x5791c0, RenderClans 0x58a240 with the skid render 0x589da0 / 0x589810, the
// textured quad submit 0x40c300, MiniWorld::Render 0x4ae650 and the FE flares 0x570c70. Bit-exact with the
// translated guest code: x87 chains in double in the guest's order, floats where the guest stores floats, the same
// guest stores, the same device and effect calls in the same order, the stack temporaries the callees read at the
// guest's addresses, and the registers the callers read.

#include "worldmodels.h"
#include "phases.h"
#include "retstate.h"
#include "sceneshadow.h"

#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <recomp.h>
#include <vector>
#include <winapi/d3d9_internal.h>
#include <winapi/d3d9_native.h>
#include <winapi/d3dx9.h>

namespace nfsu
{

// Generated functions: the overridden guest bodies and the guest-convention entries the ports call.
void sub_592100_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_591e40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5791c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_58a240_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40c300_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4ae650_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_570c70_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4ae7d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_56d8d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5437f0(win32::WinApplication* app, x86::CPU& cpu);
void sub_40c910(win32::WinApplication* app, x86::CPU& cpu);
void sub_40db20(win32::WinApplication* app, x86::CPU& cpu);
void sub_40f3d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_410400(win32::WinApplication* app, x86::CPU& cpu);
void sub_56d8d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_5447c0(win32::WinApplication* app, x86::CPU& cpu);

namespace worldmodels
{

namespace
{

using x86::reg8;
using x86::reg32;
using x86::sreg32;
using draw::Span;
namespace bridge = win32::d3d9::bridge;
typedef win32::WinApplication App;
typedef void (*Function)(App*, x86::CPU&);

// Guest data.
const reg32 ArenaNext = 0x7355b4, ArenaEnd = 0x7355b8, ArenaFailed = 0x7355c4, ArenaFailedBytes = 0x7355c8;
const reg32 WorldModels = 0x73781c;     // list of world models (sentinel)
const reg32 ViewLayout = 0x7364a0;      // 3: views drawn only with the flag at +0x14
const reg32 RelativeOn = 0x700e8c;      // world models placed relative to another object
const reg32 PixelRadius = 0x6b5aac;     // radius of the pixel size test
const reg32 Half = 0x6cc7dc, One = 0x6cc7bc;
const reg32 SharedWorld = 0x78bcc0;     // identity matrix
const reg32 Clans = 0x737bc4;           // clan list (sentinel)
const reg32 DrawnSkids = 0x737bdc;      // list the drawn skids move to
const reg32 FrameCounter = 0x6f089c;
const reg32 SkidTextures = 0x731120;
const reg32 SkidLift = 0x6b5b04, FadeNear = 0x6b5b0c, FadeFar = 0x6b5b10, ByteMax = 0x6cc85c, SkidScale = 0x6cc7d4;
const reg32 QuadTexture = 0x736450;     // texture of the quads, set through the current group's effect
const reg32 CurrentGroup = 0x73640c, CurrentView = 0x736458, DeviceSlot = 0x73636c, StreamCache = 0x7363e8;
const reg32 LastModel = 0x736404, AlphaRef = 0x7070c0, WorldCalls = 0x71b1f4, LastView = 0x7010b8,
            LastGroup = 0x78eb7c;
const reg32 FlaresOn = 0x6f41a0, CarFlaresOn = 0x700ea0, MiniWorldOn = 0x6fb038, FeCars = 0x73d6d8;
const reg32 MiniStep = 0x6cc9f8, MiniParts = 0x735eac, MiniPartsLast = 0x735eb0, MiniY = 0x735e90, MiniZ = 0x6fb044;
const reg32 MiniPhase1 = 0x6fb048, MiniPhase4 = 0x6fb04c, MiniPhase10 = 0x6fb050;

const uint8_t Saved = retstate::Ebx | retstate::Esp | retstate::Ebp | retstate::Esi | retstate::Edi;

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("world_models_native", true),
                             pf::Config::getBool("shadow_world_models", false) };
        pf::logInfo("world_models: world models, clans, mini world, FE flares and the quad submit %s%s",
                    r.native ? "native" : "guest", r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

inline double ld(reg32 a) { return double(rc::ldf32(a)); }
inline float pass(reg32 a) { return float(double(rc::ldf32(a))); }
inline float pass(float v) { return float(double(v)); }
inline reg32 bits(float v) { reg32 r; memcpy(&r, &v, 4); return r; }
inline float real(reg32 v) { float r; memcpy(&r, &v, 4); return r; }
inline float negate(float v) { return float(-double(v)); }

// A guest store outside the stack.
inline void st(reg32 a, reg32 v) { sceneshadow::store(a, v); }
inline void stf(reg32 a, float v) { st(a, bits(v)); }

// Calls a guest-convention function with esp at the return address slot; false when the game is ending.
inline bool callAt(Function f, App* app, x86::CPU& c, reg32 slot, reg32 ret)
{
    rc::st32(slot, ret);
    c.esp = slot;
    f(app, c);
    return !c.terminate;
}

// The 0x401cd0 copy of a matrix: elements 1 and 2 of each row through the x87 stack, 0 and 3 as integers.
void copyMatrix(reg32 source, float* out)
{
    for (int row = 0; row < 4; ++row)
    {
        const reg32 r = source + 16 * row;
        out[4 * row] = real(rc::ld32(r));
        out[4 * row + 1] = pass(r + 4);
        out[4 * row + 2] = pass(r + 8);
        out[4 * row + 3] = real(rc::ld32(r + 12));
    }
}

void storeMatrix(reg32 address, const float* m)
{
    for (int i = 0; i < 16; ++i)
        st(address + 4 * i, bits(m[i]));
}

void stackMatrix(reg32 address, const float* m)
{
    memcpy(rc::host(address), m, 64);
}

void loadMatrix(reg32 address, float* m)
{
    memcpy(m, rc::host(address), 64);
}

// 0x401010: a frame arena block of size bytes, or 0 (then the failure is counted); ecx and edx as it leaves them.
reg32 arena(reg32 size, reg32& ecx, reg32& edx)
{
    edx = rc::ld32(ArenaNext);
    size = (size + 0xf) & ~0xfu;
    ecx = edx + size;
    if (ecx < rc::ld32(ArenaEnd))
    {
        st(ArenaNext, ecx);
        return edx;
    }
    ecx = rc::ld32(ArenaFailedBytes) + size;
    st(ArenaFailed, 1);
    st(ArenaFailedBytes, ecx);
    return 0;
}

// SpaceNode updates WorldModel::Render made (counted for the scene snapshot's check of updateNodes), and their
// nodes while a list is set.
uint32_t s_viewUpdates = 0;
std::vector<reg32>* s_updatedNodes = nullptr;

// ---- 0x5791c0: SpaceNode::Update ----

// The node's world matrix (+0x10) and position (+0x90) from its local ones and its parent's, the parent updated
// first when it is marked (+0xe4).
void spaceNode(reg32 node)
{
    reg32 parent = rc::ld32(node + 0xe0);
    if (parent == 0)
    {
        float m[16];
        copyMatrix(node + 0x50, m);
        storeMatrix(node + 0x10, m);
        const float y = pass(node + 0xa4), z = pass(node + 0xa8);
        st(node + 0x90, rc::ld32(node + 0xa0));
        stf(node + 0x94, y);
        stf(node + 0x98, z);
        st(node + 0xe4, 0);
        return;
    }
    if (rc::ld32(parent + 0xe4))
        spaceNode(parent);
    float a[16], b[16], m[16];
    loadMatrix(node + 0x50, a);
    loadMatrix(parent + 0x10, b);
    bridge::matrixMultiply(a, b, m);
    storeMatrix(node + 0x10, m);
    parent = rc::ld32(node + 0xe0);
    if (rc::ld32(parent + 0xe4))
        spaceNode(parent);
    float v[4] = { 0, 0, 0, 1 }, p[4];
    memcpy(v, rc::host(node + 0xa0), 12);
    loadMatrix(parent + 0x10, m);
    win32::d3dx9::transform(v, m, p);
    parent = rc::ld32(node + 0xe0);
    if (rc::ld32(parent + 0xe4))
        spaceNode(parent);
    const double py = ld(parent + 0x44), pz = ld(parent + 0x48);
    p[0] = float(double(p[0]) - ld(parent + 0x40));
    p[1] = float(double(p[1]) - py);
    p[2] = float(double(p[2]) - pz);
    parent = rc::ld32(node + 0xe0);
    if (rc::ld32(parent + 0xe4))
        spaceNode(parent);
    const float x = float(double(p[0]) + ld(parent + 0x90));
    const float y = float(ld(parent + 0x94) + double(p[1]));
    const float z = float(ld(parent + 0x98) + double(p[2]));
    stf(node + 0x90, x);
    stf(node + 0x98, z);
    stf(node + 0x94, y);
    st(node + 0xe4, 0);
}

// ---- 0x591e40: WorldModel::Render ----

// 0x545180: the pixel size of a sphere (centre at pos, radius bits r) in the view; edx as the guest leaves it
// (the view, or the high half of the _ftol2 result).
reg32 pixelSize(const float pos[3], reg32 view, reg32 r, reg32& edx)
{
    const reg32 cam = rc::ld32(view + 0x40);
    const double dx = double(pos[0]) - ld(cam + 0x40);
    const double dy = double(pos[1]) - ld(cam + 0x44);
    const double dz = double(pos[2]) - ld(cam + 0x48);
    const double dot = (dz * ld(cam + 0x58) + dy * ld(cam + 0x54)) + dx * ld(cam + 0x50);
    const double radius = double(real(r));
    if (-radius > dot)
    {
        edx = view;
        return 0;
    }
    const double distance = std::sqrt((dz * dz + dy * dy) + dx * dx) - radius;
    const double size = ld(view + 0xc);
    const double value = distance > radius * ld(Half) ? size / distance * radius : size + size;
    const x86::reg64 n = x86::reg64(x86::truncInteger64(value));
    edx = reg32(n >> 32);
    return reg32(n);
}

// 0x53f5f0: the bounding box of a model (min floats through the x87 stack, max as integers); 0 without one.
bool modelBox(reg32 model, float mn[3], reg32 mx[3], reg32& edx, reg32 minAddress)
{
    const reg32 box = rc::ld32(model + 0xc);
    if (box == 0)
    {
        mn[0] = mn[1] = mn[2] = 0.0f;
        mx[0] = mx[1] = mx[2] = 0;
        edx = minAddress;
        return false;
    }
    for (int i = 0; i < 3; ++i)
    {
        mn[i] = pass(box + 0x20 + 4 * i);
        mx[i] = rc::ld32(box + 0x30 + 4 * i);
    }
    edx = mx[2];
    return true;
}

// [esp + 4] = the world model, [esp + 8] = the view, eax = the render flags (0x800: mirrored). ret 8.
void renderModel(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, wm = rc::ld32(E + 4), view = rc::ld32(E + 8);
    const reg32 ebp = E - 4, S = ((ebp & ~15u) - 0xc4) - 0xc;
    const bool mirrored = (cpu.eax & 0x800) != 0;
    reg32 ecx = cpu.ecx, edx = wm;
    const auto leave = [&]()
    {
        cpu.ecx = ecx;
        cpu.edx = edx;
        cpu.ebx = entry.ebx;
        cpu.esi = entry.esi;
        cpu.edi = entry.edi;
        cpu.ebp = entry.ebp;
        cpu.esp = E + 12;
    };
    reg32 model = rc::ld32(wm + 8);
    if (mirrored)
    {
        const reg32 other = rc::ld32(wm + 0xc);
        if (other == 0)
            return leave();
        model = other;
    }
    if (model == 0 || rc::ld32(model + 0xc) == 0)
        return leave();
    const reg32 node = rc::ld32(wm + 0x60);
    reg32 relative = 0, matrix;
    float world[16];
    bool multiplied = false;
    if (node != 0)
    {
        if (rc::ld32(node + 0xe4))
        {
            ++s_viewUpdates;
            if (s_updatedNodes)
                s_updatedNodes->push_back(node);
            spaceNode(node);
        }
        relative = rc::ld32(rc::ld32(wm + 0x60) + 0xe8);
        matrix = node + 0x10;
        ecx = matrix;
        if (rc::ld32(RelativeOn) == 0)
        {
            if (relative)
                return leave();
        }
        else if (relative)
        {
            float a[16], b[16];
            loadMatrix(relative + 0x40, a);
            loadMatrix(node + 0x10, b);
            bridge::matrixMultiply(a, b, world);
            ecx = relative + 0x40;
            edx = S + 0x90;
            multiplied = true;
        }
    }
    else
    {
        matrix = wm + 0x20;
    }
    if (!multiplied)
    {
        copyMatrix(matrix, world);
        ecx = edx = rc::ld32(matrix + 0x3c);
    }
    stackMatrix(S + 0x90, world);
    const reg32 flags = relative ? 0 : 0x200;
    rc::st32(S + 0x1c, flags);
    rc::st32(S + 0xc, matrix);
    ecx = S + 0xc0;
    const reg32 size = pixelSize(world + 12, view, rc::ld32(PixelRadius), edx);
    if (sreg32(size) < 2)
        return leave();

    // 0x591b40: the model's box through the world matrix against the view.
    {
        const reg32 E3 = S - 0xc, G = (((E3 - 4) & ~15u) - 0x20);
        float mn[3];
        reg32 mx[3];
        reg32 e;
        modelBox(model, mn, mx, e, G + 0x10);
        for (int i = 0; i < 3; ++i)
        {
            rc::st32(G + 0x10 + 4 * i, bits(mn[i]));
            rc::st32(G + 4 * i, mx[i]);
        }
        rc::st32(S - 4, S + 0x90);
        rc::st32(S - 8, view);
        rc::st32(G - 4, view);
        x86::CPU c = cpu;
        c.eax = S + 0x90;
        c.ecx = G;
        c.edx = G + 0x10;
        c.ebx = model;
        c.esi = relative;
        c.edi = view;
        c.ebp = E3 - 4;
        if (!callAt(&sub_40db20, app, c, G - 8, 0x591b6a))
        {
            cpu.terminate = true;
            return;
        }
        ecx = c.ecx;
        edx = c.edx;
        if (c.eax == 0)
            return leave();
    }
    reg32 minAddress = 0, maxAddress = 0;
    if (relative)
    {
        const reg32 box = rc::ld32(model + 0xc);
        minAddress = box + 0x20;
        maxAddress = box + 0x30;
        for (int i = 0; i < 3; ++i)
        {
            const double offset = ld(relative + 0x70 + 4 * i);
            stf(minAddress + 4 * i, float(ld(minAddress + 4 * i) + offset));
            stf(maxAddress + 4 * i, float(offset + ld(maxAddress + 4 * i)));
        }
        ecx = maxAddress;
    }
    const reg32 placed = arena(0x40, ecx, edx);
    if (placed != 0)
    {
        float m[16];
        copyMatrix(matrix, m);
        storeMatrix(placed, m);
        ecx = edx = rc::ld32(matrix + 0x3c);
        if (mirrored)
            stf(placed + 0x28, negate(rc::ldf32(placed + 0x28)));
        reg32 context = 0;
        x86::CPU c = cpu;
        c.ebp = ebp;
        c.esi = relative;
        c.edi = placed;
        if (rc::ld8(wm + 0x1c) != 0)
        {
            context = arena(0x1c0, ecx, edx);
            if (context == 0)
                return leave();
            rc::st32(S + 0xc, context);
            const reg32 cam = rc::ld32(view + 0x40);
            rc::st32(S + 0x40, rc::ld32(cam + 0x40));
            rc::st32(S + 0x44, rc::ld32(cam + 0x44));
            rc::st32(S + 0x48, rc::ld32(cam + 0x48));
            rc::st32(S + 0x4c, 0x3f800000);
            reg32 lightMatrix = placed;
            if (relative)
            {
                // The light context's matrix: the world matrix with the offset point through it as its position.
                const float v[4] = { real(rc::ld32(relative + 0x70)), pass(relative + 0x74), pass(relative + 0x78),
                                     pass(relative + 0x7c) };
                float wm4[16], copy[16], t[4];
                loadMatrix(placed, wm4);
                win32::d3dx9::transform(v, wm4, t);
                copyMatrix(placed, copy);
                memcpy(copy + 12, t, 16);
                stackMatrix(S + 0x50, copy);
                lightMatrix = S + 0x50;
            }
            rc::st32(S - 0x14, context);
            rc::st32(S - 0x10, lightMatrix);
            rc::st32(S - 0xc, cam);
            rc::st32(S - 8, S + 0x40);
            rc::st32(S - 4, view);
            c.eax = relative ? S + 0x50 : context;
            c.ecx = relative ? context : S + 0x40;
            c.edx = relative ? S + 0x40 : view;
            c.ebx = cam;
            if (!callAt(&sub_5437f0, app, c, S - 0x18, 0x59208b))
            {
                cpu.terminate = true;
                return;
            }
        }
        else
        {
            c.ebx = model;
            ecx &= ~0xffu;
        }
        rc::st32(S - 0x14, view);
        rc::st32(S - 0x10, model);
        rc::st32(S - 0xc, placed);
        rc::st32(S - 8, context);
        rc::st32(S - 4, relative);
        c.eax = flags;
        c.ecx = context ? view : ecx;
        c.edx = context ? context : view;
        rc::st32(S - 0x18, 0x5920b2);
        c.esp = S - 0x18;
        if (!sceneshadow::call(0x40c910, 5, retstate::Eax, app, c, &sub_40c910))
        {
            cpu.terminate = true;
            return;
        }
        ecx = c.ecx;
        edx = c.edx;
    }
    if (relative)
    {
        for (int i = 0; i < 3; ++i)
        {
            const double offset = ld(relative + 0x70 + 4 * i);
            stf(minAddress + 4 * i, float(ld(minAddress + 4 * i) - offset));
            stf(maxAddress + 4 * i, float(ld(maxAddress + 4 * i) - offset));
        }
        ecx = maxAddress;
    }
    leave();
}

// ---- 0x592100: RenderWorldModels ----

// 0x40bc10: the inverse of a rotation and translation matrix (the transposed rotation after the negated
// translation).
void inverse(const float m[16], float out[16])
{
    const float r[16] = { m[0], pass(m[4]), pass(m[8]), 0.0f, pass(m[1]), pass(m[5]), pass(m[9]), 0.0f,
                          pass(m[2]), pass(m[6]), pass(m[10]), 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    const float t[16] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                          negate(m[12]), negate(m[13]), negate(m[14]), 1.0f };
    bridge::matrixMultiply(t, r, out);
}

// [esp + 4] = the view, [esp + 8] = the render flags of 0x591e40. cdecl.
void renderModels(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, view = rc::ld32(E + 4), flags = rc::ld32(E + 8);
    const reg32 S = (((E - 4) & ~15u) - 0x84) - 0xc;
    const bool layout3 = rc::ld32(ViewLayout) == 3;
    reg32 ecx = cpu.ecx, edx = rc::ld32(ViewLayout);
    x86::CPU c = cpu;
    c.ebx = view;
    c.ebp = E - 4;
    for (reg32 node = rc::ld32(WorldModels); node != WorldModels; node = rc::ld32(node))
    {
        if (rc::ld32(node + 0x10) == 0 || (layout3 && rc::ld32(node + 0x14) == 0))
            continue;
        if (rc::ld32(node + 0x18) != 0)
        {
            // 0x424ba0: the matrix with the position as its last row.
            const reg32 space = rc::ld32(node + 0x60);
            const reg32 pos = space ? space + 0x80 : node + 0x50, source = space ? space + 0x50 : node + 0x20;
            float m[16];
            copyMatrix(source, m);
            m[12] = real(rc::ld32(pos));
            m[13] = pass(pos + 4);
            m[14] = pass(pos + 8);
            m[15] = 1.0f;
            float inv[16];
            inverse(m, inv);
            ecx = bits(m[0]);
            float mn[3];
            reg32 mx[3];
            modelBox(rc::ld32(node + 8), mn, mx, edx, S + 0x40);
            // The camera in the model's space, inside the box: not drawn (0x567940 with a margin of 0).
            const reg32 cam = rc::ld32(view + 0x40);
            float in[4] = { 0, 0, 0, 1 }, p[4];
            memcpy(in, rc::host(cam + 0x40), 12);
            win32::d3dx9::transform(in, inv, p);
            ecx = S + 0x20;
            edx = S + 0x40;
            bool inside = true;
            for (int i = 0; inside && i < 3; ++i)
            {
                if (double(mn[i]) - 0.0 > double(p[i]) || 0.0 + double(real(mx[i])) < double(p[i]))
                    inside = false;
            }
            if (inside)
                continue;
        }
        c.eax = flags;
        c.ecx = ecx;
        c.edx = edx;
        c.edi = node;
        rc::st32(S - 8, node);
        rc::st32(S - 4, view);
        if (!callAt(&renderModel, app, c, S - 0xc, 0x5921d4))
        {
            cpu.terminate = true;
            return;
        }
        ecx = c.ecx;
        edx = c.edx;
    }
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// The world models RenderWorldModels draws in view with the render flags 0 (no mirror): the same selection as
// renderModels and the early returns of renderModel; their marked space nodes updated in the list's order.
uint32_t updateDrawnNodes(reg32 view, std::vector<reg32>* dry)
{
    uint32_t updated = 0;
    const bool layout3 = rc::ld32(ViewLayout) == 3;
    for (reg32 node = rc::ld32(WorldModels); node != WorldModels; node = rc::ld32(node))
    {
        if (rc::ld32(node + 0x10) == 0 || (layout3 && rc::ld32(node + 0x14) == 0))
            continue;
        if (rc::ld32(node + 0x18) != 0)
        {
            const reg32 space = rc::ld32(node + 0x60);
            const reg32 pos = space ? space + 0x80 : node + 0x50, source = space ? space + 0x50 : node + 0x20;
            float m[16];
            copyMatrix(source, m);
            m[12] = real(rc::ld32(pos));
            m[13] = pass(pos + 4);
            m[14] = pass(pos + 8);
            m[15] = 1.0f;
            float inv[16];
            inverse(m, inv);
            float mn[3];
            reg32 mx[3], edx = 0;
            modelBox(rc::ld32(node + 8), mn, mx, edx, 0);
            const reg32 cam = rc::ld32(view + 0x40);
            float in[4] = { 0, 0, 0, 1 }, p[4];
            memcpy(in, rc::host(cam + 0x40), 12);
            win32::d3dx9::transform(in, inv, p);
            bool inside = true;
            for (int i = 0; inside && i < 3; ++i)
            {
                if (double(mn[i]) - 0.0 > double(p[i]) || 0.0 + double(real(mx[i])) < double(p[i]))
                    inside = false;
            }
            if (inside)
                continue;
        }
        const reg32 model = rc::ld32(node + 8);
        if (model == 0 || rc::ld32(model + 0xc) == 0)
            continue;
        const reg32 space = rc::ld32(node + 0x60);
        if (space != 0 && rc::ld32(space + 0xe4))
        {
            if (dry)
                dry->push_back(space);
            else
                spaceNode(space);
            ++updated;
        }
    }
    return updated;
}

// ---- 0x40c300: textured quad ----

// eax = the quad record (4 positions at +0x10, uv at +0x50 every 0xc bytes, colour bytes at +0x80), [esp + 4] =
// the texture, [esp + 8] = the world matrix. cdecl. The vertices go to the device from the guest's stack frame
// layout: 0x20 bytes each, the last two dwords as the stack holds them.
bool quadObjects()
{
    const win32::d3d9::Object* device = win32::d3d9::lookup(rc::ld32(DeviceSlot));
    if (!device || device->kind() != win32::d3d9::KindDevice)
        return false;
    if (rc::ld32(QuadTexture) == 0)
        return true;
    const reg32 group = rc::ld32(CurrentGroup);
    const win32::d3d9::Object* effect = group ? win32::d3d9::lookup(rc::ld32(group + 0x18)) : nullptr;
    return effect && effect->kind() == win32::d3d9::KindEffect;
}

void quadNative(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, T = E - 0x80, q = cpu.eax;
    const reg32 texture = rc::ld32(E + 4), matrix = rc::ld32(E + 8);
    reg32* v = reinterpret_cast<reg32*>(rc::host(T));
    for (reg32 k = 0; k < 4; ++k)
    {
        const reg32 o = 0x80 + 4 * k;
        v[8 * k] = rc::ld32(q + 0x10 + 0x10 * k);
        v[8 * k + 1] = rc::ld32(q + 0x14 + 0x10 * k);
        v[8 * k + 2] = rc::ld32(q + 0x18 + 0x10 * k);
        v[8 * k + 3] = (reg32(rc::ld8(q + o + 3)) << 24) | (reg32(rc::ld8(q + o)) << 16)
                       | (reg32(rc::ld8(q + o + 1)) << 8) | reg32(rc::ld8(q + o + 2));
        v[8 * k + 4] = rc::ld32(q + 0x50 + 0xc * k);
        v[8 * k + 5] = rc::ld32(q + 0x54 + 0xc * k);
    }
    x86::CPU c = cpu;
    c.eax = texture;
    c.ecx = rc::ld32(q + 0x74);
    c.edx = rc::ld32(q + 0x78);
    rc::st32(T - 4, entry.edi);
    rc::st32(T - 8, texture);
    if (!callAt(&sub_40f3d0, app, c, T - 0xc, 0x40c448))
    {
        cpu.terminate = true;
        return;
    }
    c.ecx = matrix;
    c.edx = rc::ld32(CurrentView);
    c.edi = rc::ld32(c.edx + 4);
    rc::st32(T - 0xc, matrix);
    if (!callAt(&sub_410400, app, c, T - 0x10, 0x40c45e))
    {
        cpu.terminate = true;
        return;
    }
    c.esp = T;
    c.edi = entry.edi;
    const reg32 quadTexture = rc::ld32(QuadTexture);
    if (quadTexture != 0)
    {
        const reg32 group = rc::ld32(CurrentGroup);
        const reg32 effect = rc::ld32(group + 0x18), handle = rc::ld32(group + 0x40);
        c.eax = reg32(bridge::effectSetTexture(app, c, effect, handle, quadTexture));
        if (c.terminate)
        {
            cpu = c;
            return;
        }
    }
    const reg32 device = rc::ld32(DeviceSlot);
    st(StreamCache, 0);
    c.eax = reg32(bridge::drawPrimitiveUP(app, c, device, 6, 2, v, 0x20));
    if (c.terminate)
    {
        cpu = c;
        return;
    }
    cpu.eax = c.eax;
    cpu.ecx = T;
    cpu.edx = rc::ld32(device);
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

void quad(App* app, x86::CPU& cpu)
{
    if (!quadObjects())
    {
        sub_40c300_guest(app, cpu);
        return;
    }
    quadNative(app, cpu);
}

// ---- 0x58a240: RenderClans ----

// 0x545210: the pixel size of the box between a and b (its centre and half diagonal) in the view; edx as the
// guest leaves it.
reg32 boxPixels(reg32 a, reg32 b, reg32 view, reg32& edx)
{
    const reg32 cam = rc::ld32(view + 0x40);
    const double h = ld(Half);
    const double ex = ld(b) - ld(a), ey = ld(b + 4) - ld(a + 4), ez = ld(b + 8) - ld(a + 8);
    const float half = float(std::sqrt((ez * ez + ey * ey) + ex * ex) * h);
    const double cx = (ld(b) + ld(a)) * h - ld(cam + 0x40);
    const double cy = (ld(b + 4) + ld(a + 4)) * h - ld(cam + 0x44);
    const double cz = (ld(b + 8) + ld(a + 8)) * h - ld(cam + 0x48);
    const double dot = (cz * ld(cam + 0x58) + cy * ld(cam + 0x54)) + cx * ld(cam + 0x50);
    if (-double(half) > dot)
    {
        edx = b;
        return 0;
    }
    const float t = float(std::sqrt((cz * cz + cy * cy) + cx * cx) - double(half));
    double value = ld(view + 0xc);
    if (double(t) > double(half))
        value = ld(view + 0xc) / double(t) * double(half);
    const x86::reg64 n = x86::reg64(x86::truncInteger64(value));
    edx = reg32(n >> 32);
    return reg32(n);
}

// 0x589350: the two edge points of a skid segment (position, then the signed direction bytes at +0xc scaled).
void skidPoints(reg32 segment, reg32 outA, reg32 outB)
{
    const double k = ld(SkidScale);
    const double x = ld(segment), y = ld(segment + 4);
    const reg32 z = rc::ld32(segment + 8);
    const double sx = double(sreg32(int8_t(rc::ld8(segment + 0xc)))) * k;
    const float sy = float(double(sreg32(int8_t(rc::ld8(segment + 0xd)))) * k);
    const float sz = float(double(sreg32(int8_t(rc::ld8(segment + 0xe)))) * k);
    rc::st32(outB, bits(float(sx + x)));
    rc::st32(outB + 4, bits(float(double(sy) + y)));
    rc::st32(outB + 8, bits(float(double(sz) + double(real(z)))));
    rc::st32(outA, bits(float(x - sx)));
    rc::st32(outA + 4, bits(float(y - double(sy))));
    rc::st32(outA + 8, bits(float(double(real(z)) - double(sz))));
}

// 0x40be70: the quad record's defaults.
void quadDefaults(reg32 q)
{
    static const struct { reg32 offset, value; } Words[] = {
        {0x20, 0x42800000}, {0x30, 0x42800000}, {0x34, 0x42800000}, {0x44, 0x42800000},
        {0x80, 0x80808080}, {0x84, 0x80808080}, {0x88, 0x80808080}, {0x8c, 0x80808080},
        {0x10, 0}, {0x14, 0}, {0x18, 0}, {0x24, 0}, {0x28, 0}, {0x38, 0}, {0x40, 0}, {0x48, 0}, {0x50, 0},
        {0x54, 0}, {0x58, 0x3f800000}, {0x5c, 0x3f800000}, {0x60, 0}, {0x64, 0x3f800000}, {0x68, 0x3f800000},
        {0x6c, 0x3f800000}, {0x70, 0x3f800000}, {0x74, 0}, {0x78, 0x3f800000}, {0x7c, 0x3f800000}, {0x90, 0},
    };
    for (const auto& w : Words)
        rc::st32(q + w.offset, w.value);
}

// 0x589810: edi = the skid, [esp + 4] = the view, byte [esp + 8] = the fade. ret 8. The segments' quads.
bool skidQuads(App* app, x86::CPU& cpu, reg32 skid, reg8 fade)
{
    const reg32 E = cpu.esp;
    const reg32 S = (((E - 4) & ~15u) - 0xa8) - 8;
    const reg32 texture = rc::ld32(SkidTextures + 4 * rc::ld32(skid + 0x38));
    if (texture == 0)
        return true;
    const reg32 q = S + 0x10;
    quadDefaults(q);
    cpu.ecx = 0;
    cpu.edx = 0x3f800000;
    const sreg32 count = sreg32(rc::ld32(skid + 0xf0)) - 1;
    const double lift = ld(SkidLift);
    rc::st32(S + 0xc, rc::ld32(SkidLift));
    if (count <= 0)
    {
        cpu.ecx = rc::ld32(SkidLift);
        cpu.edx = reg32(count);
        return true;
    }
    reg32 segment = skid + 0x8f;
    x86::CPU c = cpu;
    c.edi = skid;
    c.ebp = E - 4;
    for (sreg32 i = 0; i < sreg32(rc::ld32(skid + 0xf0)) - 1; ++i, segment += 0x10)
    {
        // 0x589350 pushes esi below its frame: the last dword of the fourth vertex.
        rc::st32(S - 0x14, segment);
        skidPoints(segment - 0x1f, S + 0x50, S + 0x20);
        skidPoints(segment - 0xf, S + 0x40, S + 0x30);
        for (reg32 z : { S + 0x28, S + 0x38, S + 0x48, S + 0x58 })
            rc::st32(z, bits(float(ld(z) + lift)));
        const reg8 a = rc::ld8(segment - 0x10), b = rc::ld8(segment);
        const reg8 first = reg8((fade > a ? 0 : a - fade) >> 1), second = reg8((fade > b ? 0 : b - fade) >> 1);
        rc::st8(S + 0x93, first);
        rc::st8(S + 0x9f, first);
        rc::st8(S + 0x97, second);
        rc::st8(S + 0x9b, second);
        rc::st32(S - 4, 0);
        rc::st32(S - 8, SharedWorld);
        rc::st32(S - 0xc, rc::ld32(SkidTextures + 4 * rc::ld32(skid + 0x38)));
        c.eax = q;
        c.ebx = reg32(i);
        c.esi = segment;
        rc::st32(S - 0x10, 0x589907);
        c.esp = S - 0x10;
        quad(app, c);
        if (c.terminate)
        {
            cpu.terminate = true;
            return false;
        }
    }
    cpu.ecx = c.ecx;
    cpu.edx = rc::ld32(skid + 0xf0) - 1;
    return true;
}

// Moves node to the front of the list at head (prev at +4).
void toFront(reg32 node, reg32 head, reg32& ecx)
{
    const reg32 next = rc::ld32(node), previous = rc::ld32(node + 4);
    st(previous, next);
    st(next + 4, previous);
    const reg32 first = rc::ld32(head);
    st(head, node);
    st(first + 4, node);
    st(node + 4, head);
    st(node, first);
    ecx = previous;
}

// 0x589da0: esi = the view, [esp + 4] = the clan. cdecl. The clan's visible skids, faded by pixel size, then moved
// to the drawn list.
bool renderSkids(App* app, x86::CPU& cpu, reg32 view, reg32 clan, reg32& ecx, reg32& edx)
{
    if (rc::ld8(view + 0xa) != 0)
        return true;
    const reg32 E = cpu.esp, S = E - 8 - 0xc;
    const reg32 head = clan + 8;
    for (reg32 link = rc::ld32(head); link != head; link = rc::ld32(link))
    {
        const reg32 skid = rc::ld32(link + 8);
        const reg32 car = rc::ld32(skid + 0x24);
        if (car != 0)
        {
            ecx = rc::ld32(view + 4);
            edx = 1u << (ecx & 31);
            if ((rc::ld32(car + 0x534) & edx) == 0)
                continue;
        }
        x86::CPU c = cpu;
        c.eax = 0;
        c.ecx = skid + 0x50;
        c.edx = skid + 0x60;
        c.ebx = head;
        c.ebp = link;
        c.edi = skid;
        rc::st32(S - 4, view);
        if (!callAt(&sub_40db20, app, c, S - 8, 0x589df0))
        {
            cpu.terminate = true;
            return false;
        }
        ecx = c.ecx;
        edx = c.edx;
        if (c.eax == 0)
            continue;
        const reg32 cam = rc::ld32(view + 0x40);
        const double dx = ld(skid + 0x100) - ld(cam + 0x40);
        const double dy = ld(skid + 0x104) - ld(cam + 0x44);
        const double dz = ld(skid + 0x108) - ld(cam + 0x48);
        const double distance = std::sqrt((dz * dz + dy * dy) + dx * dx);
        const double scaled = ld(Half) * distance;
        const double value = scaled < ld(One) ? distance * ld(view + 0xc) : ld(view + 0xc) + ld(view + 0xc);
        const x86::reg64 n = x86::reg64(x86::truncInteger64(value));
        edx = reg32(n >> 32);
        const double whole = double(sreg32(reg32(n)));
        const float pixels = float(whole);
        rc::st32(S + 0xc, bits(pixels));
        const double nearer = ld(FadeNear), farther = ld(FadeFar);
        if (!(whole > nearer))
            continue;
        if (double(pixels) > farther)
        {
            rc::st8(S + 0x10, 0);
        }
        else
        {
            const double k = ld(ByteMax);
            const double t = ld(ByteMax) - (double(pixels) - nearer) * k / (farther - nearer);
            const x86::reg64 m = x86::reg64(x86::truncInteger64(t));
            edx = reg32(m >> 32);
            rc::st8(S + 0x10, reg8(m));
        }
        const reg32 word = rc::ld32(S + 0x10);
        rc::st32(S - 4, word);
        rc::st32(S - 8, view);
        rc::st32(S - 0xc, 0x589ec0);
        c = cpu;
        c.esp = S - 0xc;
        c.ecx = ecx;
        c.edx = edx;
        if (!skidQuads(app, c, skid, reg8(word)))
        {
            cpu.terminate = true;
            return false;
        }
        edx = c.edx;
        toFront(skid, DrawnSkids, ecx);
    }
    return true;
}

// eax = the view. cdecl.
void renderClans(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, view = cpu.eax;
    const reg32 S = E - 0x10 - 8;
    reg32 ecx = cpu.ecx, edx = cpu.edx;
    x86::CPU c = cpu;
    c.esi = view;
    for (reg32 clan = rc::ld32(Clans); clan != Clans;)
    {
        const reg32 next = rc::ld32(clan);
        reg32 e = edx;
        const reg32 pixels = boxPixels(clan + 0x40, clan + 0x30, view, e);
        edx = e;
        ecx = clan + 0x40;
        if (sreg32(pixels) > 10)
        {
            c.eax = 0;
            c.ecx = clan + 0x40;
            c.edx = clan + 0x30;
            c.ebx = clan + 0x40;
            c.ebp = clan + 0x30;
            c.edi = clan;
            rc::st32(S - 4, view);
            if (!callAt(&sub_40db20, app, c, S - 8, 0x58a287))
            {
                cpu.terminate = true;
                return;
            }
            ecx = c.ecx;
            edx = c.edx;
            if (c.eax != 0)
            {
                const reg32 now = rc::ld32(FrameCounter);
                ecx = now - rc::ld32(clan + 0x14);
                if (sreg32(ecx) > 0x12c)
                {
                    st(clan + 0x14, now);
                    toFront(clan, Clans, ecx);
                }
                rc::st32(S - 4, clan);
                c.esp = S - 8;
                rc::st32(S - 8, 0x58a2c9);
                if (!renderSkids(app, c, view, clan, ecx, edx))
                {
                    cpu.terminate = true;
                    return;
                }
            }
        }
        clan = next;
    }
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// ---- 0x4ae650: MiniWorld::Render ----

// 0x4ae530: esi = the mini world, [esp + 4] = the view. The rows to draw and their placement; false when none.
bool miniSetup(reg32 world, reg32 view, reg32& ecx, reg32& edx)
{
    for (reg32 o = 0x1328; o <= 0x1338; o += 4)
        st(world + o, 0);
    if (rc::ld32(world + 0x18) == 0 || rc::ld32(MiniWorldOn) == 0)
        return false;
    const sreg32 id = sreg32(rc::ld32(view + 4));
    reg32 phase, row = 0, kind;
    if (id >= 4 && id <= 5)
        phase = rc::ld32(MiniPhase4), row = 1, kind = 2;
    else if (id >= 10 && id <= 15)
        phase = rc::ld32(MiniPhase10), row = 2, kind = 1;
    else if (id >= 1 && id <= 3)
        phase = rc::ld32(MiniPhase1), kind = 0;
    else
        return false;
    const x86::reg64 t = x86::reg64(x86::truncInteger64(ld(world + 4) / ld(world) + ld(world + 4) / ld(world)));
    const reg32 n = reg32(t);
    edx = reg32(t >> 32);
    sreg32 parity = sreg32(n - phase) % 2;
    if (parity < 0)
    {
        edx = (0xffffffffu - reg32(parity)) >> 1;
        parity += 2;
    }
    const double k = ld(MiniStep);
    st(world + 0x1338, kind);
    st(world + 0x1328, reg32(parity));
    st(world + 0x1330, row);
    st(world + 0x132c, phase + phase + 1);
    stf(world + 0x1334, float((ld(world + 4) - double(sreg32(n)) * k) - double(sreg32(phase)) * k));
    ecx = n;
    return true;
}

// [esp + 4] = the mini world, [esp + 8] = the view. ret 8.
void renderMini(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, world = rc::ld32(E + 4), view = rc::ld32(E + 8);
    const reg32 S = E - 0x2c;
    reg32 ecx = cpu.ecx, edx = cpu.edx;
    const auto leave = [&]()
    {
        cpu.ecx = ecx;
        cpu.edx = edx;
        cpu.ebx = entry.ebx;
        cpu.esi = entry.esi;
        cpu.edi = entry.edi;
        cpu.ebp = entry.ebp;
        cpu.esp = E + 12;
    };
    if (!miniSetup(world, view, ecx, edx))
        return leave();
    const reg32 lights = rc::ld32(view + 4) == 1 ? 0x740760 : 0x7407c0;
    reg32 phase = rc::ld32(world + 0x1328);
    const reg32 row = rc::ld32(world + 0x1330);
    float z = real(rc::ld32(world + 0x1334));
    reg32 count = rc::ld32(world + 0x132c);
    const reg32 y = rc::ld32(MiniY), height = rc::ld32(MiniZ);
    ecx = y;
    edx = height;
    if (count == 0)
        return leave();
    ecx = rc::ld32(ArenaNext);
    x86::CPU c = cpu;
    c.ebp = E - 4;
    do
    {
        reg32 parts = world + 0xd50 + phase * 0x2e0;
        const reg32 block = ecx + 0x40;
        edx = rc::ld32(ArenaEnd);
        if (block < edx)
        {
            const reg32 placed = ecx;
            st(ArenaNext, block);
            ecx = block;
            if (placed != 0)
            {
                float m[16];
                copyMatrix(SharedWorld, m);
                storeMatrix(placed, m);
                stf(placed + 0x30, negate(z));
                st(placed + 0x34, y);
                st(placed + 0x38, height);
                parts += row * 0x18;
                c.edi = placed;
                for (reg32 i = 0; i < 10; ++i, parts += 0x48)
                {
                    const bool lit = sreg32(i) >= sreg32(rc::ld32(MiniParts))
                                     && sreg32(i) <= sreg32(rc::ld32(MiniPartsLast)) && rc::ld8(lights + 8) != 0;
                    rc::st32(S - 0x14, view);
                    rc::st32(S - 0x10, parts);
                    rc::st32(S - 0xc, placed);
                    rc::st32(S - 8, 0);
                    rc::st32(S - 4, 0);
                    c.eax = lit ? 0x200 : 0;
                    c.ebx = i;
                    c.esi = parts;
                    rc::st32(S - 0x18, 0x4ae77e);
                    c.esp = S - 0x18;
                    if (!sceneshadow::call(0x40c910, 5, retstate::Eax, app, c, &sub_40c910))
                    {
                        cpu.terminate = true;
                        return;
                    }
                }
                ecx = rc::ld32(ArenaNext);
            }
        }
        else
        {
            st(ArenaFailed, 1);
            st(ArenaFailedBytes, rc::ld32(ArenaFailedBytes) + 0x40);
        }
        edx = reg32(sreg32(phase + 1) % 2);
        phase = edx;
        z = float(double(z) + ld(MiniStep));
    }
    while (--count != 0);
    leave();
}

// ---- 0x4ae7d0: MiniWorld::RenderFlares ----

// [esp + 4] = the mini world, [esp + 8] = the view. ret 8. The light flares of the drawn rows, each row placed by a
// frame arena matrix; the loop ends at an arena failure.
void miniFlares(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, world = rc::ld32(E + 4), view = rc::ld32(E + 8);
    reg32 eax = cpu.eax, ecx = cpu.ecx, edx = cpu.edx;
    const auto leave = [&]()
    {
        cpu.eax = eax;
        cpu.ecx = ecx;
        cpu.edx = edx;
        cpu.ebx = entry.ebx;
        cpu.esi = entry.esi;
        cpu.edi = entry.edi;
        cpu.ebp = entry.ebp;
        cpu.esp = E + 12;
    };
    if (!miniSetup(world, view, ecx, edx))
    {
        eax = 0;
        return leave();
    }
    reg32 phase = rc::ld32(world + 0x1328);
    float z = real(rc::ld32(world + 0x1334));
    reg32 count = rc::ld32(world + 0x132c);
    const reg32 y = rc::ld32(MiniY), height = rc::ld32(MiniZ), kind = rc::ld32(world + 0x1338);
    eax = count;
    ecx = kind;
    edx = height;
    if (count == 0)
        return leave();
    // ebp = E - 4, the six arguments of 0x5447c0 below the saved esi, ebx and edi.
    const reg32 B = E - 0x28;
    x86::CPU c = cpu;
    c.ebp = E - 4;
    for (;;)
    {
        edx = phase * 0x2e0;
        ecx = rc::ld32(ArenaEnd);
        const reg32 rows = edx + world + 0xd50;
        const reg32 placed = rc::ld32(ArenaNext);
        eax = placed + 0x40;
        if (eax >= ecx)
        {
            eax = rc::ld32(ArenaFailedBytes) + 0x40;
            st(ArenaFailed, 1);
            st(ArenaFailedBytes, eax);
            break;
        }
        st(ArenaNext, eax);
        if (placed == 0)
            break;
        float m[16];
        copyMatrix(SharedWorld, m);
        storeMatrix(placed, m);
        // 0x401cd0 leaves the matrix's last word in ecx and edx.
        edx = rc::ld32(SharedWorld + 0x3c);
        stf(placed + 0x30, negate(z));
        eax = y;
        ecx = height;
        st(placed + 0x34, y);
        st(placed + 0x38, height);
        const reg32 head = rows + 0x2d0;
        for (reg32 flare = rc::ld32(head); flare != 0 && flare != head; flare = rc::ld32(flare))
        {
            edx = kind;
            rc::st32(B - 4, 0);
            rc::st32(B - 8, kind);
            rc::st32(B - 0xc, 0);
            rc::st32(B - 0x10, 0x3f800000u);
            rc::st32(B - 0x14, flare);
            rc::st32(B - 0x18, view);
            rc::st32(B - 0x1c, 0x4ae8ad);
            c.eax = placed;
            c.ecx = ecx;
            c.edx = edx;
            c.ebx = head;
            c.esi = placed;
            c.edi = flare;
            c.ebp = E - 4;
            c.esp = B - 0x1c;
            if (!sceneshadow::call(0x5447c0, 6, retstate::Eax, app, c, &sub_5447c0))
            {
                cpu.terminate = true;
                return;
            }
            eax = c.eax;
            ecx = c.ecx;
            edx = c.edx;
        }
        ecx = (phase + 1) & 0x80000001u;
        if (sreg32(ecx) < 0)
            ecx = ((ecx - 1) | 0xfffffffeu) + 1;
        z = float(double(z) + ld(MiniStep));
        eax = count - 1;
        phase = ecx;
        count = eax;
        if (count == 0)
            break;
    }
    leave();
}

// ---- 0x570c70: FE flares ----

// edi = the view, [esp + 4] = the flare scale. cdecl. The car flares of the FE cars of the view.
void feFlares(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, view = cpu.edi, scale = rc::ld32(E + 4);
    reg32 ecx = cpu.ecx, edx = cpu.edx;
    if (view != 0 && rc::ld32(FlaresOn) != 0 && rc::ld32(CarFlaresOn) != 0 && rc::ld32(MiniWorldOn) != 0)
    {
        x86::CPU c = cpu;
        c.ebx = scale;
        c.ebp = FeCars;
        for (reg32 car = rc::ld32(FeCars); car != FeCars; car = rc::ld32(car))
        {
            if (rc::ld32(car + 0x994) == 0 || rc::ld32(car + 0x99c) != 0 || rc::ld32(car + 0x730) != rc::ld32(view + 4))
                continue;
            ecx = rc::ld32(car + 8);
            if (ecx == 0)
                continue;
            edx = car + 0x750;
            rc::st32(E - 0x20, ecx);
            rc::st32(E - 0x1c, view);
            rc::st32(E - 0x18, edx);
            rc::st32(E - 0x14, 0);
            rc::st32(E - 0x10, scale);
            rc::st32(E - 0x24, 0x570ced);
            c.eax = car + 0x740;
            c.ecx = ecx;
            c.edx = edx;
            c.esi = car;
            c.edi = view;
            c.esp = E - 0x24;
            if (!sceneshadow::call(0x56d8d0, 5, retstate::Eax, app, c, &sub_56d8d0))
            {
                cpu.terminate = true;
                return;
            }
            ecx = c.ecx;
            edx = c.edx;
        }
    }
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// ---- dispatch, shadow compare and timing ----

enum Id
{
    IdModels, IdModel, IdClans, IdQuad, IdMini, IdFeFlares, IdMiniFlares, IdCount
};

struct Port
{
    const char* name;
    reg32 address;
    Function guest;
    Function native;
};

void clansNative(App* app, x86::CPU& cpu) { renderClans(app, cpu); }

const Port Ports[IdCount] = {
    { "world models 0x592100", 0x592100, &sub_592100_guest, &renderModels },
    { "world model 0x591e40", 0x591e40, &sub_591e40_guest, &renderModel },
    { "clans 0x58a240", 0x58a240, &sub_58a240_guest, &clansNative },
    { "quad 0x40c300", 0x40c300, &sub_40c300_guest, &quad },
    { "mini world 0x4ae650", 0x4ae650, &sub_4ae650_guest, &renderMini },
    { "FE flares 0x570c70", 0x570c70, &sub_570c70_guest, &feFlares },
    { "mini world flares 0x4ae7d0", 0x4ae7d0, &sub_4ae7d0_guest, &miniFlares },
};

sceneshadow::Stats s_shadow;
uint32_t s_calls[IdCount] = {};
std::vector<Span> s_spans;

void quadSpans(std::vector<Span>& spans)
{
    spans.push_back(Span{LastModel, 4});
    spans.push_back(Span{AlphaRef, 20});
    spans.push_back(Span{WorldCalls, 16});
    spans.push_back(Span{LastView, 4});
    spans.push_back(Span{LastGroup, 8});
    spans.push_back(Span{StreamCache, 4});
}

void modelSpans(reg32 wm, std::vector<Span>& spans)
{
    for (const reg32 model : { rc::ld32(wm + 8), rc::ld32(wm + 0xc) })
    {
        if (model != 0 && rc::ld32(model + 0xc) != 0)
            spans.push_back(Span{rc::ld32(model + 0xc) + 0x20, 0x1c});
    }
    reg32 node = rc::ld32(wm + 0x60);
    for (uint32_t n = 0; node != 0 && n < 64; ++n, node = rc::ld32(node + 0xe0))
        spans.push_back(Span{node + 0x10, 0xd8});
}

void listSpans(reg32 node, std::vector<Span>& spans)
{
    spans.push_back(Span{node, 8});
    spans.push_back(Span{rc::ld32(node), 8});
    spans.push_back(Span{rc::ld32(node + 4), 8});
}

// Guest memory each port may store to, besides its stack frame.
void spans(Id id, const x86::CPU& cpu, std::vector<Span>& out)
{
    out.clear();
    const reg32 E = cpu.esp;
    switch (id)
    {
    case IdModels:
        sceneshadow::arenaSpans(out);
        for (reg32 node = rc::ld32(WorldModels); node != WorldModels; node = rc::ld32(node))
            modelSpans(node, out);
        break;
    case IdModel:
        sceneshadow::arenaSpans(out);
        modelSpans(rc::ld32(E + 4), out);
        break;
    case IdClans:
        quadSpans(out);
        listSpans(DrawnSkids, out);
        out.push_back(Span{Clans, 8});
        for (reg32 clan = rc::ld32(Clans); clan != Clans; clan = rc::ld32(clan))
        {
            out.push_back(Span{clan, 0x18});
            const reg32 head = clan + 8;
            for (reg32 link = rc::ld32(head); link != head; link = rc::ld32(link))
                listSpans(rc::ld32(link + 8), out);
        }
        break;
    case IdQuad:
        quadSpans(out);
        break;
    case IdMini:
    case IdMiniFlares:
        sceneshadow::arenaSpans(out);
        out.push_back(Span{rc::ld32(E + 4) + 0x1328, 0x14});
        break;
    default:
        break;
    }
}

void run(Id id, App* app, x86::CPU& cpu)
{
    const Port& p = Ports[id];
    // Inside a compare, this port's or another port's (its capture is active), the guest body runs: captures do not
    // nest.
    if (!settings().native || sceneshadow::comparing() || bridge::capturing())
    {
        p.guest(app, cpu);
        return;
    }
    ++s_calls[id];
    if (__builtin_expect(settings().shadow, 0))
    {
        spans(id, cpu, s_spans);
        sceneshadow::compare("shadow_world_models", p.name, p.address, Saved | retstate::Ecx | retstate::Edx, app,
                             cpu, p.guest, p.native, s_spans, s_shadow);
        return;
    }
    p.native(app, cpu);
}

}

void logUsage(uint32_t frames)
{
    if (frames == 0 || !settings().native)
        return;
    pf::logInfo("world_models: per frame: world models %u, world model %u, clans %u, quads %u, mini world %u, FE "
                "flares %u", s_calls[IdModels] / frames, s_calls[IdModel] / frames, s_calls[IdClans] / frames,
                s_calls[IdQuad] / frames, s_calls[IdMini] / frames, s_calls[IdFeFlares] / frames);
    memset(s_calls, 0, sizeof(s_calls));
}

void logShadowTotal()
{
    if (settings().native && settings().shadow)
        pf::logInfo("shadow_world_models: %u calls compared, %u differ", unsigned(s_shadow.calls),
                    unsigned(s_shadow.differing));
}

}

namespace worldmodels
{

void worldModels(win32::WinApplication* app, x86::CPU& cpu)
{
    run(IdModels, app, cpu);
}

uint32_t updateNodes(x86::reg32 view, std::vector<x86::reg32>* dry)
{
    return updateDrawnNodes(view, dry);
}

bool clanInView(x86::reg32 clan, x86::reg32 view)
{
    reg32 edx = 0;
    return sreg32(boxPixels(clan + 0x40, clan + 0x30, view, edx)) > 10;
}

void listViewUpdates(std::vector<x86::reg32>* nodes)
{
    s_updatedNodes = nodes;
}

uint32_t takeViewUpdates()
{
    const uint32_t n = s_viewUpdates;
    s_viewUpdates = 0;
    return n;
}

void worldModel(win32::WinApplication* app, x86::CPU& cpu)
{
    run(IdModel, app, cpu);
}

void clans(win32::WinApplication* app, x86::CPU& cpu)
{
    run(IdClans, app, cpu);
}

void miniWorld(win32::WinApplication* app, x86::CPU& cpu)
{
    run(IdMini, app, cpu);
}

}

namespace native
{

void sub_40c300(win32::WinApplication* app, x86::CPU& cpu)
{
    worldmodels::run(worldmodels::IdQuad, app, cpu);
}

void sub_570c70(win32::WinApplication* app, x86::CPU& cpu)
{
    // Recorded in the guest run of another shadow compare: the flare screen pass 0x409b70 calls it.
    if (sceneshadow::record(0x570c70, 1, retstate::Edi, app, cpu, &sub_570c70))
        return;
    worldmodels::run(worldmodels::IdFeFlares, app, cpu);
}

void sub_4ae7d0(win32::WinApplication* app, x86::CPU& cpu)
{
    // Recorded in the guest run of another shadow compare: the flare screen pass 0x409b70 calls it.
    if (sceneshadow::record(0x4ae7d0, 2, 0, app, cpu, &sub_4ae7d0))
        return;
    worldmodels::run(worldmodels::IdMiniFlares, app, cpu);
}

// The car flare of a car: recorded in the guest run of a shadow compare (sceneshadow.h).
void sub_56d8d0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (sceneshadow::record(0x56d8d0, 5, retstate::Eax, app, cpu, &sub_56d8d0))
        return;
    sub_56d8d0_guest(app, cpu);
}

}
}
