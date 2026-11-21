// Native parts of the world views (viewparts.h): the scenery culler setup 0x408f00 with the cull entry setup
// 0x53e290 / 0x408e30, the fog of the player view 0x4090a0 with FogVolume::CalculateDistantFogInfo 0x588f40, the
// screen state reset 0x408b20, the rear-view mirror quad 0x409180, the motion blur 0x40cd40 with its passes 0x40d370
// and 0x40d090, the screen filter pass 0x40d6e0 and the flare screen pass 0x409b70. Bit-exact with the translated
// guest code: x87 chains in double in the guest's order, floats where the guest stores floats, the same guest stores,
// the same device and effect calls in the same order, and the registers the callers read.

#include "viewparts.h"
#include "draw.h"
#include "groups.h"
#include "retstate.h"
#include "sceneshadow.h"

#include <platform/config.h>
#include <platform/log.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <recomp.h>
#include <vector>
#include <winapi/d3d9_native.h>

namespace nfsu
{

// Generated functions: the overridden guest bodies and the guest-convention entries the ports call.
void sub_408f00_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4090a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_408b20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_409180_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40cd40_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40d090_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40d370_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40d6e0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_409b70_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40d090(win32::WinApplication* app, x86::CPU& cpu);
void sub_40d370(win32::WinApplication* app, x86::CPU& cpu);
void sub_40d6e0(win32::WinApplication* app, x86::CPU& cpu);
void sub_40e6d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_40ed50(win32::WinApplication* app, x86::CPU& cpu);
void sub_544f30(win32::WinApplication* app, x86::CPU& cpu);
void sub_4ae7d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_570d00(win32::WinApplication* app, x86::CPU& cpu);
void sub_570c70(win32::WinApplication* app, x86::CPU& cpu);
void sub_5ac443(win32::WinApplication* app, x86::CPU& cpu);
void sub_567450(win32::WinApplication* app, x86::CPU& cpu);
void sub_446620(win32::WinApplication* app, x86::CPU& cpu);
void sub_410400(win32::WinApplication* app, x86::CPU& cpu);
void sub_410550(win32::WinApplication* app, x86::CPU& cpu);
void sub_410650(win32::WinApplication* app, x86::CPU& cpu);

namespace viewparts
{

namespace
{

using win32::DWORD;
using win32::HRESULT;
using x86::reg8;
using x86::reg32;
using x86::sreg32;
using draw::Span;
namespace bridge = win32::d3d9::bridge;
typedef win32::WinApplication App;
typedef void (*Function)(App*, x86::CPU&);

// Guest data.
const reg32 DevicePointer = 0x73636c;
const reg32 CurrentGroup = 0x73640c, CurrentView = 0x736458;
const reg32 Groups = 0x71abb8;          // effect group 0: vertex declaration +4, effect +0x18, start +0xd8, end +0xdc
const reg32 AlphaRef = 0x7070c0, AlphaTestOn = 0x7070c4, BlendDestination = 0x7070c8, BlendSource = 0x7070cc,
            BlendOn = 0x7070d0;
const reg32 LastView = 0x7010b8, LastGroup = 0x78eb7c, WorldCalls = 0x71b1f4, StreamCache = 0x7363e8,
            LastModel = 0x736404;
const reg32 Zero = 0x6cc7a4, One = 0x6cc7bc, ByteScale = 0x6cc970;
const reg32 FogVolume = 0x737bf0;       // colour terms +0..+0x10, colour +0x14, 0x50-byte entries from +0x20, count
const reg32 FogEntries = 0x3078;        // at +0x3078
const reg32 FogChosen = 0x6f0724;
const reg32 FeFog = 0x734520;
const reg32 FilterGroup = 0x71b0f8;     // effect group 6: the screen filter
const reg32 FilterEffect = 0x71b110;    // its effect, the vector array handle at 0x71b188, texture handles from
const reg32 FilterVectors = 0x71b188, FilterTextures = 0x71b190;   // 0x71b190
const reg32 BlurLevel = 0x73641c;       // motion blur level, eased towards [0x73654c]
const reg32 LastModelGroup = 0x736408;
const reg32 WhiteName = 0x6cc0a8;      // "WHITE16X16_NOALPHA"

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
        const Settings r = { pf::Config::getBool("view_parts_native", true),
                             pf::Config::getBool("shadow_view_parts", false) };
        pf::logInfo("view_parts: culler setup, view fog, screen passes, mirror quad and motion blur %s%s",
                    r.native ? "native" : "guest", r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

inline reg32 ld(reg32 a) { return rc::ld32(a); }
inline double ldf(reg32 a) { return double(rc::ldf32(a)); }
inline float pass(reg32 a) { return float(double(rc::ldf32(a))); }
inline reg32 bits(float v) { reg32 r; memcpy(&r, &v, 4); return r; }
inline float real(reg32 v) { float r; memcpy(&r, &v, 4); return r; }

// A guest store outside the stack.
inline void st(reg32 a, reg32 v) { sceneshadow::store(a, v); }
inline void stf(reg32 a, float v) { st(a, bits(v)); }

// _ftol2's integer (low word).
inline reg32 ftol(double v) { return reg32(x86::truncInteger64(v)); }

// SetRenderState as the guest calls it: the device in eax, its vtable in ecx or edx, the result in eax.
void renderState(App* app, x86::CPU& cpu, DWORD state, DWORD value, bool edx)
{
    const reg32 device = ld(DevicePointer);
    (edx ? cpu.edx : cpu.ecx) = ld(device);
    cpu.eax = bridge::setRenderState(app, cpu, device, state, value);
}

void samplerState(App* app, x86::CPU& cpu, DWORD sampler, DWORD type, DWORD value, bool edx)
{
    const reg32 device = ld(DevicePointer);
    (edx ? cpu.edx : cpu.ecx) = ld(device);
    cpu.eax = bridge::setSamplerState(app, cpu, device, sampler, type, value);
}

// An effect group function (no arguments) with the return address at slot.
bool groupCall(App* app, x86::CPU& cpu, reg32 slot, reg32 back, reg32 target)
{
    const reg32 esp = cpu.esp;
    rc::st32(slot, back);
    cpu.esp = slot;
    // Inside a compare the function's own device calls are recorded, as when the guest code calls it.
    if (bridge::capturing())
        app->dynamic_call(target, cpu);
    else
        groups::call(app, cpu, target);
    if (cpu.terminate)
        return false;
    cpu.esp = esp;
    return true;
}

// A guest-convention call with the return address at slot; esp and the callee-saved registers as the x86 calling
// convention leaves them.
bool callAt(App* app, x86::CPU& cpu, reg32 slot, reg32 back, Function f)
{
    const reg32 ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;
    rc::st32(slot, back);
    cpu.esp = slot;
    f(app, cpu);
    if (cpu.terminate)
        return false;
    cpu.esp = slot + 4;
    cpu.ebx = ebx;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.edi = edi;
    return true;
}

// The blend, alpha test and alpha reference resets with the current group's effect or the device (0x408d73 and
// 0x40951e): the blend sources invalid and blending off; eax, ecx, edx as the guest leaves them.
void blendOff(App* app, x86::CPU& cpu)
{
    cpu.ecx = 0xffffffffu;
    st(BlendSource, cpu.ecx);
    st(BlendDestination, cpu.ecx);
    const reg32 group = ld(CurrentGroup);
    if (group == 0)
    {
        renderState(app, cpu, 0x1b, 0, true);
    }
    else
    {
        cpu.ecx = ld(group + 0x18);
        const reg32 handle = ld(group + 0xc4);
        cpu.edx = ld(cpu.ecx);
        cpu.eax = bridge::effectSetInt(app, cpu, cpu.ecx, handle, 0);
    }
}

// ---- 0x408f00: scenery culler setup ----

// 0x53e290 with 0x408e30: the cull entry of a view (flags +0x84, the view +0x80). E = the esp of 0x53e290's return
// address slot. eax, ecx, edx as 0x408e30 leaves them (the rotation matrix it builds in its frame is not used).
void cullEntry(x86::CPU& cpu, reg32 entry, reg32 view, reg32 flags, reg32 E)
{
    reg32 f = flags | 0x10;
    const sreg32 id = sreg32(ld(view + 4));
    const reg32 layout = ld(0x7364a0);
    if (id == 1 || id == 2)
        f |= 2;
    if (layout == 4 || layout == 3)
        f |= 1;
    if (id >= 0xa && id <= 0xf)
        f |= 0x100;
    const reg32 mode = ld(0x78a2fc);
    if (mode == 0)
        f |= 8;
    else if (mode == 1)
        f |= 4;
    if (f & 0x800)
        f |= 0x40;
    else if (f & 0x1000)
        f |= 0x20;
    // 0x408e30's frame: its return address slot at E - 20, the D3DX result at +0xc of its aligned frame.
    const reg32 frame = (((E - 20) - 4) & ~15u) - 0x8c;
    st(entry + 0x80, view);
    st(entry + 0x84, f);
    cpu.eax = entry;
    cpu.ecx = frame + 0xc;
    cpu.edx = frame + 0xc;
}

// eax = the cull info (0xd0-byte entries, count at +0x820): an entry per drawn view, then the culler 0x446620.
void cullSetup(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, info = cpu.eax;
    const reg8 again = rc::ld8(0x71db20);
    cpu.eax = (cpu.eax & ~0xffu) | again;
    if (again == 0)
        st(info + 0x820, 0);
    cpu.ecx = ld(0x702c80);
    bool views = false;
    if (cpu.ecx != 0 && ld(0x77a920) == 6)
    {
        cpu.ecx = ld(0x7361f0);
        views = ld(cpu.ecx) != 1 && again == 0;
    }
    const auto add = [&](reg32 view, reg32 flags, reg32 E53)
    {
        cpu.ecx = ld(info + 0x820);
        const reg32 e = cpu.ecx * 0xd0 + info;
        ++cpu.ecx;
        st(info + 0x820, cpu.ecx);
        cullEntry(cpu, e, view, flags, E53);
    };
    if (views)
    {
        // The player views 1 and 2 (esi and edi pushed).
        for (reg32 view = 0x7404c0; sreg32(view) <= sreg32(0x740580); view += 0x60)
        {
            const reg8 on = rc::ld8(view + 8);
            cpu.eax = (cpu.eax & ~0xffu) | on;
            if (on == 0)
                continue;
            cpu.edx = ld(view + 0x58);
            cpu.eax = ld(cpu.edx + 0x10);
            if (cpu.eax == 0)
                continue;
            add(view, view == 0x740580 || ld(0x736480) != 0 ? 0x1000 : 0x2000, E - 16);
        }
        // The reflection views.
        cpu.eax = ld(0x700e74);
        if (cpu.eax != 0)
        {
            for (reg32 view = 0x740820; sreg32(view) <= sreg32(0x740a00); view += 0x60)
            {
                const reg8 on = rc::ld8(view + 8);
                cpu.eax = (cpu.eax & ~0xffu) | on;
                if (on == 0)
                    continue;
                cpu.eax = ld(view + 0x58);
                cpu.ecx = ld(cpu.eax + 0x10);
                if (cpu.ecx == 0)
                    continue;
                add(view, 0x1000, E - 16);
            }
        }
        // The env map faces and the reflection scenery view (esi pushed).
        cpu.eax = ld(0x700e90);
        if (cpu.eax != 0)
        {
            const reg8 on = rc::ld8(0x7405e8);
            cpu.eax = (cpu.eax & ~0xffu) | on;
            if (on != 0)
            {
                cpu.ecx = ld(0x740638);
                cpu.eax = ld(cpu.ecx + 0x10);
                if (cpu.eax != 0)
                    add(0x7405e0, 0x800, E - 12);
            }
        }
        cpu.eax = ld(0x700e80);
        if (cpu.eax != 0)
        {
            const reg8 on = rc::ld8(0x740768);
            cpu.eax = (cpu.eax & ~0xffu) | on;
            if (on != 0)
                add(0x740760, 0, E - 12);
        }
    }
    // The culler with esi = the cull info (esi pushed).
    rc::st32(E - 4, entry.esi);
    rc::st32(E - 8, 0x0040908b);
    cpu.esp = E - 8;
    cpu.esi = info;
    if (!sceneshadow::call(0x446620, 0, retstate::Eax | retstate::Ecx | retstate::Edx | retstate::Esi, app, cpu,
                           &sub_446620))
    {
        cpu.terminate = true;
        return;
    }
    cpu.ebx = entry.ebx;
    cpu.ebp = entry.ebp;
    cpu.edi = entry.edi;
    cpu.esi = entry.esi;
    cpu.esp = E + 4;
}

// ---- 0x4090a0: fog of a view ----

// 0x588f40 (edi = the fog volume, eax = the view, [esp + 4] = 0): the distant fog colour terms and colour from the
// fog entries around the view's camera. eax, ecx, edx as it leaves them; the x87 condition codes of its compares.
void distantFog(x86::CPU& cpu, reg32 view)
{
    const reg32 F = FogVolume;
    const reg32 camera = ld(view + 0x40);
    const double px = double(real(ld(camera + 0x40)));
    const double py = double(pass(camera + 0x44));
    if (ld(FeFog) != 0)
    {
        const reg32 a = ld(0x6f06e8), b = ld(0x6f06ec), c = ld(0x6f06fc), d = ld(0x6f06f0);
        st(F, a);
        st(F + 0x10, ld(0x6f06f4));
        reg32 colour = ld(0x6f0704) << 8;
        st(F + 4, b);
        colour |= ld(0x6f0700);
        const reg32 e = ld(0x6f06f8);
        colour = ((colour << 8) | c) & 0xffffff;
        colour |= 0x80000000u;
        st(F + 8, d);
        st(F + 0xc, e);
        st(F + 0x14, colour);
        cpu.eax = colour;
        cpu.ecx = e;
        cpu.edx = d;
        return;
    }
    st(F + 0xc, 0);
    st(F + 0x10, 0);
    st(F, 0);
    st(F + 4, 0);
    st(F + 8, 0);
    // The distance of each entry (+0x40, +0x44) from the camera, at +0x5c.
    for (sreg32 i = 0; i < sreg32(ld(F + FogEntries)); ++i)
    {
        const reg32 e = F + 0x50 * reg32(i);
        const double dx = ldf(e + 0x40) - px;
        const double dy = ldf(e + 0x44) - py;
        stf(e + 0x5c, float(sqrt(dy * dy + dx * dx)));
    }
    // Each entry's kind (+0x60: 4 outside its radius +0x4c, 1 inside its inner radius +0x50, 2 between) and weight
    // (+0x64); the innermost entry of kind 1 with the smallest radius is chosen.
    double best = ldf(0x6cca64);
    reg32 chosen = 0xffffffffu;
    st(FogChosen, chosen);
    if (sreg32(ld(F + FogEntries)) > 0)
    {
        sreg32 i = 0;
        do
        {
            const reg32 c = F + 0x30 + 0x50 * reg32(i);
            const double a = ldf(c + 0x2c), outer = ldf(c - 4);
            cpu.fpu.compare(a, outer);
            if (a < outer)
            {
                const double inner = ldf(c);
                cpu.fpu.compare(a, inner);
                const double t = a < inner ? ldf(Zero) : (ldf(c + 0x2c) - inner) / (ldf(c - 4) - inner);
                stf(c + 0x34, float(ldf(One) - t));
                const double zero = ldf(Zero);
                cpu.fpu.compare(t, zero);
                if (t == zero)
                {
                    const double radius = ldf(c - 4);
                    cpu.fpu.compare(best, radius);
                    st(c + 0x30, 1);
                    st(c + 0x34, 0x3f800000u);
                    if (best > radius)
                    {
                        chosen = reg32(i);
                        best = ldf(c - 4);
                    }
                }
                else
                {
                    st(c + 0x30, 2);
                }
            }
            else
            {
                st(c + 0x30, 4);
                st(c + 0x34, 0);
            }
            ++i;
        } while (i < sreg32(ld(F + FogEntries)));
        st(FogChosen, chosen);
        if (chosen != 0xffffffffu)
        {
            // The other entries of kind 1 lose their weight; the chosen one is scaled down by those of kind 2.
            const reg32 d = chosen * 0x50 + F + 0x20;
            if (sreg32(ld(F + FogEntries)) > 0)
            {
                sreg32 j = 0;
                do
                {
                    const reg32 w = F + 0x64 + 0x50 * reg32(j);
                    if (reg32(j) != chosen && ld(w - 4) == 1)
                        st(w, 0);
                    if (ld(w - 4) == 2)
                        stf(d + 0x44, float((ldf(One) - ldf(w)) * ldf(d + 0x44)));
                    ++j;
                } while (j < sreg32(ld(F + FogEntries)));
            }
        }
    }
    // The weighted sums of the entries' colour terms and colour bytes.
    reg32 sum0 = 0, sum1 = 0, sum2 = 0;
    for (sreg32 i = 0; i < sreg32(ld(F + FogEntries)); ++i)
    {
        const reg32 s = F + 0x64 + 0x50 * reg32(i);
        const double w = ldf(s);
        cpu.fpu.compare(w, ldf(Zero));
        if (w == ldf(Zero))
            continue;
        stf(F, float(ldf(s - 0x14) * ldf(s) + ldf(F)));
        stf(F + 4, float(ldf(s - 0x10) * ldf(s) + ldf(F + 4)));
        stf(F + 8, float(ldf(s - 0xc) * ldf(s) + ldf(F + 8)));
        stf(F + 0xc, float(ldf(s - 0x3c) * ldf(s) + ldf(F + 0xc)));
        stf(F + 0x10, float(ldf(s - 0x40) * ldf(s) + ldf(F + 0x10)));
        sum0 += ftol(double(rc::ld8(s - 0x44)) * ldf(s));
        sum1 += ftol(double(rc::ld8(s - 0x43)) * ldf(s));
        sum2 += ftol(double(rc::ld8(s - 0x42)) * ldf(s));
    }
    st(F + 0x14, ((((sum2 | 0xffff8000u) << 8) | sum1) << 8) | sum0);
    cpu.eax = 0;
    cpu.ecx = 0;
    cpu.edx = sum0;
}

// eax = the view, esi = the fog parameters: scale terms +0, +4, density +8, distance +0xc, colour +0x10..+0x18 (blue,
// green, red); density 0 when the fog is too thin.
void viewFog(App* app, x86::CPU& cpu)
{
    (void)app;
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, out = cpu.esi;
    distantFog(cpu, cpu.eax);
    const reg32 F = FogVolume;
    double v = (ldf(One) - ldf(F)) * ldf(F + 0x10);
    v = v + v;
    const double k = ldf(0x6ccb7c);
    cpu.fpu.compare(v, k);
    cpu.eax = (cpu.eax & 0xffff0000u) | cpu.fpu.status.word;
    if (!(v > k))
    {
        st(out + 8, 0);
    }
    else
    {
        const double q = ldf(One) / v;
        stf(out, float(ldf(0x6b7c84) * q));
        stf(out + 4, float(q * ldf(0x6b7c88)));
        stf(out + 8, float(double(rc::ld8(F + 0x17)) * ldf(0x6b7c90) * ldf(ByteScale)));
        stf(out + 0xc, float(ldf(0x6b7c8c) * ldf(F + 8)));
        const reg32 blue = rc::ld8(F + 0x14), green = rc::ld8(F + 0x15), red = rc::ld8(F + 0x16);
        stf(out + 0x18, float(double(blue) * ldf(ByteScale)));
        stf(out + 0x14, float(double(green) * ldf(ByteScale)));
        stf(out + 0x10, float(double(red) * ldf(ByteScale)));
        cpu.eax = red;
        cpu.ecx = blue;
        cpu.edx = green;
    }
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// ---- 0x408b20: screen states ----

// The render, sampler and texture stage states of a screen pass, effect group 0 with its first pass, blending and
// the alpha test off. ecx: the slot of the pushed ecx, where Begin stores the pass count.
void screenStates(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp;
    rc::st32(E - 4, entry.ecx);
    rc::st32(E - 8, entry.esi);
    rc::st32(E - 12, entry.edi);
    static const DWORD States[9][2] = { {7, 1}, {0xe, 1}, {0x17, 4}, {0x19, 5}, {0x1d, 1}, {0x89, 0}, {0x16, 1},
                                        {0x1c, 0}, {0xa8, 0xf} };
    static const DWORD Samplers[12][3] = { {0, 6, 2}, {1, 6, 2}, {2, 6, 2}, {3, 6, 2}, {0, 5, 2}, {1, 5, 2},
                                           {2, 5, 2}, {3, 5, 2}, {0, 7, 1}, {1, 7, 1}, {2, 7, 1}, {3, 7, 1} };
    uint32_t k = 0;
    for (const auto& s : States)
        renderState(app, cpu, s[0], s[1], (k++ & 1) != 0);
    for (const auto& s : Samplers)
        samplerState(app, cpu, s[0], s[1], s[2], (k++ & 1) != 0);
    cpu.edi = 0;
    for (reg32 stage = 0; stage < 8; ++stage)
    {
        cpu.esi = stage;
        static const DWORD Stages[3][2] = { {1, 1}, {4, 1}, {0x18, 0} };
        for (uint32_t i = 0; i < 3; ++i)
        {
            const reg32 device = ld(DevicePointer);
            (i == 1 ? cpu.ecx : cpu.edx) = ld(device);
            cpu.eax = bridge::setTextureStageState(app, cpu, device, stage, Stages[i][0], Stages[i][1]);
        }
    }
    cpu.eax = ld(CurrentGroup);
    if (cpu.eax != Groups)
    {
        if (cpu.eax != 0)
        {
            const reg32 effect = ld(cpu.eax + 0x18);
            cpu.ecx = ld(effect);
            cpu.eax = bridge::effectEnd(app, cpu, effect);
            cpu.edx = ld(CurrentGroup);
            if (!groupCall(app, cpu, E - 16, 0x00408d26, ld(cpu.edx + 0xdc)))
                return;
        }
        st(CurrentGroup, Groups);
        if (!groupCall(app, cpu, E - 16, 0x00408d36, ld(Groups + 0xd8)))
            return;
        cpu.edx = ld(Groups + 4);
        reg32 device = ld(DevicePointer);
        cpu.ecx = ld(device);
        cpu.eax = bridge::setVertexDeclaration(app, cpu, device, cpu.edx);
        reg32 effect = ld(Groups + 0x18);
        cpu.ecx = ld(effect);
        cpu.edx = E - 4;
        cpu.eax = bridge::effectBegin(app, cpu, effect, reinterpret_cast<win32::UINT*>(rc::host(E - 4)), 0);
        effect = ld(Groups + 0x18);
        cpu.ecx = ld(effect);
        cpu.eax = bridge::effectPass(app, cpu, effect, 0);
        cpu.eax = ld(CurrentGroup);
    }
    blendOff(app, cpu);
    reg32 group = ld(CurrentGroup);
    st(BlendOn, 0);
    if (group == 0)
    {
        renderState(app, cpu, 0xf, 0, false);
    }
    else
    {
        cpu.eax = ld(group + 0x18);
        cpu.ecx = ld(group + 0xbc);
        cpu.edx = ld(cpu.eax);
        cpu.eax = bridge::effectSetInt(app, cpu, cpu.eax, cpu.ecx, 0);
    }
    group = ld(CurrentGroup);
    st(AlphaTestOn, 0);
    if (group == 0)
    {
        renderState(app, cpu, 0x18, 0, true);
    }
    else
    {
        cpu.eax = ld(group + 0x18);
        cpu.ecx = ld(group + 0xc0);
        cpu.edx = ld(cpu.eax);
        cpu.eax = bridge::effectSetInt(app, cpu, cpu.eax, cpu.ecx, 0);
    }
    st(AlphaRef, 0);
    cpu.ecx = ld(E - 4);
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// ---- 0x409180: full-screen quad of the rear-view mirror frame and mask ----

// eax = the vertex colour, [esp + 4] = the texture, [esp + 8] / [esp + 0xc] = the blend source / destination: the
// current view's matrices become identities for one quad (the world scaled by [0x6d3444]), then come back.
void mirrorQuad(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp;
    const reg32 T = ((((E - 4) & ~15u) - 0x134) - 12);
    const reg32 colour = cpu.eax;
    const reg32 texture = ld(E + 4), source = ld(E + 8), destination = ld(E + 0xc);
    const reg32 one = 0x3f800000u, minus = 0xbf800000u;
    const reg32 vertices[24] = { minus, minus, 0, colour, 0, one,   minus, one, 0, colour, 0, 0,
                                 one, one, 0, colour, one, 0,       one, minus, 0, colour, one, one };
    memcpy(rc::host(T + 0x60), vertices, sizeof(vertices));
    const reg32 scale = ld(0x6d3444);
    const reg32 world[16] = { scale, 0, 0, 0, 0, one, 0, 0, 0, 0, one, 0, 0, 0, 0, one };
    memcpy(rc::host(T + 0x20), world, sizeof(world));

    const reg32 record = ld(ld(CurrentView) + 4) * 0x60 + 0x740460;
    const reg32 V = ld(record), P = V + 0x40;
    rc::st32(T + 0x18, record);
    reg32 view[16], projection[16];
    memcpy(view, rc::host(V), 64);
    memcpy(projection, rc::host(P), 64);
    static const reg32 Identity[16] = { 0x3f800000u, 0, 0, 0, 0, 0x3f800000u, 0, 0,
                                        0, 0, 0x3f800000u, 0, 0, 0, 0, 0x3f800000u };
    for (reg32 i = 0; i < 16; ++i)
        st(V + 4 * i, Identity[i]);
    for (reg32 i = 0; i < 16; ++i)
        st(P + 4 * i, Identity[i]);
    rc::st32(T + 0x1c, V + 0x80);
    const auto product = [&]()
    {
        float a[16], b[16], r[16];
        memcpy(a, rc::host(V), 64);
        memcpy(b, rc::host(P), 64);
        bridge::matrixMultiply(a, b, r);
        for (reg32 i = 0; i < 16; ++i)
            stf(V + 0x80 + 4 * i, r[i]);
    };
    product();

    // The world transform: edi = the word at the view record + 4.
    cpu.eax = V + 0x80;
    cpu.ecx = T + 0x20;
    cpu.edx = record;
    cpu.ebx = V;
    cpu.esi = 0;
    cpu.edi = ld(record + 4);
    cpu.ebp = E - 4;
    st(LastView, 0xffffffffu);
    rc::st32(T - 4, T + 0x20);
    if (!callAt(app, cpu, T - 8, 0x004093bd, &sub_410400))
        return;
    cpu.esp = T;
    for (reg32 i = 0; i < 8; ++i)
        samplerState(app, cpu, i >> 1, 1 + (i & 1), 3, (i & 1) != 0);
    {
        const reg32 device = ld(DevicePointer);
        cpu.edx = texture;
        cpu.ecx = ld(device);
        cpu.eax = bridge::setTexture(app, cpu, device, 0, texture);
    }
    cpu.esi = destination;
    cpu.edi = source;
    rc::st32(T - 4, 1);
    if (!callAt(app, cpu, T - 8, 0x0040947e, &sub_410550))
        return;
    cpu.esp = T;
    cpu.esi = 0;
    cpu.edi = 0;
    if (!callAt(app, cpu, T - 4, 0x0040948a, &sub_410650))
        return;
    cpu.esp = T;
    renderState(app, cpu, 7, 0, false);
    renderState(app, cpu, 0xe, 0, true);
    {
        const reg32 device = ld(DevicePointer);
        cpu.ecx = ld(device);
        cpu.edx = T + 0x60;
        cpu.eax = bridge::drawPrimitiveUP(app, cpu, device, 6, 2, rc::host(T + 0x60), 0x18);
        if (cpu.terminate)
            return;
    }
    renderState(app, cpu, 0xe, 1, false);
    renderState(app, cpu, 7, 1, true);
    cpu.eax = 0xffffffffu;
    st(BlendSource, cpu.eax);
    st(BlendDestination, cpu.eax);
    const reg32 group = ld(CurrentGroup);
    if (group == 0)
    {
        renderState(app, cpu, 0x1b, 0, false);
    }
    else
    {
        cpu.ecx = ld(group + 0x18);
        const reg32 handle = ld(group + 0xc4);
        cpu.edx = ld(cpu.ecx);
        cpu.eax = bridge::effectSetInt(app, cpu, cpu.ecx, handle, 0);
    }
    for (reg32 i = 0; i < 16; ++i)
        st(V + 4 * i, view[i]);
    for (reg32 i = 0; i < 16; ++i)
        st(P + 4 * i, projection[i]);
    st(BlendOn, 0);
    product();
    cpu.eax = ld(CurrentView);
    cpu.ecx = V + 0x80;
    cpu.edx = T + 0x20;
    cpu.edi = ld(cpu.eax + 4);
    cpu.esi = T + 0x100;
    rc::st32(T - 4, T + 0x20);
    if (!callAt(app, cpu, T - 8, 0x0040956d, &sub_410400))
        return;
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// ---- the screen filter passes: 0x40d6e0, 0x40d090, 0x40d370 ----

// A device call's device (eax) and vtable (ecx or edx) as the guest loads them.
reg32 deviceRegs(x86::CPU& cpu, bool edx)
{
    const reg32 device = ld(DevicePointer);
    cpu.eax = device;
    (edx ? cpu.edx : cpu.ecx) = ld(device);
    return device;
}

// Effect::SetTexture of the filter effect at its texture slot k: eax = the effect, the vtable in ecx and the handle
// in edx (handleInEcx: the other way round).
void filterTexture(App* app, x86::CPU& cpu, reg32 k, reg32 texture, bool handleInEcx)
{
    const reg32 effect = ld(FilterEffect), handle = ld(FilterTextures + 4 * k);
    cpu.eax = effect;
    (handleInEcx ? cpu.edx : cpu.ecx) = ld(effect);
    (handleInEcx ? cpu.ecx : cpu.edx) = handle;
    cpu.eax = bridge::effectSetTexture(app, cpu, effect, handle, texture);
}

// Effect::SetVectorArray of the filter effect's 4 vectors, the handle in edx (handleInEcx: in ecx).
void filterVectors(App* app, x86::CPU& cpu, const reg32* vectors, bool handleInEcx)
{
    const reg32 effect = ld(FilterEffect), handle = ld(FilterVectors);
    cpu.eax = effect;
    (handleInEcx ? cpu.edx : cpu.ecx) = ld(effect);
    (handleInEcx ? cpu.ecx : cpu.edx) = handle;
    float v[16];
    memcpy(v, vectors, sizeof(v));
    cpu.eax = bridge::effectSetVectorArray(app, cpu, effect, handle, v, 4);
}

// The blend reset of the filter passes: blend sources invalid, blending off through the current group's effect or
// the device (its vtable in ecx).
void blendReset(App* app, x86::CPU& cpu)
{
    st(BlendSource, 0xffffffffu);
    st(BlendDestination, 0xffffffffu);
    const reg32 group = ld(CurrentGroup);
    cpu.eax = group;
    if (group == 0)
    {
        renderState(app, cpu, 0x1b, 0, false);
        return;
    }
    cpu.ecx = ld(group + 0x18);
    cpu.eax = ld(group + 0xc4);
    cpu.edx = ld(cpu.ecx);
    cpu.eax = bridge::effectSetInt(app, cpu, cpu.ecx, cpu.eax, 0);
}

// The full-screen quad (4 vertices of 0x30 bytes at q: position, colour, 4 texture coordinates) drawn with the
// stream cache cleared; eax = the device, its vtable in ecx (vtableInEdx: edx), the other register = q.
bool quadDraw(App* app, x86::CPU& cpu, reg32 q, bool vtableInEdx)
{
    const reg32 device = deviceRegs(cpu, vtableInEdx);
    (vtableInEdx ? cpu.ecx : cpu.edx) = q;
    st(StreamCache, 0);
    cpu.eax = bridge::drawPrimitiveUP(app, cpu, device, 6, 2, rc::host(q), 0x30);
    return !cpu.terminate;
}

// The queue flush 0x40ed50 with its return address at slot (a recorded callee of the shadow compare).
bool flush(App* app, x86::CPU& cpu, reg32 slot, reg32 back)
{
    const reg32 esp = cpu.esp, ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;
    rc::st32(slot, back);
    cpu.esp = slot;
    if (!sceneshadow::call(0x40ed50, 0, 0, app, cpu, &sub_40ed50))
    {
        cpu.terminate = true;
        return false;
    }
    cpu.esp = esp;
    cpu.ebx = ebx;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.edi = edi;
    return true;
}

// The view's render target, depth surface and whole viewport back (vp: the viewport's stack slot), the view current
// and no effect group. swap: the guest loads the surfaces into ecx and the vtables into edx (else the other way
// round; the viewport pointer goes to the other register than the vtable).
void viewTargets(App* app, x86::CPU& cpu, reg32 view, reg32 vp, bool swap)
{
    reg32 device = deviceRegs(cpu, swap);
    (swap ? cpu.ecx : cpu.edx) = ld(view + 8);
    cpu.eax = bridge::setRenderTarget(app, cpu, device, 0, ld(view + 8));
    device = deviceRegs(cpu, swap);
    (swap ? cpu.ecx : cpu.edx) = ld(view + 0xc);
    cpu.eax = bridge::setDepthStencilSurface(app, cpu, device, ld(view + 0xc));
    const DWORD viewport[6] = { 0, 0, ld(view + 0x14), ld(view + 0x18), 0, 0x3f800000u };
    memcpy(rc::host(vp), viewport, sizeof(viewport));
    device = deviceRegs(cpu, !swap);
    (swap ? cpu.edx : cpu.ecx) = vp;
    cpu.eax = bridge::setViewport(app, cpu, device, viewport);
    st(CurrentView, view);
    st(CurrentGroup, 0);
}

// The quad's corners (-1, 1), (1, 1), (1, -1), (-1, -1) with colour 0xffffffff at q.
void quadCorners(reg32 q)
{
    static const reg32 Corners[4][2] = { {0xbf800000u, 0x3f800000u}, {0x3f800000u, 0x3f800000u},
                                         {0x3f800000u, 0xbf800000u}, {0xbf800000u, 0xbf800000u} };
    for (reg32 v = 0; v < 4; ++v)
    {
        rc::st32(q + 0x30 * v, Corners[v][0]);
        rc::st32(q + 0x30 * v + 4, Corners[v][1]);
        rc::st32(q + 0x30 * v + 8, 0);
        rc::st32(q + 0x30 * v + 0xc, 0xffffffffu);
    }
}

// The texture coordinates (0, 0), (1, 0), (1, 1), (0, 1) of the 4 sets.
void quadCoordinates(reg32 q)
{
    static const reg32 Coordinates[4][2] = { {0, 0}, {0x3f800000u, 0}, {0x3f800000u, 0x3f800000u},
                                             {0, 0x3f800000u} };
    for (reg32 v = 0; v < 4; ++v)
    {
        for (reg32 k = 0; k < 4; ++k)
        {
            rc::st32(q + 0x30 * v + 0x10 + 8 * k, Coordinates[v][0]);
            rc::st32(q + 0x30 * v + 0x14 + 8 * k, Coordinates[v][1]);
        }
    }
}

// Effect group 6 with pass n - 1 (0x40e6d0, edi = the pass), the return address at slot.
bool filterGroup(App* app, x86::CPU& cpu, reg32 count, reg32 slot, reg32 back)
{
    cpu.edi = count - 1;
    cpu.eax = FilterGroup;
    const reg32 esp = cpu.esp;
    if (!callAt(app, cpu, slot, back, &sub_40e6d0))
        return false;
    cpu.esp = esp;
    return true;
}

// 0x40d6e0: eax = the render target (0: keep the current one), [esp + 4] = the texture count n (0-4), [esp + 8] =
// the textures, [esp + 0xc] = a colour per texture (4 bytes), [esp + 0x10] = blending, [esp + 0x14] / [esp + 0x18] =
// the blend arguments 0x410550 takes in edi / esi. cdecl. A full-screen quad through the filter effect with each
// texture weighted by its colour, then the current view's targets back.
void filterPass(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, A = E - 0x128, B = A - 8;
    const reg32 count = ld(E + 4), textures = ld(E + 8), colours = ld(E + 0xc);
    if (count > 4)
    {
        sub_40d6e0_guest(app, cpu);
        return;
    }
    cpu.ebp = 0;
    if (entry.eax != 0)
    {
        cpu.ecx = ld(DevicePointer);
        cpu.edx = ld(cpu.ecx);
        cpu.eax = bridge::setRenderTarget(app, cpu, cpu.ecx, 0, entry.eax);
    }
    const reg32 q = A + 0x28;
    quadCorners(q);
    quadCoordinates(q);
    cpu.eax = 4;
    cpu.ecx = 0x3f800000u;
    cpu.esi = count;
    cpu.esp = B;
    if (!filterGroup(app, cpu, count, B - 4, 0x0040d7d9))
        return;
    cpu.ebx = 0;
    reg32 vectors[16] = {};
    const double scale = ldf(ByteScale);
    for (reg32 k = 0; k < count; ++k)
    {
        filterTexture(app, cpu, k, ld(textures + 4 * k), false);
        const reg32 c = colours + 4 * k;
        for (reg32 j = 0; j < 4; ++j)
            vectors[4 * k + j] = bits(float(double(rc::ld8(c + j)) * scale));
        cpu.ecx = rc::ld8(c + 1);
        cpu.edx = rc::ld8(c + 2);
        cpu.eax = count;
        cpu.ebx = k + 1;
    }
    for (reg32 k = count; k < 4; ++k)
        filterTexture(app, cpu, k, 0, false);
    // 0x410550: [esp + 4] = blending, esi, edi.
    cpu.esi = ld(E + 0x18);
    cpu.edi = ld(E + 0x14);
    cpu.eax = ld(E + 0x10);
    rc::st32(B - 4, cpu.eax);
    if (!callAt(app, cpu, B - 8, 0x0040d8fd, &sub_410550))
        return;
    cpu.esp = A;
    cpu.ebx = entry.ebx;
    cpu.edi = entry.edi;
    cpu.esi = ld(E + 0x18);
    // Alpha test and reference off.
    reg32 group = ld(CurrentGroup);
    cpu.eax = group;
    if (group == 0)
    {
        renderState(app, cpu, 0xf, 0, false);
    }
    else
    {
        cpu.ecx = ld(group + 0x18);
        cpu.eax = ld(group + 0xbc);
        cpu.edx = ld(cpu.ecx);
        cpu.eax = bridge::effectSetInt(app, cpu, cpu.ecx, cpu.eax, 0);
    }
    group = ld(CurrentGroup);
    cpu.ecx = group;
    st(AlphaTestOn, 0);
    if (group == 0)
    {
        renderState(app, cpu, 0x18, 0, false);
    }
    else
    {
        cpu.eax = ld(group + 0x18);
        cpu.ecx = ld(group + 0xc0);
        cpu.edx = ld(cpu.eax);
        cpu.eax = bridge::effectSetInt(app, cpu, cpu.eax, cpu.ecx, 0);
    }
    st(AlphaRef, 0);
    filterVectors(app, cpu, vectors, true);
    if (!quadDraw(app, cpu, q, true))
        return;
    const reg32 view = ld(CurrentView);
    cpu.esi = view;
    if (!flush(app, cpu, A - 4, 0x0040d9b0))
        return;
    viewTargets(app, cpu, view, A + 0x10, true);
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// 0x40d090: ecx = the render target, eax = 4 floats (the coordinate steps), [esp + 4] = the texture, [esp + 8] = the
// copy count n (0-4), [esp + 0xc] = the weight vector. cdecl. The texture n times with coordinates stepped by k times
// the steps, each weighted by the vector, into the target; then the current view's targets back.
void blurPass(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, A = E - 0x14c;
    const reg32 target = entry.ecx, steps = entry.eax;
    const reg32 texture = ld(E + 4), count = ld(E + 8), weights = ld(E + 0xc);
    DWORD desc[8];
    HRESULT h = 0;
    if (count > 4 || !bridge::surfaceDesc(app, cpu, target, desc, h))
    {
        sub_40d090_guest(app, cpu);
        return;
    }
    memcpy(rc::host(A + 0x12c), desc, sizeof(desc));
    cpu.ecx = A + 0x12c;
    cpu.esi = target;
    cpu.edi = steps;
    cpu.ebx = weights;
    cpu.ebp = 0;
    cpu.esp = A;
    const reg32 device = deviceRegs(cpu, true);
    cpu.eax = bridge::setRenderTarget(app, cpu, device, 0, target);
    const reg32 q = A + 0x2c;
    quadCorners(q);
    const double o0 = ldf(steps), o1 = ldf(steps + 4), o2 = ldf(steps + 8), o3 = ldf(steps + 0xc);
    const double one = ldf(One);
    for (reg32 k = 0; k < 4; ++k)
    {
        const double kd = double(sreg32(k));
        const reg32 u0 = bits(float(-(kd * o0))), v0 = bits(float(-(kd * o1)));
        const reg32 u1 = bits(float(one - kd * o2)), v1 = bits(float(one - kd * o3));
        rc::st32(q + 0x10 + 8 * k, u0);
        rc::st32(q + 0x14 + 8 * k, v0);
        rc::st32(q + 0x40 + 8 * k, u1);
        rc::st32(q + 0x44 + 8 * k, v0);
        rc::st32(q + 0x70 + 8 * k, u1);
        rc::st32(q + 0x74 + 8 * k, v1);
        rc::st32(q + 0xa0 + 8 * k, u0);
        rc::st32(q + 0xa4 + 8 * k, v1);
    }
    rc::st32(A + 0x10, 4);
    cpu.eax = 4;
    cpu.esi = 0xffffffffu;
    if (!filterGroup(app, cpu, count, A - 4, 0x0040d1c1))
        return;
    reg32 vectors[16] = {};
    for (reg32 k = 0; k < count; ++k)
    {
        filterTexture(app, cpu, k, texture, false);
        for (reg32 j = 0; j < 4; ++j)
            vectors[4 * k + j] = ld(weights + 4 * j);
        cpu.eax = ld(weights + 0xc);
        cpu.ecx = ld(weights + 4);
        cpu.edx = ld(weights + 8);
    }
    cpu.eax = count;
    for (reg32 k = count; k < 4; ++k)
        filterTexture(app, cpu, k, 0, false);
    cpu.esi = 0xffffffffu;
    filterVectors(app, cpu, vectors, false);
    blendReset(app, cpu);
    st(BlendOn, 0);
    if (!quadDraw(app, cpu, q, false))
        return;
    const reg32 view = ld(CurrentView);
    cpu.esi = view;
    if (!flush(app, cpu, A - 4, 0x0040d2f6))
        return;
    viewTargets(app, cpu, view, A + 0x14, false);
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// 0x40d370: eax = the render target, [esp + 4] = the texture, [esp + 8] = the copy count n (0-4), [esp + 0xc] = n
// triples (weight, x and y offset in texels). cdecl. The texture n times, each moved by its offset and weighted, into
// the target; then the current view's targets back.
void blurOffsets(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, A = E - 0x174;
    const reg32 target = entry.eax;
    const reg32 texture = ld(E + 4), count = ld(E + 8), offsets = ld(E + 0xc);
    DWORD desc[8], level[8];
    HRESULT h = 0, hl = 0;
    if (count > 4 || !bridge::surfaceDesc(app, cpu, target, desc, h)
        || !bridge::textureLevelDesc(app, cpu, texture, 0, level, hl))
    {
        sub_40d370_guest(app, cpu);
        return;
    }
    memcpy(rc::host(A + 0x154), desc, sizeof(desc));
    cpu.esp = A;
    const reg32 device = deviceRegs(cpu, true);
    cpu.esi = 0;
    cpu.eax = bridge::setRenderTarget(app, cpu, device, 0, target);
    cpu.ebp = 0xffffffffu;
    const reg32 q = A + 0x34;
    quadCorners(q);
    quadCoordinates(q);
    cpu.eax = 4;
    cpu.ecx = 0x3f800000u;
    cpu.ebx = count;
    if (!filterGroup(app, cpu, count, A - 4, 0x0040d466))
        return;
    // GetLevelDesc(0) of the texture: the texel size.
    memcpy(rc::host(A + 0xf4), level, sizeof(level));
    cpu.eax = reg32(hl);
    cpu.ecx = ld(texture);
    cpu.edx = A + 0xf4;
    const float du = float(ldf(One) / double(level[6])), dv = float(ldf(One) / double(level[7]));
    rc::st32(A + 0x10, bits(du));
    rc::st32(A + 0x14, bits(dv));
    reg32 vectors[16] = {};
    for (reg32 k = 0; k < count; ++k)
    {
        filterTexture(app, cpu, k, texture, true);
        const reg32 o = offsets + 0xc * k;
        const double x = double(du) * ldf(o + 4), y = double(dv) * ldf(o + 8);
        const reg32 w = ld(o);
        for (reg32 j = 0; j < 4; ++j)
            vectors[4 * k + j] = w;
        for (reg32 v = 0; v < 4; ++v)
        {
            const reg32 c = q + 0x30 * v + 0x10 + 8 * k;
            rc::st32(c, bits(float(x + ldf(c))));
            rc::st32(c + 4, bits(float(y + ldf(c + 4))));
        }
        cpu.eax = count;
        cpu.ecx = w;
        cpu.edx = w;
    }
    cpu.esi = 0;
    cpu.ebp = 0xffffffffu;
    cpu.ebx = count;
    for (reg32 k = count; k < 4; ++k)
        filterTexture(app, cpu, k, 0, false);
    filterVectors(app, cpu, vectors, false);
    blendReset(app, cpu);
    st(BlendOn, 0);
    if (!quadDraw(app, cpu, q, false))
        return;
    const reg32 view = ld(CurrentView);
    cpu.edi = view;
    if (!flush(app, cpu, A - 4, 0x0040d669))
        return;
    viewTargets(app, cpu, view, A + 0x1c, false);
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// ---- 0x40cd40: motion blur ----

// The blur level eased towards [0x73654c], the camera's velocity in view space as the blur directions, then the blur
// passes 0x40d370 and 0x40d090 (twice) into the blur targets and the filter pass 0x40d6e0 of the result with the
// blur strength as its blend factor.
void motionBlur(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp;
    const reg32 S = (((E - 4) & ~15u) - 0x3c) - 4;
    const double target = double(sreg32(ld(0x73654c)));
    const reg32 camera = ld(0x740500);
    const double f = ldf(BlurLevel);
    const double gap = fabs(f - target);
    double limit;
    memcpy(&limit, rc::host(0x6ccd00), 8);
    cpu.fpu.compare(gap, limit);
    if (gap > limit)
    {
        cpu.fpu.compare(f, target);
        if (f < target)
            stf(BlurLevel, float(ldf(0x702a48) + ldf(BlurLevel)));
        else
            stf(BlurLevel, float(ldf(BlurLevel) - ldf(0x702a4c)));
    }
    // The velocity (+0x1e0) and its length; the velocity in view space (D3DXVec4Transform with the camera matrix).
    const double c0 = ldf(camera + 0x1e0), c1 = ldf(camera + 0x1e4), c2 = ldf(camera + 0x1e8);
    rc::st32(S + 0x30, ld(camera + 0x1e0));
    rc::st32(S + 0x34, ld(camera + 0x1e4));
    rc::st32(S + 0x38, ld(camera + 0x1e8));
    rc::st32(S + 0x3c, 0);
    rc::st32(S + 4, bits(float(sqrt(c0 * c0 + c1 * c1 + c2 * c2))));
    rc::st32(S - 4, camera);
    rc::st32(S - 8, S + 0x30);
    rc::st32(S - 0xc, S + 0x30);
    cpu.eax = ld(camera + 0x1e8);
    cpu.ecx = S + 0x30;
    cpu.edx = S + 0x30;
    cpu.ebp = E - 4;
    if (!callAt(app, cpu, S - 0x10, 0x0040ce0c, &sub_5ac443))
        return;
    const double length = ldf(S + 4);
    cpu.fpu.compare(length, ldf(0x6ccb7c));
    if (length > ldf(0x6ccb7c))
    {
        cpu.ecx = S + 0x30;
        cpu.edx = S + 0x20;
        if (!callAt(app, cpu, S - 4, 0x0040ce2a, &sub_567450))
            return;
        const double n0 = ldf(S + 0x20), n1 = ldf(S + 0x24), n2 = ldf(S + 0x28);
        rc::st32(S + 0x10, bits(float(n2 + n0)));
        rc::st32(S + 0x14, bits(float(n1 + n2)));
        rc::st32(S + 0x18, bits(float(n0 - n2)));
        rc::st32(S + 0x1c, bits(float(n1 - n2)));
    }
    else
    {
        cpu.eax = 0;
        for (reg32 i = 0; i < 4; ++i)
            rc::st32(S + 0x10 + 4 * i, 0);
    }
    // The strength: the length between [0x702a38] and [0x702a3c] as 0..1, plus the scaled level, at most [0x702a40].
    cpu.ecx = ld(0x702a38);
    cpu.edx = ld(0x702a3c);
    rc::st32(S + 8, cpu.ecx);
    rc::st32(S + 0xc, cpu.edx);
    const double low = ldf(0x702a38), high = ldf(0x702a3c);
    cpu.fpu.compare(length, low);
    double t;
    if (length < low || length == low)
    {
        t = ldf(Zero);
    }
    else
    {
        cpu.fpu.compare(length, high);
        t = !(length >= high) ? (length - low) / (high - low) : ldf(One);
    }
    double scale;
    memcpy(&scale, rc::host(0x6cccf8), 8);
    const double v = t + ldf(BlurLevel) * scale;
    rc::st32(S + 4, bits(float(v)));
    cpu.fpu.compare(v, ldf(0x702a40));
    if (v > ldf(0x702a40))
    {
        cpu.eax = ld(0x702a40);
        rc::st32(S + 4, cpu.eax);
    }
    const double a = ldf(S + 4);
    cpu.fpu.compare(a, ldf(ByteScale));
    if (a < ldf(ByteScale))
    {
        cpu.eax = (cpu.eax & 0xffff0000u) | cpu.fpu.status.word;
        cpu.esi = entry.esi;
        cpu.ebp = entry.ebp;
        cpu.esp = E + 4;
        return;
    }
    // The directions scaled by the strength.
    const double g = ((ldf(BlurLevel) * ldf(0x6cc7dc) + ldf(One)) * ldf(0x702a44)) * a;
    const double k = ldf(0x6ccc64);
    for (reg32 i = 0; i < 3; ++i)
        rc::st32(S + 0x10 + 4 * i, bits(float(ldf(S + 0x10 + 4 * i) * g * k)));
    rc::st32(S + 0x1c, bits(float(g * ldf(S + 0x1c) * k)));
    rc::st32(S + 0xc, 0xffffffffu);
    // 0x40d370 (eax = [0x71aba8]; [0x736460], 4, the offsets 0x7024b8).
    rc::st32(S - 4, 0x7024b8);
    rc::st32(S - 8, 4);
    rc::st32(S - 0xc, ld(0x736460));
    cpu.eax = ld(0x71aba8);
    cpu.ecx = ld(0x736460);
    if (!callAt(app, cpu, S - 0x10, 0x0040cf79, &blurOffsets))
        return;
    // 0x40d090 (ecx = [0x71abac], eax = the directions; [0x71ab9c], 4, weights of 0.25).
    for (reg32 i = 0; i < 4; ++i)
        rc::st32(S + 0x20 + 4 * i, 0x3e800000u);
    rc::st32(S - 0x10, S + 0x20);
    rc::st32(S - 0x14, 4);
    rc::st32(S - 0x18, ld(0x71ab9c));
    cpu.eax = S + 0x10;
    cpu.ecx = ld(0x71abac);
    cpu.edx = S + 0x20;
    if (!callAt(app, cpu, S - 0x1c, 0x0040cfb5, &blurPass))
        return;
    // Again with the directions scaled by [0x6cc9d8], into [0x71aba8] from [0x71aba0].
    double factor;
    memcpy(&factor, rc::host(0x6cc9d8), 8);
    for (reg32 i = 0; i < 4; ++i)
        rc::st32(S + 0x10 + 4 * i, bits(float(ldf(S + 0x10 + 4 * i) * factor)));
    rc::st32(S - 0x1c, S + 0x20);
    rc::st32(S - 0x20, 4);
    rc::st32(S - 0x24, ld(0x71aba0));
    cpu.eax = S + 0x10;
    cpu.ecx = ld(0x71aba8);
    cpu.edx = ld(0x71aba0);
    if (!callAt(app, cpu, S - 0x28, 0x0040d00a, &blurPass))
        return;
    // The blend factor: the strength times [0x6ccc0c], at most 1, as a grey level in all four bytes.
    double b = ldf(S + 4) * ldf(0x6ccc0c);
    cpu.fpu.compare(b, ldf(One));
    if (b > ldf(One))
        b = ldf(One);
    b = b * ldf(0x6cc974);
    cpu.edx_eax = x86::reg64(x86::truncInteger64(b));
    const reg32 n = cpu.eax;
    const reg32 grey = (((((n << 8) | n) << 8) | n) << 8) | n;
    cpu.esi = grey;
    cpu.ecx = ld(DevicePointer);
    cpu.edx = ld(cpu.ecx);
    cpu.eax = bridge::setRenderState(app, cpu, cpu.ecx, 0xc1, grey);
    // 0x40d6e0 (eax = [0x736464]; 1, the texture 0x71ab9c, colour 0xffffffff, blending, 0xe, 0xf).
    rc::st32(S - 4, 0xf);
    rc::st32(S - 8, 0xe);
    rc::st32(S - 0xc, 1);
    rc::st32(S - 0x10, S + 0xc);
    rc::st32(S - 0x14, 0x71ab9c);
    rc::st32(S - 0x18, 1);
    cpu.eax = ld(0x736464);
    if (!callAt(app, cpu, S - 0x1c, 0x0040d079, &filterPass))
        return;
    cpu.esi = entry.esi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// ---- 0x409b70: screen pass of the player view's flares ----

// 0x40abd0: esi = the view, the clear flag at slot + 4: the queue flushed, the view's targets, a clear when asked,
// its viewport; the view current with no effect group.
bool viewBegin(App* app, x86::CPU& cpu, reg32 slot)
{
    const reg32 view = cpu.esi, l = slot - 0x18;
    cpu.esp = l;
    if (!flush(app, cpu, l - 4, 0x0040abd8))
        return false;
    reg32 device = ld(DevicePointer);
    cpu.edx = ld(view + 8);
    cpu.ecx = ld(device);
    cpu.eax = bridge::setRenderTarget(app, cpu, device, 0, cpu.edx);
    device = ld(DevicePointer);
    cpu.edx = ld(view + 0xc);
    cpu.ecx = ld(device);
    cpu.eax = bridge::setDepthStencilSurface(app, cpu, device, cpu.edx);
    cpu.eax = ld(slot + 4);
    if (cpu.eax != 0)
    {
        device = ld(DevicePointer);
        cpu.ecx = ld(device);
        cpu.eax = bridge::clear(app, cpu, device, 0, nullptr, 7, 0xff000000u, 1.0f, 0);
    }
    const DWORD viewport[6] = { 0, 0, ld(view + 0x14), ld(view + 0x18), 0, 0x3f800000u };
    memcpy(rc::host(l), viewport, sizeof(viewport));
    cpu.edx = l;
    device = ld(DevicePointer);
    cpu.ecx = ld(device);
    cpu.eax = bridge::setViewport(app, cpu, device, viewport);
    st(CurrentView, view);
    st(CurrentGroup, 0);
    cpu.esp = slot + 4;
    return true;
}

// A recorded callee of the shadow compare with its return address at slot; the callee-saved registers as the x86
// calling convention leaves them.
bool recorded(App* app, x86::CPU& cpu, reg32 address, uint32_t arguments, uint8_t registers, Function f, reg32 slot,
              reg32 back)
{
    const reg32 ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;
    rc::st32(slot, back);
    cpu.esp = slot;
    if (!sceneshadow::call(address, arguments, registers, app, cpu, f))
    {
        cpu.terminate = true;
        return false;
    }
    cpu.ebx = ebx;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.edi = edi;
    return true;
}

// The name hash of "WHITE16X16_NOALPHA" as the guest computes it (its first letter is an immediate); p ends at the
// terminator.
reg32 whiteHash(reg32& p)
{
    reg32 hash = 0xffffffffu, c = 0x57;
    p = WhiteName;
    do
    {
        hash = hash * 0x21 + c;
        c = rc::ld8(p + 1);
        ++p;
    } while (c != 0);
    return hash;
}

// With [0x73645c] = 1: the player view's light flares drawn into the flare target [0x71abb0] over a filter pass of
// the white texture, then the player view (0x71aa4c) begun again and the flare target added by a filter pass.
void flareScreen(App* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 E = cpu.esp, S = E - 8;
    cpu.eax = ld(0x73645c);
    if (cpu.eax != 1)
    {
        cpu.esp = E + 4;
        return;
    }
    reg32 device = ld(DevicePointer);
    cpu.edx = ld(0x71abb0);
    cpu.eax = device;
    cpu.ecx = ld(device);
    cpu.eax = bridge::setRenderTarget(app, cpu, device, 0, cpu.edx);
    // The texture "WHITE16X16_NOALPHA" (0x5461c0 of its hash).
    reg32 p = 0;
    const reg32 hash = whiteHash(p);
    rc::st32(S - 4, entry.esi);
    rc::st32(S - 8, entry.edi);
    rc::st32(S - 0x18, 0x00409bbe);
    reg32 info = 0, ecx = 0, edx = p;
    groups::lookup(app, cpu, hash, S - 0x18, info, ecx, edx);
    if (cpu.terminate)
        return;
    cpu.ecx = ecx;
    cpu.edx = edx;
    const reg32 white = ld(info);
    cpu.esi = white;
    device = deviceRegs(cpu, true);
    cpu.eax = bridge::setRenderState(app, cpu, device, 0xc1, 0x8296b4);
    // 0x40d6e0 (eax = 0; 1, the white texture, colour 0x1fffffff, blending, 1, 0xe).
    rc::st32(S, 0x1fffffffu);
    rc::st32(S + 4, ld(white + 0x18));
    rc::st32(S - 0xc, 0xe);
    rc::st32(S - 0x10, 1);
    rc::st32(S - 0x14, 1);
    rc::st32(S - 0x18, S);
    rc::st32(S - 0x1c, S + 4);
    rc::st32(S - 0x20, 1);
    cpu.eax = 0;
    cpu.ecx = S;
    cpu.edx = S + 4;
    if (!callAt(app, cpu, S - 0x24, 0x00409c03, &filterPass))
        return;
    device = ld(DevicePointer);
    cpu.edx = ld(0x71abb0);
    cpu.eax = device;
    cpu.ecx = ld(device);
    cpu.eax = bridge::setRenderTarget(app, cpu, device, 0, cpu.edx);
    cpu.edi = 0;
    cpu.eax = Groups;
    if (!callAt(app, cpu, S - 0xc, 0x00409c29, &sub_40e6d0))
        return;
    renderState(app, cpu, 7, 1, false);
    renderState(app, cpu, 0xe, 0, true);
    // The flares of the player view.
    const reg32 view = 0x7404c0;
    rc::st32(S - 0xc, 0);
    rc::st32(S - 0x10, view);
    if (!recorded(app, cpu, 0x544f30, 2, 0, &sub_544f30, S - 0x14, 0x00409c57))
        return;
    rc::st32(S - 0xc, view);
    rc::st32(S - 0x10, 0x746dd0);
    if (!recorded(app, cpu, 0x4ae7d0, 2, 0, &sub_4ae7d0, S - 0x14, 0x00409c69))
        return;
    rc::st32(S - 0xc, 0);
    cpu.eax = view;
    if (!recorded(app, cpu, 0x570d00, 1, retstate::Eax, &sub_570d00, S - 0x10, 0x00409c74))
        return;
    rc::st32(S - 0x10, 0);
    cpu.edi = view;
    if (!recorded(app, cpu, 0x570c70, 1, retstate::Edi, &sub_570c70, S - 0x14, 0x00409c7f))
        return;
    renderState(app, cpu, 0xe, 1, false);
    // The player view again (no clear), then the flare target added.
    rc::st32(S - 0xc, 0);
    cpu.esi = 0x71aa4c;
    if (!viewBegin(app, cpu, S - 0x10))
        return;
    rc::st32(S - 0x10, 2);
    rc::st32(S - 0x14, 2);
    rc::st32(S - 0x18, 1);
    rc::st32(S - 0x1c, S);
    rc::st32(S - 0x20, 0x71aba4);
    rc::st32(S - 0x24, 1);
    cpu.eax = 0;
    cpu.edx = S;
    if (!callAt(app, cpu, S - 0x28, 0x00409cb9, &filterPass))
        return;
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = E + 4;
}

// ---- dispatch and shadow compare ----

enum Id
{
    IdCull, IdFog, IdScreen, IdMirror, IdFilter, IdBlurPass, IdBlurOffsets, IdMotionBlur, IdFlareScreen, IdCount
};

struct Port
{
    const char* name;
    reg32 address;
    Function guest;
    Function native;
};

const Port Ports[IdCount] = {
    { "culler setup 0x408f00", 0x408f00, &sub_408f00_guest, &cullSetup },
    { "view fog 0x4090a0", 0x4090a0, &sub_4090a0_guest, &viewFog },
    { "screen states 0x408b20", 0x408b20, &sub_408b20_guest, &screenStates },
    { "mirror quad 0x409180", 0x409180, &sub_409180_guest, &mirrorQuad },
    { "filter pass 0x40d6e0", 0x40d6e0, &sub_40d6e0_guest, &filterPass },
    { "blur pass 0x40d090", 0x40d090, &sub_40d090_guest, &blurPass },
    { "blur offsets 0x40d370", 0x40d370, &sub_40d370_guest, &blurOffsets },
    { "motion blur 0x40cd40", 0x40cd40, &sub_40cd40_guest, &motionBlur },
    { "flare screen pass 0x409b70", 0x409b70, &sub_409b70_guest, &flareScreen },
};

sceneshadow::Stats s_shadow[IdCount];
std::vector<Span> s_spans;

// The render-state words the device and effect paths store to.
void stateSpans(std::vector<Span>& out)
{
    out.push_back(Span{AlphaRef, 20});
    out.push_back(Span{CurrentGroup, 4});
    out.push_back(Span{LastView, 4});
    out.push_back(Span{LastGroup, 8});
    out.push_back(Span{WorldCalls, 16});
    out.push_back(Span{StreamCache, 4});
    out.push_back(Span{LastModel, 4});
    out.push_back(Span{LastModelGroup, 4});
    out.push_back(Span{CurrentView, 4});
}

// Guest memory each port may store to, besides its stack frame.
void spans(Id id, const x86::CPU& cpu, std::vector<Span>& out)
{
    out.clear();
    switch (id)
    {
    case IdCull:
        // The count and the view and flags words of the ten entries; the culler's own stores are its own.
        out.push_back(Span{cpu.eax + 0x820, 4});
        for (reg32 i = 0; i < 10; ++i)
            out.push_back(Span{cpu.eax + 0xd0 * i + 0x80, 8});
        break;
    case IdFog:
        out.push_back(Span{FogVolume, FogEntries + 8});
        out.push_back(Span{FogChosen, 4});
        out.push_back(Span{cpu.esi, 0x1c});
        break;
    case IdScreen:
        stateSpans(out);
        break;
    case IdMirror:
    {
        stateSpans(out);
        const reg32 record = ld(ld(CurrentView) + 4) * 0x60 + 0x740460;
        out.push_back(Span{ld(record), 0xc0});
        break;
    }
    case IdFilter:
    case IdBlurPass:
    case IdBlurOffsets:
        stateSpans(out);
        break;
    case IdMotionBlur:
        stateSpans(out);
        out.push_back(Span{BlurLevel, 4});
        break;
    case IdFlareScreen:
    {
        stateSpans(out);
        reg32 p = 0;
        groups::textureSpans(whiteHash(p), out);
        break;
    }
    default:
        break;
    }
}

void run(Id id, App* app, x86::CPU& cpu)
{
    const Port& p = Ports[id];
    if (!settings().native || sceneshadow::comparing() || bridge::capturing())
    {
        p.guest(app, cpu);
        return;
    }
    if (__builtin_expect(settings().shadow, 0))
    {
        spans(id, cpu, s_spans);
        sceneshadow::compare("shadow_view_parts", p.name, p.address, Saved | retstate::Eax | retstate::Ecx |
                             retstate::Edx, app, cpu, p.guest, p.native, s_spans, s_shadow[id]);
        return;
    }
    p.native(app, cpu);
}

}

void logShadowTotal()
{
    if (!settings().native || !settings().shadow)
        return;
    uint32_t calls = 0, differing = 0;
    for (uint32_t i = 0; i < IdCount; ++i)
    {
        calls += s_shadow[i].calls;
        differing += s_shadow[i].differing;
        if (s_shadow[i].calls)
            pf::logInfo("shadow_view_parts: %s: %u calls compared, %u differ", Ports[i].name,
                        unsigned(s_shadow[i].calls), unsigned(s_shadow[i].differing));
    }
    pf::logInfo("shadow_view_parts: %u calls compared, %u differ", unsigned(calls), unsigned(differing));
}

}

namespace native
{

void sub_408f00(win32::WinApplication* app, x86::CPU& cpu)
{
    viewparts::run(viewparts::IdCull, app, cpu);
}

void sub_4090a0(win32::WinApplication* app, x86::CPU& cpu)
{
    viewparts::run(viewparts::IdFog, app, cpu);
}

void sub_408b20(win32::WinApplication* app, x86::CPU& cpu)
{
    viewparts::run(viewparts::IdScreen, app, cpu);
}

void sub_409180(win32::WinApplication* app, x86::CPU& cpu)
{
    viewparts::run(viewparts::IdMirror, app, cpu);
}

void sub_40d6e0(win32::WinApplication* app, x86::CPU& cpu)
{
    viewparts::run(viewparts::IdFilter, app, cpu);
}

void sub_40d090(win32::WinApplication* app, x86::CPU& cpu)
{
    viewparts::run(viewparts::IdBlurPass, app, cpu);
}

void sub_40d370(win32::WinApplication* app, x86::CPU& cpu)
{
    viewparts::run(viewparts::IdBlurOffsets, app, cpu);
}

void sub_40cd40(win32::WinApplication* app, x86::CPU& cpu)
{
    viewparts::run(viewparts::IdMotionBlur, app, cpu);
}

void sub_409b70(win32::WinApplication* app, x86::CPU& cpu)
{
    viewparts::run(viewparts::IdFlareScreen, app, cpu);
}

}
}
