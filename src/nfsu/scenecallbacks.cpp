// Native guest callbacks of the native light flares and particles (scenecallbacks.h), bit-exact with the translated
// guest code: the same guest stores, registers and x87 state.

#include "scenecallbacks.h"
#include "cull.h"
#include "mathfn.h"
#include "retstate.h"
#include "sceneshadow.h"

#include <platform/config.h>
#include <platform/log.h>

#include <cmath>
#include <cstring>
#include <recomp.h>
#include <vector>
#include <winapi/d3d9_native.h>
#include <winapi/d3dx9.h>

namespace nfsu
{

// Generated guest functions.
void sub_403a00(win32::WinApplication* app, x86::CPU& cpu);
void sub_401f30(win32::WinApplication* app, x86::CPU& cpu);
void sub_401e10(win32::WinApplication* app, x86::CPU& cpu);
void sub_446fd0(win32::WinApplication* app, x86::CPU& cpu);
void sub_4406d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_440710(win32::WinApplication* app, x86::CPU& cpu);
void sub_402d30(win32::WinApplication* app, x86::CPU& cpu);
void sub_567370(win32::WinApplication* app, x86::CPU& cpu);

namespace callbacks
{

namespace
{

using x86::reg8;
using x86::reg32;
using x86::sreg32;
using draw::Span;
typedef win32::WinApplication App;
typedef void (*Function)(App*, x86::CPU&);

const reg32 ParticlePool = 0x73652c;
const reg32 RandomScale = 0x6cca54;
const reg32 ViewCameras[3] = { 0x740500, 0x740560, 0x7404a0 };   // eView camera slots: views 2, 3, 1

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("scene_callbacks_native", true),
                             pf::Config::getBool("shadow_scene_callbacks", false) };
        pf::logInfo("scene_callbacks: flare and particle callbacks %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

inline float pass(reg32 a) { return float(double(rc::ldf32(a))); }
inline reg32 bits(float v) { reg32 r; memcpy(&r, &v, 4); return r; }
inline float real(reg32 v) { float r; memcpy(&r, &v, 4); return r; }

// ---- 0x403a00: the cameras of views 2, 3 and 1 into a particle system (eax) at +0x10, +0x20, +0x30 ----

void cameraPositionsNative(App*, x86::CPU& cpu)
{
    const reg32 system = cpu.eax;
    for (int i = 0; i < 3; ++i)
    {
        const reg32 camera = rc::ld32(ViewCameras[i]);
        cpu.ecx = camera;
        if (camera == 0)
            continue;
        const reg32 at = system + 0x10 * reg32(i + 1);
        const float y = pass(camera + 0x44), z = pass(camera + 0x48);
        const reg32 x = rc::ld32(camera + 0x40);
        if (i == 1)
            cpu.edx = x;
        else
            cpu.ecx = x;
        sceneshadow::store(at, x);
        rc::stf32(at + 4, y);
        rc::stf32(at + 8, z);
        if (draw::g_logStores)
        {
            draw::g_stores.push_back(at + 4);
            draw::g_stores.push_back(at + 8);
        }
    }
}

// The natives store into cpu what the translated guest code stores at its returns (its direct callers keep the
// rest, esp included, in their locals).

// ---- 0x401f30: edx = a random state, [esp + 4] = a float range; st0 = a random value in [0, range) ----

void randomNative(App*, x86::CPU& cpu)
{
    reg32 x = rc::ld32(cpu.edx);
    x ^= x << 17;
    x ^= x >> 13;
    x ^= x << 5;
    sceneshadow::store(cpu.edx, x);
    const reg32 n = x & 0x7fffffff;
    const double value = double(sreg32(n)) * double(rc::ldf32(RandomScale)) * double(rc::ldf32(cpu.esp + 4));
    cpu.fpu.push(x86::Float(value));
    cpu.ecx = n;
}

// ---- 0x401e10: eax = a particle group: its counters cleared, its particles back to the particle pool ----

void releaseGroupNative(App*, x86::CPU& cpu)
{
    const reg32 group = cpu.eax, head = group + 0x14;
    sceneshadow::store(group + 0xc, 0);
    sceneshadow::store(group + 8, 0);
    reg32 ecx = 0, edx = cpu.edx;
    reg32 p = rc::ld32(head);
    if (p != head)
    {
        ecx = rc::ld32(ParticlePool);
        for (; p != head; p = rc::ld32(head))
        {
            const reg32 next = rc::ld32(p), previous = rc::ld32(p + 4);
            sceneshadow::store(previous, next);
            sceneshadow::store(next + 4, previous);
            edx = rc::ld32(ecx + 0x18) - 1;
            sceneshadow::store(p, rc::ld32(ecx + 0x10));
            sceneshadow::store(ecx + 0x18, edx);
            sceneshadow::store(ecx + 0x10, p);
        }
    }
    (void)ecx;
    (void)edx;
}

// ---- 0x446fd0: eax = the region list, [esp + 4] = a position; the region holding it, moved to the front ----

void regionNative(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    if (!cull::findDrivableSection(cpu))
    {
        sub_446fd0(app, cpu);
        return;
    }
    // The translated guest code leaves edx and esp as they were, and ecx when no region holds the position.
    cpu.edx = entry.edx;
    cpu.esp = entry.esp;
    if (cpu.eax == 0)
        cpu.ecx = entry.ecx;
}

// ---- 0x4406d0, 0x440710: a topology coordinate (eax) set up empty; its cached polygons released ----

void coordinateInitNative(App*, x86::CPU& cpu)
{
    const reg32 c = cpu.eax;
    for (const reg32 o : { 0x2a0u, 0x2a4u, 0x2a8u, 0x2acu, 0x2b4u, 0x2b8u, 0x2bcu })
        rc::st32(c + o, 0);
    for (const reg32 o : { 0u, 4u, 0x10u, 0x14u })
        rc::st32(c + o, 0x7f7fffff);
    for (const reg32 o : { 8u, 0xcu, 0x18u, 0x1cu })
        rc::st32(c + o, 0xff7fffff);
    for (const reg32 o : { 0x2a0u, 0x2a4u, 0x2a8u, 0x2acu, 0x2b0u })
        rc::st32(c + o, 0);
    cpu.ecx = 0;
    cpu.edx = 0x7f7fffff;
}

void coordinateReleaseNative(App*, x86::CPU& cpu)
{
    const reg32 c = cpu.eax;
    const sreg32 count = sreg32(rc::ld32(c + 0x2a0));
    cpu.edx = reg32(count);
    if (count > 0)
    {
        reg32 entry = c + 0x20;
        for (sreg32 i = 0; i < count; ++i, entry += 4)
        {
            cpu.eax = rc::ld32(entry);
            rc::st8(cpu.eax + 8, reg8(rc::ld8(cpu.eax + 8) - 1));
        }
        cpu.ecx = entry;
        cpu.edx = 0;
    }
    cpu.esp += 4;
}

// ---- 0x402d30: an emitter's spawns ----

const reg32 GroupPool = 0x736530;
const reg32 GlobalRandom = 0x704304, SpawnToggle = 0x78eb98, SpawnEvery = 0x700eb0;
const reg32 Zero = 0x6cc7a4, One = 0x6cc7bc, Half = 0x6cc7dc;
const reg32 SeedScale = 0x6cc84c, AngleScale = 0x6cc934, AngleBias = 0x6ccb3c, Degrees = 0x6cc840;
const reg32 SizeScale = 0x6ccd0c, SizeScale2 = 0x6cc920, LifeMax = 0x6cc8b0, TickRate = 0x6cc8e0;
const reg32 SpeedScale = 0x6cc7c4, SpeedMax = 0x6ccd08, ByteScale = 0x6cc974, DirectionScale = 0x6ccb34;
const reg32 RateMax = 0x6cc91c, RateNone = 0x6cc7b8;

inline double ldd(reg32 a) { return double(rc::ldf32(a)); }
inline reg32 xorshift(reg32 x)
{
    x ^= x << 17;
    x ^= x >> 13;
    x ^= x << 5;
    return x;
}
// fild of the state's low 31 bits, times the random scale.
inline double unit(reg32 x) { return double(sreg32(x & 0x7fffffff)) * ldd(RandomScale); }
inline reg32 ftol(double v) { return reg32(x86::truncInteger64(v)); }
// The angle in degrees as a 16-bit angle: (a * 65536) / 360 by the guest's multiply and shifts.
inline reg32 angle16(reg32 a)
{
    const x86::sreg64 p = x86::sreg64(sreg32(0xb60b60b7u)) * x86::sreg64(sreg32(a));
    sreg32 t = sreg32(reg32(x86::reg64(p) >> 32) + a);
    t >>= 8;
    return reg32(t) + (reg32(t) >> 31);
}

inline void store8(reg32 a, reg8 v)
{
    rc::st8(a, v);
    if (__builtin_expect(draw::g_logStores, 0))
        draw::g_stores.push_back(a & ~3u);
}
inline void store16(reg32 a, x86::reg16 v)
{
    rc::st16(a, v);
    if (__builtin_expect(draw::g_logStores, 0))
        draw::g_stores.push_back(a & ~3u);
}

// The rotation matrices of 0x40b930 (z), 0x40b990 (x) and 0x40ba40 (y) from a 16-bit angle.
void rotation(int axis, reg32 angle, float r[16])
{
    const float c = float(mathfn::sin16(angle + 0x4000));
    const double s = mathfn::sin16(angle);
    const float sf = float(s), ns = float(-s);
    for (int i = 0; i < 16; ++i)
        r[i] = 0.0f;
    r[15] = 1.0f;
    if (axis == 2)
    {
        r[0] = c, r[1] = sf, r[4] = ns, r[5] = c, r[10] = 1.0f;
    }
    else if (axis == 0)
    {
        r[0] = 1.0f, r[5] = c, r[6] = sf, r[9] = ns, r[10] = c;
    }
    else
    {
        r[0] = c, r[2] = ns, r[5] = 1.0f, r[8] = sf, r[10] = c;
    }
}

void multiply(reg32 out, reg32 a, const float* b)
{
    float ma[16], r[16];
    memcpy(ma, rc::host(a), 64);
    win32::d3d9::bridge::matrixMultiply(ma, b, r);
    memcpy(rc::host(out), r, 64);
}

void transform4(reg32 out, reg32 v, reg32 m)
{
    float in[4], mm[16], r[4];
    memcpy(in, rc::host(v), 16);
    memcpy(mm, rc::host(m), 64);
    win32::d3dx9::transform(in, mm, r);
    memcpy(rc::host(out), r, 16);
}

// The 0x401cd0 matrix copy between guest addresses.
void copyMatrix(reg32 from, reg32 to)
{
    for (reg32 row = 0; row < 4; ++row)
    {
        const reg32 s = from + 16 * row, d = to + 16 * row;
        const reg32 a = rc::ld32(s), b = rc::ld32(s + 12);
        rc::stf32(d + 4, pass(s + 4));
        rc::st32(d, a);
        rc::stf32(d + 8, pass(s + 8));
        rc::st32(d + 12, b);
    }
}

// [esp + 4] = the system, [esp + 8] = the emitter, [esp + 0xc] = dt (float), [esp + 0x10] = the group flag. ret 0x10.
// The particles the emitter spawns in dt, the stack locals at the guest's addresses.
void spawnNative(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp;
    const reg32 system = rc::ld32(E + 4), emitter = rc::ld32(E + 8), dtBits = rc::ld32(E + 0xc),
                flag = rc::ld32(E + 0x10);
    const reg32 S = (((E - 4) & ~15u) - 0x234) - 0xc;
    const auto at = [S](reg32 o) { return S + o; };
    const auto f = [S](reg32 o) { return double(rc::ldf32(S + o)); };
    const auto put = [S](reg32 o, double v) { rc::stf32(S + o, float(v)); };
    const auto leave = [&]()
    {
        cpu.ebx = entry.ebx;
        cpu.esi = entry.esi;
        cpu.edi = entry.edi;
        cpu.ebp = entry.ebp;
        cpu.esp = E + 0x14;
    };
    const reg32 P = rc::ld32(emitter + 0x1c);
    if (P == 0)
        return leave();
    // In range of one of the three cameras.
    const double range = ldd(P + 0x80);
    bool near = false;
    for (reg32 i = 0; i < 3 && !near; ++i)
    {
        const reg32 cam = system + 0x10 + 0x10 * i;
        const double dx = ldd(emitter + 0x60) - ldd(cam), dy = ldd(emitter + 0x64) - ldd(cam + 4),
                     dz = ldd(emitter + 0x68) - ldd(cam + 8);
        near = std::sqrt((dz * dz + dy * dy) + dx * dx) < range;
    }
    if (rc::ld32(emitter + 0x24) == 0)
    {
        const reg32 x = xorshift(rc::ld32(GlobalRandom));
        sceneshadow::store(GlobalRandom, x);
        sceneshadow::store(emitter + 0x24, ftol(unit(x) * ldd(SeedScale)));
    }
    if (!near)
        return leave();
    const double dt = double(real(dtBits)), zero = ldd(Zero);
    if (rc::ld32(emitter + 0x10) != 1 && !(dt < zero))
        return leave();
    // The time to spend: -dt, dt, or the emitter's timer when it runs out first.
    rc::st32(at(0x18), rc::ld32(emitter + 0x14));
    if (dt < zero)
    {
        put(0x1c, -dt);
    }
    else
    {
        rc::st32(at(0x1c), dtBits);
        const double timer = f(0x18);
        if (!(timer == zero) && dt > timer)
            rc::st32(at(0x1c), rc::ld32(at(0x18)));
    }
    double acc = ldd(emitter + 0x18);
    put(0x28, acc);
    bool spawning = f(0x1c) > acc;
    if (spawning)
    {
        // The emitter's particle group, or a new one at the end of the system's list.
        reg32 group = rc::ld32(emitter + 0x20);
        const reg32 pool = rc::ld32(GroupPool);
        rc::st32(at(0x18), group);
        if (!(group != 0 && rc::ld32(group + 0xc) == emitter && rc::ld32(group + 0x10) == flag)
            && rc::ld32(pool + 0x18) != rc::ld32(pool + 0x2c))
        {
            const reg32 count = rc::ld32(pool + 0x18) + 1;
            reg32 g = rc::ld32(pool + 0x10);
            if (g != 0)
            {
                if (sreg32(count) > sreg32(rc::ld32(pool + 0x1c)))
                    sceneshadow::store(pool + 0x1c, count);
                sceneshadow::store(pool + 0x18, count);
                sceneshadow::store(pool + 0x10, rc::ld32(g));
                sceneshadow::store(g + 0x10, flag);
                sceneshadow::store(g + 0xc, emitter);
                sceneshadow::store(g + 0x14, g + 0x14);
                sceneshadow::store(g + 0x18, g + 0x14);
                sceneshadow::store(g + 8, rc::ld32(emitter + 0x1c));
                sceneshadow::store(emitter + 0x20, g);
            }
            const reg32 tail = rc::ld32(system + 4);
            sceneshadow::store(tail, g);
            sceneshadow::store(system + 4, g);
            rc::st32(at(0x18), g);
            sceneshadow::store(g + 4, tail);
            sceneshadow::store(g, system);
            group = g;
        }
        spawning = group != 0;
    }
    if (spawning)
    {
        rc::st32(at(0xc0), rc::ld32(P + 0x68));
        rc::st32(at(0xcc), rc::ld32(P + 0x64));
        rc::st32(at(0x84), rc::ld32(P + 0x78));
        rc::st32(at(0xbc), rc::ld32(P + 0x60));
        rc::st32(at(0x7c), rc::ld32(P + 0x74));
        rc::st32(at(0x6c), rc::ld32(P + 0x70));
        copyMatrix(emitter + 0x30, at(0x180));
        copyMatrix(at(0x180), at(0x100));
        rc::st32(at(0x130), 0);
        rc::st32(at(0x134), 0);
        rc::st32(at(0x138), 0);
        rc::st32(at(0x13c), 0x3f800000);
        rc::st32(at(0x78), 0x3f800000);
        if (rc::ld8(P + 0x1c) != 0)
        {
            const reg32 frames = (1u << (rc::ld8(P + 0x1a) & 31)) * (1u << (rc::ld8(P + 0x19) & 31));
            rc::st32(at(0x30), frames);
            put(0x78, double(sreg32(frames)));
        }
        const double size = ldd(P + 0x48) * ldd(SizeScale);
        rc::st32(at(0x38), rc::ld32(P + 0x4c));
        put(0x74, size);
        rc::st32(at(0x44), rc::ld32(P + 0x38));
        rc::st32(at(0x34), rc::ld32(P + 0x50));
        put(0xe8, size * ldd(SizeScale2));
        rc::st32(at(0xe4), reg32(sreg32(int8_t(rc::ld8(P + 0x1d)))));
        rc::st32(at(0xd0), reg32(sreg32(int8_t(rc::ld8(P + 0x18)))));
        const double a30 = ldd(P + 0x30), a40 = ldd(P + 0x40);
        put(0x4c, f(0x38) * ldd(Half));
        put(0x30, ldd(P + 0x34) * a30);
        put(0x88, ldd(P + 0x44) * a40);
        put(0x70, ldd(P + 0x3c) * f(0x44));
        put(0x80, ldd(P + 0x54) * f(0x34));
        put(0x8c, a30 - f(0x30));
        put(0xc8, a40 - f(0x88));
        put(0x44, f(0x44) - f(0x70));
        put(0x34, f(0x34) - f(0x80));
        acc = f(0x28);
        reg32 state = rc::ld32(emitter + 0x24);
        if (f(0x1c) >= acc)
        {
            put(0xb8, f(0x6c) * ldd(Half));
            put(0xc4, f(0x7c) * ldd(Half));
            put(0xd4, f(0x84) * ldd(Half));
            const reg32 group = rc::ld32(at(0x18));
            for (;;)
            {
                put(0x1c, f(0x1c) - acc);
                reg32 ebx;
                if (rc::ld32(at(0xd0)) != 0)
                {
                    // A direction in the plane, turned by a random angle, through the emitter's rotation.
                    const reg32 s1 = xorshift(state), s2 = xorshift(s1);
                    ebx = s2;
                    const reg32 a1 = angle16(ftol((unit(s2) * ldd(AngleScale) - ldd(AngleBias)) * ldd(Degrees)));
                    rc::stf32(at(0x90), float(mathfn::sin16(a1 + 0x4000)));
                    rc::stf32(at(0x94), float(mathfn::sin16(a1)));
                    rc::st32(at(0x98), 0);
                    const reg32 a2 = angle16(ftol((unit(s1) * f(0x38) - f(0x4c)) * ldd(Degrees)));
                    float r[16];
                    rotation(1, a2, r);
                    multiply(at(0x1c0), 0x78bcc0, r);
                    transform4(at(0x90), at(0x90), at(0x1c0));
                    transform4(at(0x90), at(0x90), at(0x100));
                    rc::st32(at(0x3c), rc::ld32(at(0x90)));
                    rc::st32(at(0x40), rc::ld32(at(0x94)));
                    rc::st32(at(0x48), rc::ld32(at(0x98)));
                }
                else
                {
                    // The emitter's z axis turned by random angles about x, y and z.
                    const reg32 s1 = xorshift(state), s2 = xorshift(s1), s3 = xorshift(s2);
                    ebx = s3;
                    float r[16];
                    rotation(0, angle16(ftol((unit(s1) * f(0x38) - f(0x4c)) * ldd(Degrees))), r);
                    multiply(at(0x140), 0x78bcc0, r);
                    rotation(1, angle16(ftol((unit(s2) * f(0x38) - f(0x4c)) * ldd(Degrees))), r);
                    multiply(at(0x140), at(0x140), r);
                    rotation(2, angle16(ftol((unit(s3) * f(0x38) - f(0x4c)) * ldd(Degrees))), r);
                    memcpy(rc::host(at(0x200)), r, 64);
                    multiply(at(0x140), at(0x140), r);
                    transform4(at(0xf0), at(0x1a0), at(0x140));
                    rc::st32(at(0x3c), rc::ld32(at(0xf0)));
                    rc::st32(at(0x40), rc::ld32(at(0xf4)));
                    rc::st32(at(0x48), rc::ld32(at(0xf8)));
                }
                // Position in the emitter's box, frame, life, size and speed.
                const reg32 r1 = xorshift(ebx), r2 = xorshift(r1), r3 = xorshift(r2);
                put(0xa0, (unit(r1) * f(0x6c) - f(0xb8)) + f(0xbc));
                put(0xa4, (unit(r2) * f(0x7c) - f(0xc4)) + f(0xcc));
                rc::st32(at(0xac), 0x3f800000);
                put(0xa8, (unit(r3) * f(0x84) - f(0xd4)) + f(0xc0));
                transform4(at(0xa0), at(0xa0), at(0x180));
                rc::st32(at(0xd8), rc::ld32(at(0xa0)));
                rc::st32(at(0xec), rc::ld32(at(0xa4)));
                rc::st32(at(0xe0), rc::ld32(at(0xa8)));
                const reg32 r4 = xorshift(r3);
                rc::st8(at(0x27), reg8(ftol(unit(r4) * f(0x78))));
                const reg32 r5 = xorshift(r4), r6 = xorshift(r5), r7 = xorshift(r6);
                put(0x28, unit(r5) * f(0x70) + f(0x44));
                put(0x14, unit(r6) * f(0x74) + f(0xe8));
                ebx = r7;
                rc::st32(at(0x20), 0);
                put(0x2c, unit(r7) * f(0x88) + f(0xc8));
                if (!(f(0x34) == zero))
                {
                    const reg32 r8 = xorshift(r7);
                    ebx = r8;
                    double w = unit(r8) * f(0x80) + f(0x34);
                    put(0x20, w);
                    if (!(w > zero))
                        w = zero;
                    else if (ldd(One) < f(0x20))
                        w = ldd(One);
                    else
                        w = f(0x20);
                    put(0x20, w);
                    if (rc::ld8(P + 0x1e) == 0)
                    {
                        // The direction blended with the emitter's velocity; its length is the speed.
                        put(0x50, w * ldd(emitter + 0x70) + f(0x2c) * f(0x3c));
                        put(0x54, w * ldd(emitter + 0x74) + f(0x2c) * f(0x40));
                        const double z = w * ldd(emitter + 0x78) + f(0x2c) * f(0x48);
                        put(0x58, z);
                        put(0x2c, std::sqrt((z * f(0x58) + f(0x54) * f(0x54)) + f(0x50) * f(0x50)));
                        x86::CPU c = cpu;
                        c.ecx = at(0x50);
                        c.edx = at(0x50);
                        rc::st32(at(-4), 0x40374f);
                        c.esp = at(-4);
                        sub_567370(app, c);
                        if (c.terminate)
                        {
                            cpu.terminate = true;
                            return;
                        }
                        rc::st32(at(0x3c), rc::ld32(at(0x50)));
                        rc::st32(at(0x40), rc::ld32(at(0x54)));
                        rc::st32(at(0x48), rc::ld32(at(0x58)));
                    }
                }
                reg32 info = ftol(f(0x14)) & 0xfffe;
                if (rc::ld32(at(0xe4)) != 0)
                    info |= ebx & 1;
                if (f(0x28) > ldd(LifeMax))
                    rc::st32(at(0x28), 0x42700000);
                if (rc::ld32(emitter + 0x28) == 0)
                {
                    if (sreg32(rc::ld32(SpawnEvery)) < 2)
                        sceneshadow::store(SpawnToggle, rc::ld32(SpawnToggle) ^ 1);
                    else
                        sceneshadow::store(SpawnToggle, 0);
                    const reg32 pp = rc::ld32(ParticlePool);
                    if (rc::ld32(pp + 0x18) != rc::ld32(pp + 0x2c) && rc::ld32(SpawnToggle) == 0)
                    {
                        const reg32 count = rc::ld32(pp + 0x18) + 1;
                        reg32 p = rc::ld32(pp + 0x10);
                        if (p != 0)
                        {
                            if (sreg32(count) > sreg32(rc::ld32(pp + 0x1c)))
                                sceneshadow::store(pp + 0x1c, count);
                            sceneshadow::store(pp + 0x18, count);
                            sceneshadow::store(pp + 0x10, rc::ld32(p));
                        }
                        store16(p + 8, x86::reg16(ftol(f(0x28) * ldd(TickRate))));
                        store16(p + 0xa, x86::reg16(info));
                        double speed = f(0x2c) * ldd(SpeedScale);
                        if (!(ldd(SpeedMax) > speed || ldd(SpeedMax) == speed
                              || std::isnan(speed) || std::isnan(ldd(SpeedMax))))
                            speed = ldd(SpeedMax);
                        store16(p + 0xc, x86::reg16(ftol(speed)));
                        store8(p + 0xe, reg8(ftol(f(0x20) * ldd(ByteScale))));
                        sceneshadow::store(p + 0x10, rc::ld32(at(0xd8)));
                        sceneshadow::store(p + 0x14, rc::ld32(at(0xec)));
                        sceneshadow::store(p + 0x18, rc::ld32(at(0xe0)));
                        store8(p + 0x1c, reg8(ftol(f(0x3c) * ldd(DirectionScale))));
                        store8(p + 0x1d, reg8(ftol(f(0x40) * ldd(DirectionScale))));
                        store8(p + 0x1e, reg8(ftol(f(0x48) * ldd(DirectionScale))));
                        store8(p + 0x1f, rc::ld8(at(0x27)));
                        const reg32 head = group + 0x14, tail = rc::ld32(group + 0x18);
                        sceneshadow::store(tail, p);
                        sceneshadow::store(head + 4, p);
                        sceneshadow::store(p + 4, tail);
                        sceneshadow::store(p, head);
                    }
                }
                // The time to the next spawn.
                state = xorshift(ebx);
                const double v = unit(state) * f(0x30) + f(0x8c);
                if (!(v > zero))
                {
                    acc = ldd(RateNone);
                }
                else
                {
                    acc = ldd(One) / v;
                    if (acc > ldd(RateMax))
                        acc = ldd(RateMax);
                    else if (acc < zero)
                        acc = zero;
                }
                if (!(f(0x1c) >= acc))
                    break;
            }
        }
        // The group goes back to its pool when it holds no particle.
        const reg32 group = rc::ld32(at(0x18));
        put(0x28, acc);
        sceneshadow::store(emitter + 0x24, state);
        if (rc::ld32(group + 0x14) == group + 0x14)
        {
            const reg32 pool = rc::ld32(GroupPool);
            const reg32 next = rc::ld32(group), previous = rc::ld32(group + 4);
            sceneshadow::store(previous, next);
            sceneshadow::store(next + 4, previous);
            sceneshadow::store(group + 0xc, 0);
            sceneshadow::store(group + 8, 0);
            acc = f(0x28);
            sceneshadow::store(group, rc::ld32(pool + 0x10));
            sceneshadow::store(pool + 0x18, rc::ld32(pool + 0x18) - 1);
            sceneshadow::store(pool + 0x10, group);
        }
    }
    if (dt < zero)
        sceneshadow::store(emitter + 0x18, 0);
    else
        sceneshadow::store(emitter + 0x18, bits(float(acc - f(0x1c))));
    leave();
}

// ---- dispatch and shadow compare ----

enum Id
{
    IdCameras, IdRandom, IdRelease, IdSpawn, IdRegion, IdCoordinateInit, IdCoordinateRelease, IdCount
};

struct Callback
{
    const char* name;
    reg32 address;
    Function guest;
    Function native;
    bool x87;
    uint8_t registers;      // the registers the translated guest code stores at its returns: compared
};

using namespace retstate;
const uint8_t Saved = Ebx | Esp | Ebp | Esi | Edi;

const Callback Callbacks[IdCount] = {
    { "particle cameras 0x403a00", 0x403a00, &sub_403a00, &cameraPositionsNative, false, Ecx | Edx },
    { "random 0x401f30", 0x401f30, &sub_401f30, &randomNative, true, Ecx },
    { "particle group release 0x401e10", 0x401e10, &sub_401e10, &releaseGroupNative, false, Edi },
    { "spawn 0x402d30", 0x402d30, &sub_402d30, &spawnNative, false, Saved },
    { "region 0x446fd0", 0x446fd0, &sub_446fd0, &regionNative, true, Eax | Ecx | Ebx | Esi | Edi },
    { "coordinate 0x4406d0", 0x4406d0, &sub_4406d0, &coordinateInitNative, false, Ecx | Edx },
    { "coordinate release 0x440710", 0x440710, &sub_440710, &coordinateReleaseNative, false, Eax | Ecx | Edx | Esp },
};

sceneshadow::Stats s_shadow;
std::vector<Span> s_spans;

void spans(Id id, const x86::CPU& cpu, std::vector<Span>& out)
{
    out.clear();
    switch (id)
    {
    case IdCameras:
        out.push_back(Span{cpu.eax + 0x10, 0x2c});
        break;
    case IdRandom:
        out.push_back(Span{cpu.edx, 4});
        break;
    case IdRelease:
    {
        const reg32 head = cpu.eax + 0x14, pool = rc::ld32(ParticlePool);
        out.push_back(Span{cpu.eax + 8, 0x14});
        out.push_back(Span{pool + 0x10, 4});
        out.push_back(Span{pool + 0x18, 4});
        reg32 p = rc::ld32(head);
        for (uint32_t n = 0; p != head && n < 100000; ++n, p = rc::ld32(p))
            out.push_back(Span{p, 8});
        break;
    }
    case IdSpawn:
    {
        const reg32 system = rc::ld32(cpu.esp + 4), emitter = rc::ld32(cpu.esp + 8);
        const reg32 groups = rc::ld32(GroupPool), particles = rc::ld32(ParticlePool);
        out.push_back(Span{emitter + 0x10, 0x1c});
        out.push_back(Span{GlobalRandom, 4});
        out.push_back(Span{SpawnToggle, 4});
        out.push_back(Span{groups + 0x10, 0x10});
        out.push_back(Span{particles + 0x10, 0x10});
        out.push_back(Span{system, 8});
        reg32 g = rc::ld32(system);
        for (uint32_t n = 0; g != system && n < 4096; ++n, g = rc::ld32(g))
        {
            out.push_back(Span{g, 0x1c});
            out.push_back(Span{rc::ld32(g + 0x18), 8});
        }
        if (rc::ld32(groups + 0x10) != 0)
            out.push_back(Span{rc::ld32(groups + 0x10), 0x1c});
        reg32 p = rc::ld32(particles + 0x10);
        for (uint32_t n = 0; p != 0 && n < 1024; ++n, p = rc::ld32(p))
            out.push_back(Span{p, 0x20});
        break;
    }
    case IdRegion:
    {
        const reg32 sentinel = cpu.eax + 0x10;
        out.push_back(Span{sentinel, 8});
        reg32 node = rc::ld32(sentinel);
        for (uint32_t n = 0; node != sentinel && n < 100000; ++n, node = rc::ld32(node))
            out.push_back(Span{node, 8});
        break;
    }
    case IdCoordinateInit:
        out.push_back(Span{cpu.eax, 0x2c0});
        break;
    case IdCoordinateRelease:
    {
        const sreg32 count = sreg32(rc::ld32(cpu.eax + 0x2a0));
        for (sreg32 i = 0; i < count && i < 0xa0; ++i)
            out.push_back(Span{rc::ld32(cpu.eax + 0x20 + 4 * reg32(i)) + 8, 1});
        break;
    }
    default:
        break;
    }
}

void run(Id id, App* app, x86::CPU& cpu)
{
    const Callback& c = Callbacks[id];
    // Inside another port's compare (its capture is active) the guest body runs: captures do not nest.
    if (!settings().native || (win32::d3d9::bridge::capturing() && !sceneshadow::comparing()))
    {
        c.guest(app, cpu);
        return;
    }
    if (__builtin_expect(settings().shadow, 0) && !sceneshadow::comparing())
    {
        spans(id, cpu, s_spans);
        sceneshadow::compare("shadow_scene_callbacks", c.name, c.address, c.registers, app, cpu, c.guest, c.native,
                             s_spans, s_shadow, c.x87);
        return;
    }
    c.native(app, cpu);
}

}

void cameraPositions(win32::WinApplication* app, x86::CPU& cpu) { run(IdCameras, app, cpu); }
void random(win32::WinApplication* app, x86::CPU& cpu) { run(IdRandom, app, cpu); }
void releaseGroup(win32::WinApplication* app, x86::CPU& cpu) { run(IdRelease, app, cpu); }
void spawn(win32::WinApplication* app, x86::CPU& cpu) { run(IdSpawn, app, cpu); }
void region(win32::WinApplication* app, x86::CPU& cpu) { run(IdRegion, app, cpu); }
void coordinateInit(win32::WinApplication* app, x86::CPU& cpu) { run(IdCoordinateInit, app, cpu); }
void coordinateRelease(win32::WinApplication* app, x86::CPU& cpu) { run(IdCoordinateRelease, app, cpu); }

void logShadowTotal()
{
    if (settings().native && settings().shadow)
        pf::logInfo("shadow_scene_callbacks: %u calls compared, %u differ", unsigned(s_shadow.calls),
                    unsigned(s_shadow.differing));
}

}
}
