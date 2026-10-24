// Native light flares (flares.h): guest 0x544f30, 0x544e90, 0x540c70, 0x5447c0 and 0x5443f0. Bit-exact with the
// translated guest code: every x87 chain in double in the guest's order, floats where the guest stores floats.
// One flare runs in three phases:
//   A  game thread, in the guest's order: the entry tests and the flare record's cached ground height
//      ([flare+0x1c]), through guest 0x4406d0, 0x440a40 and 0x440710;
//   B  any thread: visibility, distance fade, facing and the quad inputs, reading guest memory only;
//   C  game thread, in the guest's order: per flare type the intensity (pow 0x673890 natively on its plain paths,
//      the guest code otherwise), the quad and its submission through guest 0x40c300.
// The world pass runs phase A for all its packs and flares, phase B on the job workers, then phase C. The x87
// compares of phases A and B are applied to the CPU again in the guest's order. The overrides restore ebx, esi, edi
// and ebp and pop the return address; ecx, edx and the flags are not set.

#include "flares.h"
#include "cull.h"

#include <platform/config.h>
#include <platform/jobs.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <recomp.h>
#include <vector>
#include <winapi/d3dx9.h>

namespace nfsu
{

// Generated functions: the overridden guest bodies and the guest helpers the native code calls.
void sub_544f30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5447c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5443f0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_446fd0(win32::WinApplication* app, x86::CPU& cpu);
void sub_4406d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_440a40(win32::WinApplication* app, x86::CPU& cpu);
void sub_440710(win32::WinApplication* app, x86::CPU& cpu);
void sub_673890(win32::WinApplication* app, x86::CPU& cpu);
void sub_40c300(win32::WinApplication* app, x86::CPU& cpu);

namespace flares
{

namespace
{

using x86::reg8;
using x86::reg16;
using x86::reg32;
using x86::reg64;
using x86::sreg8;
using x86::sreg16;
using x86::sreg32;
using x86::sreg64;

// Guest data.
const reg32 FlaresOn = 0x6f41a0;
const reg32 GameState = 0x77a920;       // the world pass runs in state 6
const reg32 SkipState = 0x7361f0;       // pointer; the world pass is skipped when it points at 1
const reg32 ListMode = 0x735550;        // 0: flare packs by region, else the flare lists at FlareLists
const reg32 Regions = 0x77a930;
const reg32 PackGrid = 0x740b50;        // 0x30-byte packs, 0x1e0 bytes per row of ten
const reg32 PackRows = 0x735538;        // rows - 1
const reg32 PackColumns = 0x73553c;     // columns - 1
const reg32 FlareLists = 0x740a60;      // sentinel node; each node's flares at +0x38
const reg32 Identity = 0x78bcc0;
const reg32 Zero = 0x6cc7a4;            // float constants in .rdata
const reg32 One = 0x6cc7bc;
const reg32 GroundLimit = 0x6cc964;
const reg32 Ranges = 0x6b6350;          // near and far squared distance: mode 0 at +0, mode 2 at +8, mode 1 at +0x10
const reg32 FlareRadius = 0x6f3df4;     // box radius in mode 2, z scale of the quad in mode 2
const reg32 ModeOneRadius = 0x6f41a8;
const reg32 SizeScale = 0x6b633c;
const reg32 ColourMax = 0x6f41a4;
const reg32 FlareTypes = 0x6f4150;      // two flare types per kind, 8 bytes per kind
const reg32 Textures = 0x72fa14;
const reg32 Sse2 = 0x792fb8;            // pow: the SSE2 path is taken when set
const reg32 MathErrors = 0x717ad0;      // pow: no range check of the result when set
const reg32 Half64 = 0x6eb4e0;          // double 0.5
const reg32 Unset = 0x4479c000;         // [flare+0x1c] 1000.0f: no ground height yet
const reg32 NoGround = 0x461c3c00;      // 10000.0f
const reg32 Poison = 0x7fbadbad;
const int PolyWords = 0x94 / 4;
const int BatchItems = 16;              // flares per claim
const uint32_t ShadowLogLimit = 20;

inline double ld(reg32 address) { return double(rc::ldf32(address)); }
inline reg32 bits(float value) { reg32 r; memcpy(&r, &value, 4); return r; }
inline float real(reg32 value) { float r; memcpy(&r, &value, 4); return r; }
float real(float) = delete;
float real(double) = delete;

// The words of the quad record that 0x5443f0 writes.
inline bool polyWord(int w) { return w >= 20 || (w >= 4 && (w & 3) != 3); }

// The frame of 0x5447c0 entered with esp at the return address slot (its esp after the prologue).
inline reg32 lightFrame(reg32 esp) { return ((esp - 4) & ~15u) - 0x3e0; }
// The frame of 0x5443f0 entered with esp at the return address slot.
inline reg32 quadFrame(reg32 esp) { return ((esp - 4) & ~15u) - 0xe0; }

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
        const Settings r = { pf::Config::getBool("flares_native", true), pf::Config::getBool("flares_parallel", true),
                             pf::Config::getBool("shadow_flares", false) };
        pf::logInfo("flares: light flares %s%s%s", r.native ? "native" : "guest",
                    r.native && r.parallel ? ", parallel" : "", r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// The operands of the last x87 compare of a phase.
struct Compare
{
    bool set = false;
    double a = 0, b = 0;

    void operator()(double x, double y)
    {
        set = true;
        a = x;
        b = y;
    }
    void apply(x86::CPU& cpu) const
    {
        if (set)
            cpu.fpu.compare(a, b);
    }
};

// One call of 0x5447c0: its arguments, then the results of phases A and B.
struct Flare
{
    reg32 view, flare, matrix, scale, force, mode, height;
    reg32 esp;              // the return address slot of the call
    bool live;              // past the entry tests
    bool shown;             // visible: phase C draws its types
    Compare a, b;
    reg32 pos[3], toward[3], right[3], up[3];
    float facing, depth, fade;
    reg32 types[2];
};

// One call of 0x5443f0: its arguments and the quad it submitted.
struct Draw
{
    reg32 type, view, scale, facing, depth, fade, mode;
    reg32 pos[3], toward[3], right[3], up[3];
    bool drawn;
    reg32 texture;
    reg32 poly[PolyWords];
};

struct Write
{
    reg32 address, old;
};

// A native call: with record set the quads go to draws instead of 0x40c300; writes, when set, logs the guest
// dwords written outside the stack.
struct Run
{
    bool record = false;
    std::vector<Draw>* draws = nullptr;
    std::vector<Write>* writes = nullptr;
    uint32_t flares = 0, quads = 0;
};

inline void store(Run& run, reg32 address, reg32 value)
{
    if (run.writes)
        run.writes->push_back(Write{address, rc::ld32(address)});
    rc::st32(address, value);
}

// Calls guest code with esp at the return address slot; false when the guest is terminating.
inline bool call(void (*function)(win32::WinApplication*, x86::CPU&), win32::WinApplication* app, x86::CPU& cpu,
                 reg32 esp, reg32 ret)
{
    rc::st32(esp, ret);
    cpu.esp = esp;
    function(app, cpu);
    return !cpu.terminate;
}

inline void transform(const float in[4], reg32 matrix, float out[4])
{
    float m[16];
    memcpy(m, rc::host(matrix), 64);
    win32::d3dx9::transform(in, m, out);
}

// ---- phase A ----

// Guest 0x54484b-0x5448c8: the ground height under the flare, cached in [flare+0x1c]. c is a scratch CPU.
bool ground(win32::WinApplication* app, x86::CPU& c, Flare& f, Run& run)
{
    const reg32 s = lightFrame(f.esp);
    const reg32 coordinate = s + 0x120, point = s + 0x10;
    c.eax = coordinate;
    if (!call(sub_4406d0, app, c, s - 4, 0x544857))
        return false;
    rc::st32(point, rc::ld32(f.flare + 0x10));
    rc::stf32(point + 4, float(ld(f.flare + 0x14)));
    rc::st32(point + 8, rc::ld32(f.flare + 0x18));
    float in[4], out[4];
    memcpy(in, rc::host(point), 12);
    in[3] = 1.0f;
    transform(in, f.matrix, out);
    memcpy(rc::host(point), out, 16);
    rc::st32(s - 0x14, coordinate);
    rc::st32(s - 0x10, point);
    rc::st32(s - 0xc, 0);
    rc::st32(s - 8, 0);
    rc::st32(s - 4, 0);
    if (!call(sub_440a40, app, c, s - 0x18, 0x544894))
        return false;
    auto& x = c.fpu;
    store(run, f.flare + 0x1c, bits(float(x.st(0))));
    x.push(x.st(0));
    x.st(0) -= x86::Float(ld(f.flare + 0x18));
    x.st(0) = x.abs(x.st(0));
    const double d = x.st(0), limit = ld(GroundLimit);
    x.compare(d, limit);
    f.a(d, limit);
    x.pop();
    if (d < limit)
    {
        store(run, f.flare + 0x1c, NoGround);
    }
    else
    {
        x.push(x86::Float(ld(point + 8)));
        x.st(0) -= x86::Float(x.st(1));
        x.st(0) = x86::Float(x.st(1)) - x86::Float(x.st(0));
        store(run, f.flare + 0x1c, bits(float(x.st(0))));
        x.pop();
    }
    x.st(0) = x86::Float(x.st(0));
    x.pop();
    c.eax = coordinate;
    return call(sub_440710, app, c, s - 0x18, 0x5448cd);
}

// Guest 0x5447c0-0x5448cd: the entry tests and the flare record. c is a scratch CPU. False when the guest is
// terminating.
bool enter(win32::WinApplication* app, x86::CPU& c, Flare& f, Run& run)
{
    f.live = false;
    f.shown = false;
    f.a = Compare();
    f.b = Compare();
    if (rc::ld32(FlaresOn) == 0 || f.flare == 0 || f.matrix == 0 || rc::ld32(f.view + 0x40) == 0
        || rc::ld8(f.flare + 0xc) == 0)
        return true;
    f.live = true;
    const double height = double(real(f.height)), zero = ld(Zero);
    f.a(height, zero);
    if (!(height == zero))
    {
        store(run, f.flare + 0x1c, f.height);
        return true;
    }
    if (rc::ld32(f.flare + 0x1c) != Unset && f.force != 1)
        return true;
    return ground(app, c, f, run);
}

// ---- phase B ----

// Guest 0x567710, 0x5677d0 and 0x40db20: the box of the point grown by the float at radius against the view.
bool boxVisible(Flare& f, const float p[3], reg32 radius)
{
    float lo[3], hi[3];
    for (int i = 0; i < 3; ++i)
        lo[i] = hi[i] = float(double(p[i]));
    const double r = ld(radius);
    const double lo0 = double(p[0]) - r;
    const float lo1 = float(double(p[1]) - r);
    const float lo2 = float(double(p[2]) - r);
    const float hi0 = float(r + double(p[0]));
    const float hi1 = float(r + double(p[1]));
    const float hi2 = float(r + double(p[2]));
    f.b(lo0, double(lo[0]));
    if (lo0 < double(lo[0]))
        lo[0] = float(lo0);
    f.b(double(lo1), double(lo[1]));
    if (double(lo1) < double(lo[1]))
        lo[1] = lo1;
    f.b(double(lo2), double(lo[2]));
    if (double(lo2) < double(lo[2]))
        lo[2] = lo2;
    f.b(double(hi0), double(hi[0]));
    if (double(hi0) > double(hi[0]))
        hi[0] = hi0;
    f.b(double(hi1), double(hi[1]));
    if (double(hi1) > double(hi[1]))
        hi[1] = hi1;
    f.b(double(hi2), double(hi[2]));
    if (double(hi2) > double(hi[2]))
        hi[2] = hi2;
    // 0x40db20 loads x as bits, y and z through the FPU.
    const float mn[3] = { lo[0], float(double(lo[1])), float(double(lo[2])) };
    const float mx[3] = { hi[0], float(double(hi[1])), float(double(hi[2])) };
    double a, b;
    const int state = cull::boxState(mn, mx, f.view, a, b);
    f.b(a, b);
    return state != 0;
}

// Guest 0x40dc60: the point in front of the six planes of the view.
bool pointVisible(Flare& f, const float p[3])
{
    const double zero = ld(Zero);
    reg32 plane = rc::ld32(f.view) + 0xc4;
    for (int n = 0; n < 6; ++n, plane += 0x10)
    {
        double d = ld(plane + 4) * double(p[2]);
        d += ld(plane - 4) * double(p[0]);
        d += ld(plane) * double(p[1]);
        d += ld(plane + 8);
        f.b(d, zero);
        if (!(d > zero))
            return false;
    }
    return true;
}

// Guest 0x5448cd-0x544e3e: the flare position, visibility, fade, facing and the quad inputs.
void visibility(Flare& f)
{
    if (!f.live)
        return;
    const reg32 camera = rc::ld32(f.view + 0x40);
    float in[4], p4[4];
    memcpy(in, rc::host(f.flare + 0x10), 12);
    in[3] = 1.0f;
    transform(in, f.matrix, p4);
    reg32 pos[3];
    memcpy(pos, p4, 12);
    reg32 dir[3] = { 0, 0, 0 };
    if (f.mode == 2)
    {
        const reg32 height = rc::ld32(f.flare + 0x1c);
        if (height == Unset || height == NoGround)
            return;
        pos[2] = height;
        const float p[3] = { real(pos[0]), real(pos[1]), real(pos[2]) };
        if (!boxVisible(f, p, FlareRadius))
            return;
        dir[0] = rc::ld32(f.flare + 0x20);
        dir[1] = rc::ld32(f.flare + 0x24);
        dir[2] = bits(float(-ld(f.flare + 0x28)));
    }
    else if (f.mode == 1)
    {
        const float p[3] = { real(pos[0]), real(pos[1]), real(pos[2]) };
        if (!boxVisible(f, p, ModeOneRadius))
            return;
    }
    else
    {
        const float p[3] = { real(pos[0]), real(pos[1]), real(pos[2]) };
        if (!pointVisible(f, p))
            return;
        dir[0] = rc::ld32(f.flare + 0x20);
        dir[1] = rc::ld32(f.flare + 0x24);
        dir[2] = rc::ld32(f.flare + 0x28);
    }
    const float p[3] = { real(pos[0]), real(pos[1]), real(pos[2]) };
    float v[3];
    for (int i = 0; i < 3; ++i)
        v[i] = float(ld(camera + 0x40 + 4 * i) - double(p[i]));
    const float d2 = float((double(v[2]) * double(v[2]) + double(v[1]) * double(v[1])) + double(v[0]) * double(v[0]));
    const reg32 range = Ranges + (f.mode == 1 ? 0x10 : f.mode == 2 ? 8 : 0);
    const double nearer = ld(range), farther = ld(range + 4);
    f.b(double(d2), farther);
    if (double(d2) > farther)
        return;
    f.b(double(d2), nearer);
    float fade = 1.0f;
    if (double(d2) > nearer)
        fade = float(ld(One) - (double(d2) - nearer) / (farther - nearer));
    float dw[4];
    const float d4[4] = { real(dir[0]), real(dir[1]), real(dir[2]), 0.0f };
    transform(d4, f.matrix, dw);
    double length = double(v[0]) * double(v[0]);
    length += double(v[1]) * double(v[1]);
    length += double(v[2]) * double(v[2]);
    const double inverse = ld(One) / std::sqrt(length);
    float u[3];
    for (int i = 0; i < 3; ++i)
        u[i] = float(inverse * double(v[i]));
    const float dot = float((double(dw[2]) * double(u[2]) + double(dw[1]) * double(u[1])) + double(dw[0]) * double(u[0]));
    float facing = dot;
    if (sreg16(rc::ld16(f.flare + 0xe)) & 1)
    {
        facing = float(std::fabs(double(dot)));
    }
    else
    {
        f.b(double(dot), ld(Zero));
        if (double(dot) < ld(Zero))
            facing = real(0u);
    }
    const float p1[4] = { p[0], p[1], p[2], 1.0f };
    float q[4];
    transform(p1, camera, q);
    const float viewDepth = float(ld(f.view + 0xc));
    for (int i = 0; i < 3; ++i)
    {
        f.pos[i] = pos[i];
        f.toward[i] = bits(u[i]);
        f.right[i] = rc::ld32(camera + 0x10 * i);
        f.up[i] = rc::ld32(camera + 4 + 0x10 * i);
    }
    f.facing = facing;
    f.depth = float(double(q[2]) / double(viewDepth));
    f.fade = fade;
    const reg32 kind = reg32(sreg32(sreg8(rc::ld8(f.flare + 0xd))));
    f.types[0] = rc::ld32(kind * 8 + FlareTypes);
    f.types[1] = rc::ld32(kind * 8 + FlareTypes + 4);
    f.shown = true;
}

// ---- phase C ----

// Guest 0x673890 (pow, x = st1, y = st0) where the guest code stores nothing outside its stack: x > 0 or x = +-0
// with y >= 0, y finite, a normal result unless MathErrors is set. False where the guest code has to run.
bool power(x86::CPU& cpu, double x, double y, double& r)
{
    if (rc::ld32(Sse2) != 0)
        return false;
    reg64 xb, yb;
    memcpy(&xb, &x, 8);
    memcpy(&yb, &y, 8);
    const reg32 xhi = reg32(xb >> 32), xlo = reg32(xb), yhi = reg32(yb >> 32), ylo = reg32(yb);
    if ((yhi & 0x7ff00000) == 0x7ff00000 || (xhi & 0x7ff00000) == 0x7ff00000)
        return false;
    if ((xhi & 0x7ff00000) == 0 && ((xhi & 0xfffff) | xlo) == 0)
    {
        if (((yhi & 0x7fffffff) | ylo) == 0)
        {
            r = 1.0;
            return true;
        }
        reg32 odd = 0;
        const double n = x86::roundIntegral(y, 0);
        cpu.fpu.compare(n, y);
        if (n == y)
        {
            const double h = y * rc::ldf64(Half64);
            const double m = x86::roundIntegral(h, 0);
            cpu.fpu.compare(m, h);
            odd = m == h ? 0 : 1;
        }
        if (yhi & 0x80000000)
            return false;
        r = (xhi >> 31) & odd ? -0.0 : 0.0;
        return true;
    }
    if (xhi & 0x80000000)
        return false;
    const double t = double(cpu.fpu.log2(x86::Float(x))) * y;
    const double n = x86::roundIntegral(t, 0);
    const double e = double(cpu.fpu.f2xm1(x86::Float(-(n - t)))) + 1.0;
    r = double(cpu.fpu.scale(x86::Float(e), x86::Float(n)));
    if (rc::ld32(MathErrors) != 0)
        return true;
    reg64 rb;
    memcpy(&rb, &r, 8);
    const reg32 exponent = reg32(rb >> 32) & 0x7ff00000;
    return exponent != 0 && exponent != 0x7ff00000;
}

inline void fillDraw(Draw& d, const Flare& f, reg32 type)
{
    d.type = type;
    d.view = f.view;
    d.scale = f.scale;
    d.facing = bits(f.facing);
    d.depth = bits(f.depth);
    d.fade = bits(f.fade);
    d.mode = f.mode;
    for (int i = 0; i < 3; ++i)
    {
        d.pos[i] = f.pos[i];
        d.toward[i] = f.toward[i];
        d.right[i] = f.right[i];
        d.up[i] = f.up[i];
    }
    d.drawn = false;
    d.texture = 0;
    memset(d.poly, 0, sizeof(d.poly));
}

// Guest 0x5443f0 for one flare type: the quad and its submission.
bool quad(win32::WinApplication* app, x86::CPU& cpu, const Flare& f, reg32 type, Run& run)
{
    Draw d;
    if (run.draws)
        fillDraw(d, f, type);
    const reg32 texture = rc::ld32(rc::ld32(type + 0x38) * 4 + Textures);
    if (texture == 0)
    {
        if (run.draws)
            run.draws->push_back(d);
        return true;
    }
    const reg32 sp = lightFrame(f.esp) - 0x110;
    double r;
    if (!power(cpu, double(f.facing), ld(type + 0x28), r))
    {
        cpu.fpu.push(x86::Float(double(f.facing)));
        cpu.fpu.push(x86::Float(ld(type + 0x28)));
        if (!call(sub_673890, app, cpu, sp - 4, 0x54441e))
            return false;
        r = double(cpu.fpu.pop());
    }
    auto& x = cpu.fpu;
    const double one = ld(One), zero = ld(Zero);
    double intensity = r * double(real(f.scale));
    x.compare(intensity, one);
    if (intensity > one)
        intensity = one;
    const double base = ld(type);
    const double a = (ld(type + 4) - base) * intensity + base;
    x.compare(a, zero);
    if (a < zero || a == zero)
    {
        if (run.draws)
            run.draws->push_back(d);
        return true;
    }
    const double offset = ld(type + 0x2c);
    float c[3];
    for (int i = 0; i < 3; ++i)
        c[i] = float(offset * double(real(f.toward[i])) + double(real(f.pos[i])));
    double depth = double(f.depth);
    x.compare(depth, ld(type + 0x30));
    if (depth < ld(type + 0x30))
        depth = ld(type + 0x30);
    x.compare(depth, ld(type + 0x34));
    if (depth > ld(type + 0x34))
        depth = ld(type + 0x34);
    float size = float(a * depth);
    if (f.mode != 2)
        size = float(ld(SizeScale) * double(size));
    reg32 colour[4];
    colour[0] = bits(float((ld(type + 0x18) - ld(type + 8)) * intensity + ld(type + 8)));
    colour[1] = bits(float((ld(type + 0x1c) - ld(type + 0xc)) * intensity + ld(type + 0xc)));
    colour[2] = bits(float((ld(type + 0x20) - ld(type + 0x10)) * intensity + ld(type + 0x10)));
    const double alpha = (intensity * (ld(type + 0x24) - ld(type + 0x14)) + ld(type + 0x14)) * double(f.fade);
    colour[3] = bits(float(alpha));
    const double top = ld(ColourMax);
    for (int i = 0; i < 3; ++i)
    {
        x.compare(double(real(colour[i])), top);
        if (double(real(colour[i])) > top)
            colour[i] = rc::ld32(ColourMax);
    }
    x.compare(alpha, top);
    if (alpha > top)
        colour[3] = rc::ld32(ColourMax);
    const double sz = double(size);
    const float rx = float(sz * double(real(f.right[0])));
    const float ry = float(sz * double(real(f.right[1])));
    double rz = sz * double(real(f.right[2]));
    const float ux = float(sz * double(real(f.up[0])));
    const float uy = float(sz * double(real(f.up[1])));
    double uz = sz * double(real(f.up[2]));
    if (f.mode == 2)
    {
        rz *= ld(FlareRadius);
        uz *= ld(FlareRadius);
    }
    reg32 rgba = 0;
    for (int i = 0; i < 4; ++i)
        rgba |= reg32(reg8(x86::fistInteger<sreg64>(std::trunc(double(real(colour[i])))))) << (8 * i);
    const double t1 = double(c[0]) - double(rx);
    const double t2 = double(c[1]) - double(ry);
    const double t3 = double(c[2]) - rz;
    const float t3f = float(t3);
    reg32 w[PolyWords] = {};
    w[4] = bits(float(double(ux) + t1));
    w[5] = bits(float(double(uy) + t2));
    w[6] = bits(float(t3 + uz));
    w[8] = bits(float((double(ux) + double(rx)) + double(c[0])));
    w[9] = bits(float((double(uy) + double(ry)) + double(c[1])));
    w[10] = bits(float((uz + rz) + double(c[2])));
    w[12] = bits(float((double(rx) + double(c[0])) - double(ux)));
    w[13] = bits(float((double(ry) + double(c[1])) - double(uy)));
    w[14] = bits(float((rz + double(c[2])) - uz));
    w[16] = bits(float(t1 - double(ux)));
    w[17] = bits(float(t2 - double(uy)));
    w[18] = bits(float(double(t3f) - uz));
    const reg32 OneBits = 0x3f800000;
    const reg32 uv[12] = { 0, 0, OneBits, OneBits, 0, OneBits, OneBits, OneBits, OneBits, 0, OneBits, OneBits };
    for (int i = 0; i < 12; ++i)
        w[20 + i] = uv[i];
    for (int i = 32; i < 36; ++i)
        w[i] = rgba;
    w[36] = 0;
    if (run.record)
    {
        d.drawn = true;
        d.texture = texture;
        memcpy(d.poly, w, sizeof(w));
        run.draws->push_back(d);
        return true;
    }
    const reg32 poly = sp + 0x40;
    for (int i = 0; i < PolyWords; ++i)
        if (polyWord(i))
            rc::st32(poly + 4 * i, w[i]);
    rc::st32(sp - 0xc, texture);
    rc::st32(sp - 8, Identity);
    rc::st32(sp - 4, 0);
    cpu.eax = poly;
    cpu.ebx = f.mode;
    cpu.esi = type;
    cpu.edi = texture;
    cpu.ebp = lightFrame(f.esp) - 0x30;
    if (!call(sub_40c300, app, cpu, sp - 0x10, 0x54477b))
        return false;
    ++run.quads;
    return true;
}

// Phase C of one flare: the compares of phases A and B, then its two types.
bool draw(win32::WinApplication* app, x86::CPU& cpu, const Flare& f, Run& run)
{
    f.a.apply(cpu);
    f.b.apply(cpu);
    if (!f.shown)
        return true;
    for (int k = 0; k < 2; ++k)
        if (f.types[k] && !quad(app, cpu, f, f.types[k], run))
            return false;
    return true;
}

// ---- 0x5447c0 ----

void one(win32::WinApplication* app, x86::CPU& cpu, Run& run)
{
    const x86::CPU entry = cpu;
    const reg32 esp = cpu.esp;
    Flare f;
    f.view = rc::ld32(esp + 4);
    f.flare = rc::ld32(esp + 8);
    f.scale = rc::ld32(esp + 0xc);
    f.force = rc::ld32(esp + 0x10);
    f.mode = rc::ld32(esp + 0x14);
    f.height = rc::ld32(esp + 0x18);
    f.matrix = cpu.eax;
    f.esp = esp;
    x86::CPU work = cpu;
    if (!enter(app, work, f, run))
    {
        cpu.terminate = true;
        return;
    }
    visibility(f);
    if (!draw(app, cpu, f, run))
        return;
    ++run.flares;
    cpu.eax = f.shown ? 1 : 0;
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = esp + 4;
}

// ---- 0x544f30 ----

struct Event
{
    int flare;              // index in s_flares, or -1 for a compare
    double a, b;
};

std::vector<Flare> s_flares;
std::vector<Event> s_events;

void visibilityBatch(void*, int begin, int end, int)
{
    for (int i = begin; i < end; ++i)
        visibility(s_flares[size_t(i)]);
}

// One flare of the world pass: phase A now.
bool addFlare(win32::WinApplication* app, x86::CPU& work, reg32 view, reg32 flare, reg32 mode, reg32 esp, Run& run)
{
    Flare f;
    f.view = view;
    f.flare = flare;
    f.matrix = Identity;
    f.scale = 0x3f800000;
    f.force = 0;
    f.mode = mode;
    f.height = 0;
    f.esp = esp;
    if (!enter(app, work, f, run))
        return false;
    s_events.push_back(Event{int(s_flares.size()), 0, 0});
    s_flares.push_back(f);
    return true;
}

// Guest 0x544e90: the flares of a pack, once per frame, when its box is visible.
bool addPack(win32::WinApplication* app, x86::CPU& work, sreg32 id, reg32 view, reg32 mode, reg32 esp, Run& run)
{
    const sreg32 q = id / 100, r = id % 100;
    if (r > 9)
        return true;
    const reg32 pack = PackGrid + reg32(q * 10 + r) * 0x30;
    if (rc::ld32(pack + 8) == 0 || rc::ld32(pack) == 1)
        return true;
    store(run, pack, 1);
    // 0x40db20 loads x as bits, y and z through the FPU; min at +0x20, max at +0x10.
    const float mn[3] = { rc::ldf32(pack + 0x20), float(ld(pack + 0x24)), float(ld(pack + 0x28)) };
    const float mx[3] = { rc::ldf32(pack + 0x10), float(ld(pack + 0x14)), float(ld(pack + 0x18)) };
    Event e = { -1, 0, 0 };
    const int state = cull::boxState(mn, mx, view, e.a, e.b);
    s_events.push_back(e);
    if (state == 0)
        return true;
    reg32 flare = rc::ld32(pack + 0xc);
    for (sreg32 k = 0; k < sreg32(rc::ld32(pack + 8)); ++k)
    {
        if (!addFlare(app, work, view, flare, mode, esp, run))
            return false;
        flare = rc::ld32(flare);
    }
    return true;
}

// Guest 0x540c70: clears the drawn flag of every pack.
void clearPacks(Run& run)
{
    const sreg32 rows = sreg32(rc::ld32(PackRows) + 1);
    if (rows <= 0)
        return;
    const sreg32 columns = sreg32(rc::ld32(PackColumns) + 1);
    reg32 row = PackGrid;
    for (sreg32 i = 0; i < rows; ++i, row += 0x1e0)
        for (sreg32 j = 0; j < columns; ++j)
            store(run, row + reg32(j) * 0x30, 0);
}

void world(win32::WinApplication* app, x86::CPU& cpu, Run& run)
{
    const x86::CPU entry = cpu;
    const reg32 esp = cpu.esp, view = rc::ld32(esp + 4), mode = rc::ld32(esp + 8);
    s_flares.clear();
    s_events.clear();
    if (rc::ld32(FlaresOn) != 0 && rc::ld32(GameState) == 6 && rc::ld32(rc::ld32(SkipState)) != 1)
    {
        bool live = true;
        if (rc::ld32(ListMode) == 0)
        {
            rc::st32(esp - 0x1c, rc::ld32(view + 0x40) + 0x40);
            cpu.eax = Regions;
            if (!call(sub_446fd0, app, cpu, esp - 0x20, 0x544f7e))
                return;
            const reg32 region = cpu.eax;
            clearPacks(run);
            x86::CPU work = cpu;
            if (region)
            {
                for (sreg32 i = 0; live && i < sreg32(rc::ld32(region + 0x10)); ++i)
                {
                    const sreg32 id = sreg16(rc::ld16(region + 0x14 + 2 * reg32(i)));
                    const sreg32 r = id % 100;
                    live = addPack(app, work, r > 9 && r < 20 ? id - 10 : id, view, mode, esp - 0x58, run);
                }
            }
        }
        else
        {
            x86::CPU work = cpu;
            for (reg32 node = rc::ld32(FlareLists); live && node != FlareLists; node = rc::ld32(node))
                for (reg32 flare = rc::ld32(node + 0x38); live && flare != node + 0x38; flare = rc::ld32(flare))
                    live = addFlare(app, work, view, flare, mode, esp - 0x34, run);
        }
        if (!live)
        {
            cpu.terminate = true;
            return;
        }
        const int count = int(s_flares.size());
        if (settings().parallel)
            pf::jobs::run(count, BatchItems, &visibilityBatch, nullptr);
        else
            visibilityBatch(nullptr, 0, count, 0);
        for (const Event& e : s_events)
        {
            if (e.flare < 0)
                cpu.fpu.compare(e.a, e.b);
            else if (!draw(app, cpu, s_flares[size_t(e.flare)], run))
                return;
        }
        run.flares += uint32_t(count);
    }
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = esp + 4;
}

// ---- shadow compare ----

// Set while shadow mode runs the guest code: 0x5447c0 runs the guest body and 0x5443f0 records its calls here.
std::vector<Draw>* s_capture = nullptr;
bool s_guestRun = false;

struct Shadow
{
    uint32_t calls = 0, differing = 0, logged = 0;
};
Shadow s_shadow;

bool sameDraw(const Draw& n, const Draw& g, char* text, size_t size)
{
    const struct { const char* name; reg32 n, g; } fields[] = {
        {"type", n.type, g.type}, {"view", n.view, g.view}, {"scale", n.scale, g.scale},
        {"facing", n.facing, g.facing}, {"depth", n.depth, g.depth}, {"fade", n.fade, g.fade},
        {"mode", n.mode, g.mode}, {"pos.x", n.pos[0], g.pos[0]}, {"pos.y", n.pos[1], g.pos[1]},
        {"pos.z", n.pos[2], g.pos[2]}, {"toward.x", n.toward[0], g.toward[0]},
        {"toward.y", n.toward[1], g.toward[1]}, {"toward.z", n.toward[2], g.toward[2]},
        {"right.x", n.right[0], g.right[0]}, {"right.y", n.right[1], g.right[1]},
        {"right.z", n.right[2], g.right[2]}, {"up.x", n.up[0], g.up[0]}, {"up.y", n.up[1], g.up[1]},
        {"up.z", n.up[2], g.up[2]}, {"drawn", reg32(n.drawn), reg32(g.drawn)}, {"texture", n.texture, g.texture},
    };
    for (const auto& f : fields)
    {
        if (f.n != f.g)
        {
            snprintf(text, size, "%s native 0x%08x guest 0x%08x", f.name, unsigned(f.n), unsigned(f.g));
            return false;
        }
    }
    if (!n.drawn)
        return true;
    for (int i = 0; i < PolyWords; ++i)
    {
        if (polyWord(i) && n.poly[i] != g.poly[i])
        {
            snprintf(text, size, "quad +0x%02x native 0x%08x guest 0x%08x", unsigned(4 * i), unsigned(n.poly[i]),
                     unsigned(g.poly[i]));
            return false;
        }
    }
    return true;
}

// Compares a native run in record mode (its CPU, draws and guest writes) with the guest run that followed it.
void compareRuns(const char* what, const x86::CPU& native, const x86::CPU& guest, const std::vector<Draw>& nd,
                 const std::vector<Draw>& gd, const std::vector<Write>& writes, const std::vector<reg32>& values)
{
    ++s_shadow.calls;
    char text[200] = {};
    bool same = true;
    bool drew = false;
    for (const Draw& d : nd)
        drew = drew || d.drawn;
    const struct { const char* name; reg32 n, g; } regs[] = {
        {"ebx", native.ebx, guest.ebx}, {"esp", native.esp, guest.esp}, {"ebp", native.ebp, guest.ebp},
        {"esi", native.esi, guest.esi}, {"edi", native.edi, guest.edi},
        {"x87 depth", native.fpu.count, guest.fpu.count},
        {"x87 status", drew ? 0u : native.fpu.status.word, drew ? 0u : guest.fpu.status.word},
    };
    for (const auto& r : regs)
    {
        if (same && r.n != r.g)
        {
            same = false;
            snprintf(text, sizeof(text), "%s native 0x%08x guest 0x%08x", r.name, unsigned(r.n), unsigned(r.g));
        }
    }
    for (size_t i = 0; same && i < writes.size(); ++i)
    {
        const reg32 g = rc::ld32(writes[i].address);
        if (g != values[i])
        {
            same = false;
            snprintf(text, sizeof(text), "dword at 0x%08x native 0x%08x guest 0x%08x", unsigned(writes[i].address),
                     unsigned(values[i]), unsigned(g));
        }
    }
    if (same && nd.size() != gd.size())
    {
        same = false;
        snprintf(text, sizeof(text), "%u quad calls native, %u guest", unsigned(nd.size()), unsigned(gd.size()));
    }
    for (size_t i = 0; same && i < nd.size(); ++i)
    {
        char field[160];
        if (!sameDraw(nd[i], gd[i], field, sizeof(field)))
        {
            same = false;
            snprintf(text, sizeof(text), "quad call %u: %s", unsigned(i), field);
        }
    }
    if (same)
        return;
    ++s_shadow.differing;
    if (s_shadow.logged < ShadowLogLimit)
    {
        ++s_shadow.logged;
        pf::logInfo("shadow_flares: call %u (%s): %s", unsigned(s_shadow.calls), what, text);
    }
}

std::vector<Draw> s_nativeDraws, s_guestDraws;
std::vector<Write> s_writes;
std::vector<reg32> s_values;

// Runs native into records, takes its guest writes back, runs the guest code for real and compares.
void shadowRun(const char* what, win32::WinApplication* app, x86::CPU& cpu,
               void (*native)(win32::WinApplication*, x86::CPU&, Run&),
               void (*guest)(win32::WinApplication*, x86::CPU&), Run& run)
{
    s_nativeDraws.clear();
    s_guestDraws.clear();
    s_writes.clear();
    s_values.clear();
    run.record = true;
    run.draws = &s_nativeDraws;
    run.writes = &s_writes;
    x86::CPU n = cpu;
    native(app, n, run);
    if (n.terminate)
    {
        cpu.terminate = true;
        return;
    }
    for (const Write& w : s_writes)
        s_values.push_back(rc::ld32(w.address));
    for (size_t i = s_writes.size(); i-- > 0;)
        rc::st32(s_writes[i].address, s_writes[i].old);
    s_capture = &s_guestDraws;
    s_guestRun = true;
    guest(app, cpu);
    s_capture = nullptr;
    s_guestRun = false;
    if (!cpu.terminate)
        compareRuns(what, n, cpu, s_nativeDraws, s_guestDraws, s_writes, s_values);
}

struct PerfSums
{
    uint64_t us = 0;
    uint32_t calls = 0, flares = 0, quads = 0, maxUs = 0;
};
PerfSums s_perf;

void account(uint32_t start, const Run& run)
{
    if (!pf::perf::enabled())
        return;
    const uint32_t us = pf::perf::now() - start;
    s_perf.us += us;
    s_perf.calls++;
    s_perf.flares += run.flares;
    s_perf.quads += run.quads;
    if (us > s_perf.maxUs)
        s_perf.maxUs = us;
}

}

void worldFlares(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
    Run run;
    if (!s.native)
        sub_544f30_guest(app, cpu);
    else if (s.shadow)
        shadowRun("world", app, cpu, &world, &sub_544f30_guest, run);
    else
        world(app, cpu, run);
    account(start, run);
}

void lightFlare(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    if (!s.native || s_guestRun)
    {
        sub_5447c0_guest(app, cpu);
        return;
    }
    const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
    Run run;
    if (s.shadow)
        shadowRun("flare", app, cpu, &one, &sub_5447c0_guest, run);
    else
        one(app, cpu, run);
    account(start, run);
}

void flareQuad(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_capture)
    {
        sub_5443f0_guest(app, cpu);
        return;
    }
    const reg32 esp = cpu.esp;
    Draw d;
    d.type = cpu.esi;
    d.view = rc::ld32(esp + 4);
    for (int i = 0; i < 3; ++i)
    {
        d.pos[i] = rc::ld32(rc::ld32(esp + 8) + 4 * i);
        d.toward[i] = rc::ld32(rc::ld32(esp + 0xc) + 4 * i);
        d.right[i] = rc::ld32(rc::ld32(esp + 0x10) + 4 * i);
        d.up[i] = rc::ld32(rc::ld32(esp + 0x14) + 4 * i);
    }
    d.scale = rc::ld32(esp + 0x18);
    d.facing = rc::ld32(esp + 0x1c);
    d.depth = rc::ld32(esp + 0x20);
    d.fade = rc::ld32(esp + 0x24);
    d.mode = rc::ld32(esp + 0x28);
    const reg32 sp = quadFrame(esp), poly = sp + 0x40;
    for (int i = 0; i < PolyWords; ++i)
        if (polyWord(i))
            rc::st32(poly + 4 * i, Poison);
    sub_5443f0_guest(app, cpu);
    // Quad word +0x90: written by the guest only when it submits.
    d.drawn = rc::ld32(poly + 0x90) != Poison;
    d.texture = d.drawn ? rc::ld32(sp - 0xc) : 0;
    for (int i = 0; i < PolyWords; ++i)
        d.poly[i] = d.drawn && polyWord(i) ? rc::ld32(poly + 4 * i) : 0;
    s_capture->push_back(d);
}

void report(uint32_t frames)
{
    if (frames == 0)
        return;
    const Settings& s = settings();
    pf::logInfo("flares: %s %u flares/frame, %u quads/frame, %.2f ms/frame (max %.2f ms per call)",
                !s.native ? "guest" : s.shadow ? "shadow" : s.parallel ? "parallel" : "serial",
                s_perf.flares / frames, s_perf.quads / frames, double(s_perf.us) / 1000.0 / frames,
                double(s_perf.maxUs) / 1000.0);
    if (s.native && s.shadow)
        pf::logInfo("shadow_flares: %u calls compared, %u differ", unsigned(s_shadow.calls),
                    unsigned(s_shadow.differing));
    s_perf = PerfSums();
}

}
}

namespace nfsu { namespace native
{

void sub_544f30(win32::WinApplication* app, x86::CPU& cpu)
{
    flares::worldFlares(app, cpu);
}

void sub_5447c0(win32::WinApplication* app, x86::CPU& cpu)
{
    flares::lightFlare(app, cpu);
}

void sub_5443f0(win32::WinApplication* app, x86::CPU& cpu)
{
    flares::flareQuad(app, cpu);
}

}}
