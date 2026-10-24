// Native math helpers (mathfn.h). Each override sets the registers, flags, x87 depth, status word and result slot
// the translated guest function sets at its return, and its stores at or above the entry stack pointer.

#include "mathfn.h"
#include "retstate.h"

#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <recomp.h>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_564b10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_564bb0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_564bd0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_564cd0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_564db0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_583ea0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_583f20_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_4387e0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_567370_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_567450_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_567310_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_540830_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5677d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_567740_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_567710_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace mathfn
{

namespace
{

using x86::reg8;
using x86::reg16;
using x86::reg32;
using x86::reg64;
using x86::sreg32;
using x86::sreg64;
using x86::parity8;

// Float constants in .rdata.
const reg32 Zero = 0x6cc7a4;
const reg32 One = 0x6cc7bc;
const reg32 Half = 0x6cc7dc;
const reg32 MinusOne = 0x6cc8a8;
const reg32 AngleToRadians = 0x6cc980;  // 2 pi / 65536
const reg32 RadiansToAngle = 0x6cca4c;
const reg32 HalfPi = 0x6cca40;
const reg32 Pi = 0x6cca44;
const reg32 TwoPi = 0x6cca48;
const reg32 ThreeHalfPi = 0x6cca90;
const reg32 SinTerms = 0x6cca80;        // x^9, x^7, x^5, x^3 coefficients at +0, +4, +8, +0xc
const reg32 CosTerms = 0x6cca70;        // x^8 at +0, x^6 at +4, x^4 at +8, x^6 / x^4 parts at +0xc
const reg32 Fixed16 = 0x6cc840;         // 65536
const reg32 InverseFixed16 = 0x6cc83c;
const reg32 MinusFixed16 = 0x6cc838;
const reg32 AsinTable = 0x6f2280;       // 8-byte entries: 16-bit angle, float slope at +4
const reg32 AtanEpsilon = 0x6ccab0;
const reg32 GraphEpsilon = 0x6ccae0;

inline double ld(reg32 address) { return double(rc::ldf32(address)); }
inline reg32 bits(float value) { reg32 r; memcpy(&r, &value, 4); return r; }
inline bool pf(reg32 value) { return !parity8(reg8(value)); }

// _ftol2 as the game's native override computes it.
inline reg64 ftol(double value)
{
    return reg64(x86::fistInteger<sreg64>(std::trunc(value)));
}

// The status bits and ax after fcom a, b; fnstsw ax.
inline void compare(x86::CPU& cpu, reg32& eax, double a, double b)
{
    cpu.fpu.compare(a, b);
    eax = (eax & 0xffff0000u) | cpu.fpu.status.word;
}

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("math_native", true), pf::Config::getBool("shadow_math", false) };
        pf::logInfo("mathfn: math helpers %s%s", r.native ? "native" : "guest", r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// ---- trigonometry by 16-bit angle (65536 per turn) ----

// The angle in radians folded to [-pi/2, pi/2] with its sign (guest 0x564b10 and 0x564bd0 share the folding).
struct Folded
{
    double x;
    bool negative;
    double a, b;        // the last compare
};

inline Folded fold(reg32 angle)
{
    Folded f;
    f.x = double(sreg32(reg16(angle))) * ld(AngleToRadians);
    f.negative = false;
    f.a = f.x;
    f.b = ld(ThreeHalfPi);
    if (f.a >= f.b)
    {
        f.x -= ld(TwoPi);
        return f;
    }
    f.b = ld(HalfPi);
    if (f.a >= f.b)
    {
        f.x -= ld(Pi);
        f.negative = true;
    }
    return f;
}

inline double sinPoly(double x, double sign)
{
    const double x2 = x * x;
    const double x3 = x2 * x;
    const double x5 = x3 * x2;
    const double x7 = x5 * x2;
    double p = x - x3 * ld(SinTerms + 0xc);
    p += x5 * ld(SinTerms + 8);
    p -= x7 * ld(SinTerms + 4);
    p += x7 * x2 * ld(SinTerms);
    return p * sign;
}

// Guest 0x564b10: sine of the angle in ax, in st0. ecx gets the dword the guest pops: the zero-extended angle.
void sinNative(x86::CPU& cpu)
{
    const reg32 angle = cpu.eax & 0xffffu;
    const Folded f = fold(angle);
    cpu.fpu.compare(f.a, f.b);
    const double sign = f.negative ? ld(MinusOne) : ld(One);
    const double r = sinPoly(f.x, sign);
    const bool ge = f.a >= f.b;
    cpu.eax = cpu.fpu.status.word;
    cpu.ecx = angle;
    cpu.esp += 4;
    cpu.flags.cf = false;
    cpu.flags.pf = ge;
    cpu.flags.zf = ge;
    const reg32 count = cpu.fpu.count;
    cpu.fpu.regs[(count + 1) & 7] = r;
    cpu.fpu.count = count + 1;
}

// Guest 0x564bb0: sine of a float in radians (stack argument), through the angle.
void sinRadiansNative(x86::CPU& cpu)
{
    const double v = ld(cpu.esp + 4) * ld(RadiansToAngle);
    cpu.fpu.regs[(cpu.fpu.count + 1) & 7] = v;
    cpu.edx_eax = ftol(v);
    sinNative(cpu);
}

struct SinCos
{
    float sine, cosine;
    double a, b;        // the last compare
};

inline SinCos sinCos(reg32 angle)
{
    const Folded f = fold(angle);
    const float sign = f.negative ? -1.0f : 1.0f;
    const double x = f.x;
    const double x2 = x * x;
    const double x3 = x2 * x;
    const float s4 = float(x2 * x2);
    const double x5 = x3 * x2;
    const float s6 = float(double(s4) * x2);
    const double x7 = x5 * x2;
    const float s8 = float(double(s6) * x2);
    double p = x - x3 * ld(SinTerms + 0xc);
    p += x5 * ld(SinTerms + 8);
    p -= x7 * ld(SinTerms + 4);
    p += x7 * x2 * ld(SinTerms);
    SinCos r;
    r.sine = float(p * double(sign));
    double q = ld(One) - ld(Half) * x2;
    q += double(s4) * ld(CosTerms + 0xc);
    q -= double(s6) * ld(CosTerms + 8);
    q += double(s8) * ld(CosTerms + 4);
    q -= double(s8) * x2 * ld(CosTerms);
    r.cosine = float(q * double(sign));
    r.a = f.a;
    r.b = f.b;
    return r;
}

// Guest 0x564bd0: sine to [ecx], cosine to [edx] of the angle in ax.
void sinCosNative(x86::CPU& cpu)
{
    const reg32 angle = cpu.eax & 0xffffu;
    const SinCos r = sinCos(angle);
    rc::stf32(cpu.ecx, r.sine);
    rc::stf32(cpu.edx, r.cosine);
    cpu.fpu.compare(r.a, r.b);
    cpu.eax = cpu.fpu.status.word;
    const reg32 frame = cpu.esp - 0x10;
    cpu.esp += 4;
    cpu.flags.cf = reg32(frame + 0x10) < frame;
    cpu.flags.pf = r.a >= r.b;
    cpu.flags.zf = reg32(frame + 0x10) == 0;
}

// Guest 0x564cd0: arcsine of a float (stack argument) as a 16-bit angle in eax, from the slope table.
void asinNative(x86::CPU& cpu)
{
    const reg32 esp0 = cpu.esp, arg = esp0 + 4;
    const reg32 ecx0 = cpu.ecx, ebx0 = cpu.ebx, esi0 = cpu.esi, edi0 = cpu.edi;
    const reg32 count = cpu.fpu.count;
    reg32 eax = cpu.eax;
    compare(cpu, eax, ld(arg), ld(Zero));
    reg32 ebx = 0;
    if (parity8(reg8((eax >> 8) & 5)))
    {
        rc::stf32(arg, float(-ld(arg)));
        ebx = 1;
    }
    const double g = ld(arg);
    compare(cpu, eax, g, ld(One));
    if (g >= ld(One))
    {
        const reg32 sign = ebx ? 0x8000u : 0;
        cpu.eax = sign + 0x4000;
        cpu.ecx = ecx0;
        cpu.ebx = ebx0;
        cpu.esp = esp0 + 4;
        cpu.flags.pf = pf(sign);
        cpu.fpu.count = count;
        return;
    }
    const double v = ld(arg) * ld(Fixed16);
    cpu.fpu.regs[(count + 1) & 7] = v;
    eax = reg32(ftol(v));
    reg32 edx = 0x8000, ecx = 0x8000, esi = 0;
    if (!(sreg32(eax) < sreg32(edx)))
    {
        while (sreg32(esi) < 11)
        {
            edx = reg32(sreg32(edx) >> 1);
            ecx += edx;
            ++esi;
            if (!(sreg32(eax) >= sreg32(ecx)))
                break;
        }
    }
    ecx -= edx;
    const reg32 edi = ecx;
    ecx = 11 - esi;
    eax -= edi;
    if (const unsigned n = reg8(ecx) & 31)
        eax = reg32(sreg32(eax) >> n);
    edx = reg32(sreg32(edx) >> 4);
    esi <<= 4;
    edx = reg32(sreg64(sreg32(edx)) * sreg64(sreg32(eax)));
    edx += edi;
    esi = (esi + eax) << 3;
    double w = double(sreg32(edx)) * ld(InverseFixed16);
    w = ld(arg) - w;
    w *= ld(esi + AsinTable + 4);
    w *= ld(MinusFixed16);
    cpu.fpu.regs[(count + 1) & 7] = w;
    cpu.edx_eax = ftol(w);
    eax = cpu.eax;
    const reg32 angle = reg16(reg16(rc::ld16(esi + AsinTable)) - reg16(eax));
    cpu.eax = ebx == 0 ? (eax & 0xffff0000u) | angle : 0x10000u - angle;
    cpu.ecx = edx;
    cpu.ebx = ebx0;
    cpu.esp = esp0 + 4;
    cpu.esi = esi0;
    cpu.edi = edi0;
    cpu.flags.pf = pf(ebx);
    cpu.fpu.count = count;
}

// Guest 0x564db0: the 16-bit angle of the vector (x, y) (stack arguments) in ax, through the arcsine. The guest
// stores the arcsine result into the y argument slot.
void atanNative(x86::CPU& cpu)
{
    const reg32 esp0 = cpu.esp, xs = esp0 + 4, ys = esp0 + 8;
    const reg32 esi0 = cpu.esi;
    reg32 eax = cpu.eax;
    compare(cpu, eax, ld(xs), ld(Zero));
    reg32 esi = parity8(reg8((eax >> 8) & 5)) ? 1 : 0;
    const double y0 = ld(ys);
    compare(cpu, eax, y0, ld(Zero));
    if (y0 < ld(Zero))
        esi ^= 3;
    const float ay = float(std::fabs(ld(ys)));
    const float ax = float(std::fabs(ld(xs)));
    cpu.fpu.compare(double(ay), double(ax));
    const reg8 order = reg8((cpu.fpu.status.word >> 8) & 5);
    double sum = ld(xs) * ld(xs);
    sum += ld(ys) * ld(ys);
    eax = (eax & 0xffff0000u) | cpu.fpu.status.word;
    const float sumf = float(sum);
    compare(cpu, eax, sum, ld(AtanEpsilon));
    const bool small = !(sum > ld(AtanEpsilon));
    float t = 0;
    const reg32 slot = esp0 - 0x18;
    if (parity8(order))
    {
        // |y| < |x|: the angle from the y part.
        if (!small)
        {
            const double y = ld(ys);
            cpu.fpu.compare(y, ld(Zero));
            if (!(y == ld(Zero)))
                t = float(double(ay) / std::sqrt(double(sumf)));
        }
        rc::st32(slot, bits(t));
        cpu.eax = bits(t);
        cpu.esp = slot - 4;
        cpu.esi = esi;
        asinNative(cpu);
        rc::st32(ys, cpu.eax);
    }
    else
    {
        if (!small)
        {
            const double x = ld(xs);
            compare(cpu, eax, x, ld(Zero));
            if (!(x == ld(Zero)))
                t = float(double(ax) / std::sqrt(double(sumf)));
        }
        rc::st32(slot, bits(t));
        cpu.eax = eax;
        cpu.ecx = bits(t);
        cpu.esp = slot - 4;
        cpu.esi = esi;
        asinNative(cpu);
        cpu.edx = 0x4000u - cpu.eax;
        rc::st32(ys, cpu.edx);
    }
    const reg32 result = rc::ld32(ys);
    if (esi == 0)
        cpu.eax = (cpu.eax & 0xffff0000u) | reg16(result);
    else if (esi == 3)
    {
        cpu.ecx = result;
        cpu.eax = 0x10000u - result;
    }
    else if (esi == 1)
    {
        cpu.edx = result;
        cpu.eax = 0x8000u - result;
    }
    else
        cpu.eax = result + 0x8000u;
    cpu.esp = esp0 + 4;
    cpu.esi = esi0;
    cpu.flags.pf = pf(esi);
}

// ---- lookups ----

// Guest 0x4387e0: st0 = the float argument truncated to an integer.
void truncateNative(x86::CPU& cpu)
{
    const reg32 count = cpu.fpu.count;
    const double v = ld(cpu.esp + 4);
    cpu.edx_eax = ftol(v);
    cpu.esp += 4;
    cpu.fpu.regs[(count + 1) & 7] = double(sreg32(cpu.eax));
    cpu.fpu.count = count + 1;
}

// Guest 0x583ea0: value of the table at esi for the float argument (entries [esi], minimum [esi+4], index scale
// [esi+0xc], values at [esi+0x10]), linear between entries; stdcall. The guest stores the scaled index into its
// argument slot.
void tableNative(x86::CPU& cpu)
{
    const reg32 esp0 = cpu.esp, arg = esp0 + 4;
    const reg32 table = cpu.esi;
    const reg32 count = cpu.fpu.count;
    const reg32 ebx0 = cpu.ebx, edi0 = cpu.edi;
    double index = ld(arg) - ld(table + 4);
    const reg32 entries = rc::ld32(table);
    index *= ld(table + 0xc);
    rc::stf32(arg, float(index));
    cpu.fpu.regs[(count + 1) & 7] = index;
    cpu.edx_eax = ftol(index);
    const reg32 edi = cpu.eax;
    cpu.ebx = ebx0;
    cpu.edi = edi0;
    cpu.esp = esp0 + 8;
    cpu.fpu.count = count + 1;
    if (sreg32(edi) < 0)
    {
        cpu.eax = rc::ld32(table + 0x10);
        cpu.flags.cf = false;
        cpu.flags.pf = pf(edi);
        cpu.flags.zf = edi == 0;
        cpu.fpu.regs[(count + 1) & 7] = ld(cpu.eax);
        return;
    }
    const reg32 last = entries - 1;
    if (!(sreg32(edi) < sreg32(last)))
    {
        cpu.ecx = last;
        cpu.edx = rc::ld32(table + 0x10);
        cpu.flags.cf = edi < last;
        cpu.flags.pf = pf(edi);
        cpu.flags.zf = edi == last;
        cpu.fpu.regs[(count + 1) & 7] = ld(cpu.edx + entries * 4 - 4);
        return;
    }
    // The fraction: index - its truncation (guest 0x4387e0), one less when the truncation is above the index.
    cpu.edx_eax = ftol(ld(arg));
    const double whole = double(sreg32(cpu.eax));
    cpu.fpu.compare(whole, ld(arg));
    double frac = whole;
    if (whole > ld(arg))
        frac -= ld(One);
    const reg8 test = reg8((cpu.fpu.status.word >> 8) & 0x41);
    frac = ld(arg) - frac;
    const reg32 values = rc::ld32(table + 0x10);
    const reg32 at = values + edi * 4;
    double r = ld(One);
    r -= frac;
    r *= ld(at);
    double s = frac;
    s *= ld(at + 4);
    r += s;
    cpu.eax = at;
    cpu.ecx = values;
    cpu.flags.cf = false;
    cpu.flags.pf = pf(test);
    cpu.flags.zf = test == 0;
    cpu.fpu.regs[(count + 1) & 7] = r;
}

// Guest 0x583f20: value of the graph (stack argument: points pointer, point count; points are float pairs x, y)
// at a float x (stack argument), linear between points and clamped at the ends; stdcall.
void graphNative(x86::CPU& cpu)
{
    const reg32 esp0 = cpu.esp;
    const reg32 graph = rc::ld32(esp0 + 4), xs = esp0 + 8;
    const reg32 count = cpu.fpu.count;
    const reg32 n = rc::ld32(graph + 4);
    reg32 eax = cpu.eax;
    bool cf = n < 1u, pfl = cpu.flags.pf, zf = n == 1;
    cpu.esp = esp0 + 12;
    cpu.fpu.count = count + 1;
    if (!(sreg32(n) <= 1))
    {
        const reg32 points = rc::ld32(graph);
        double a = ld(xs), b = ld(points);
        compare(cpu, eax, a, b);
        if (a <= b)
        {
            cpu.eax = eax;
            cpu.edx = points;
            cpu.flags.cf = false;
            cpu.flags.pf = false;
            cpu.flags.zf = (!(a >= b) || !__builtin_islessgreater(a, b)) == false;
            cpu.fpu.regs[(count + 1) & 7] = ld(points + 4);
            return;
        }
        a = ld(xs);
        b = ld(points + n * 8 - 8);
        compare(cpu, eax, a, b);
        if (a >= b)
        {
            cpu.eax = eax;
            cpu.edx = points;
            cpu.flags.cf = false;
            cpu.flags.pf = true;
            cpu.flags.zf = true;
            cpu.fpu.regs[(count + 1) & 7] = ld(points + n * 8 - 4);
            return;
        }
        const reg32 edi = n - 1;
        reg32 ecx = 0;
        cf = false;
        pfl = pf(edi);
        zf = edi == 0;
        bool found = false;
        for (reg32 p = points; sreg32(edi) > 0;)
        {
            a = ld(xs);
            b = ld(p);
            compare(cpu, eax, a, b);
            pfl = a >= b;
            if (a >= b)
            {
                a = ld(xs);
                b = ld(p + 8);
                compare(cpu, eax, a, b);
                pfl = !(a < b);
                if (a < b)
                {
                    found = true;
                    break;
                }
            }
            ++ecx;
            p += 8;
            cf = ecx < edi;
            zf = ecx == edi;
            if (!(sreg32(ecx) < sreg32(edi)))
                break;
        }
        cpu.ecx = ecx;
        cpu.edx = points;
        if (found)
        {
            // Between points ecx and ecx + 1.
            const reg32 at = points + ecx * 8;
            double dy = ld(at + 0xc) - ld(at + 4);
            const double dx = ld(at + 8) - ld(at);
            const double adx = std::fabs(dx);
            const double e = ld(GraphEpsilon);
            compare(cpu, eax, adx, e);
            double result;
            if (adx > e)
            {
                double f = ld(xs) - ld(at);
                f /= dx;
                dy *= f;
                result = dy + ld(at + 4);
            }
            else
                result = dy * ld(Half) + ld(at + 4);
            cpu.eax = eax;
            cpu.flags.cf = false;
            cpu.flags.pf = !(adx <= e);
            cpu.flags.zf = (!(adx >= e) || !__builtin_islessgreater(adx, e)) == false;
            cpu.fpu.regs[(count + 1) & 7] = result;
            return;
        }
    }
    cpu.eax = rc::ld32(graph);
    cpu.flags.cf = cf;
    cpu.flags.pf = pfl;
    cpu.flags.zf = zf;
    cpu.fpu.regs[(count + 1) & 7] = ld(cpu.eax + 4);
}

// ---- vectors and boxes ----

// Guest 0x567370: [ecx] = the vector at edx normalised; (1, 0, 0) for length 0. eax = ecx; ecx gets the dword the
// guest pops (the source z bits, or ecx).
void normalizeNative(x86::CPU& cpu)
{
    const reg32 dst = cpu.ecx, src = cpu.edx;
    const double z = ld(src + 8), y = ld(src + 4), x = ld(src);
    double len = x * x;
    len += y * y;
    len += z * z;
    len = std::sqrt(len);
    const double zero = ld(Zero);
    cpu.fpu.compare(len, zero);
    cpu.esp += 4;
    cpu.flags.cf = false;
    if (len == zero)
    {
        rc::st32(dst + 4, 0);
        rc::st32(dst + 8, 0);
        rc::st32(dst, 0x3f800000u);
        cpu.eax = dst;
        cpu.flags.pf = true;
        cpu.flags.zf = true;
        return;
    }
    const double inv = ld(One) / len;
    const reg32 zbits = rc::ld32(src + 8);
    const double sy = ld(src + 4);
    rc::stf32(dst, float(inv * ld(src)));
    rc::stf32(dst + 4, float(sy * inv));
    float zf;
    memcpy(&zf, &zbits, 4);
    rc::stf32(dst + 8, float(double(zf) * inv));
    cpu.eax = dst;
    cpu.ecx = zbits;
    cpu.flags.pf = true;
    cpu.flags.zf = (__builtin_isunordered(len, zero) || !__builtin_islessgreater(len, zero)) == false;
}

// Guest 0x567450: [edx] = the vector at ecx scaled by 1 / sqrt(x^2 + y^2 + z^2 + 2 w); (1, 0, 0) for 0.
void normalize4Native(x86::CPU& cpu)
{
    const reg32 src = cpu.ecx, dst = cpu.edx;
    const double z = ld(src + 8), y = ld(src + 4), x = ld(src);
    double len = x * x;
    len += y * y;
    len += z * z;
    len += ld(src + 0xc);
    len += ld(src + 0xc);
    len = std::sqrt(len);
    const double zero = ld(Zero);
    cpu.fpu.compare(len, zero);
    cpu.esp += 4;
    if (len == zero)
    {
        rc::st32(dst + 4, 0);
        rc::st32(dst + 8, 0);
        rc::st32(dst, 0x3f800000u);
        cpu.eax = dst;
        cpu.flags.pf = true;
        return;
    }
    const double inv = ld(One) / len;
    const reg32 zbits = rc::ld32(src + 8);
    const double sy = ld(src + 4);
    rc::stf32(dst, float(inv * ld(src)));
    rc::stf32(dst + 4, float(sy * inv));
    float zf;
    memcpy(&zf, &zbits, 4);
    rc::stf32(dst + 8, float(double(zf) * inv));
    cpu.eax = dst;
    cpu.ecx = zbits;
    cpu.flags.pf = true;
}

// Guest 0x567310: [ecx] = the 2D vector at edx scaled to the length given as a float divisor (stack argument):
// (x, y) / (|v| / d); (1, 0) when that is 0. The guest stores |v| / d into the argument slot.
void normalize2Native(x86::CPU& cpu)
{
    const reg32 dst = cpu.ecx, src = cpu.edx, arg = cpu.esp + 4;
    const double x = ld(src), y = ld(src + 4);
    double len = y * y;
    len += x * x;
    len = std::sqrt(len);
    len /= ld(arg);
    rc::stf32(arg, float(len));
    cpu.eax = dst;
    cpu.esp += 4;
    if (len == ld(Zero))
    {
        rc::st32(dst, 0x3f800000u);
        rc::st32(dst + 4, 0);
        return;
    }
    const double sy = ld(src + 4);
    const double inv = ld(One) / ld(arg);
    rc::stf32(dst, float(inv * ld(src)));
    rc::stf32(dst + 4, float(inv * sy));
}

// Guest 0x540830: st0 = sqrt(x^2 + y^2 + z^2 + 2 w) of the vector at eax.
void length4Native(x86::CPU& cpu)
{
    const reg32 v = cpu.eax;
    const double z = ld(v + 8), y = ld(v + 4), x = ld(v);
    double len = x * x;
    len += y * y;
    len += z * z;
    len += ld(v + 0xc);
    len += ld(v + 0xc);
    cpu.esp += 4;
    const reg32 count = cpu.fpu.count;
    cpu.fpu.regs[(count + 1) & 7] = std::sqrt(len);
    cpu.fpu.count = count + 1;
}

// Guest 0x5677d0: the box min [ecx], max [edx] grown to hold the point at eax plus and minus a float (stack
// argument). The guest stores point + float z into the argument slot.
void expandBoxNative(x86::CPU& cpu)
{
    const reg32 p = cpu.eax, mn = cpu.ecx, mx = cpu.edx, arg = cpu.esp + 4;
    reg32 eax = cpu.eax, ecx = cpu.ecx;
    const double r = ld(arg);
    const double lo0 = ld(p) - r;
    const float lo1 = float(ld(p + 4) - r);
    const float lo2 = float(ld(p + 8) - r);
    const float hi0 = float(r + ld(p));
    const float hi1 = float(r + ld(p + 4));
    const float hi2 = float(r + ld(p + 8));
    rc::stf32(arg, hi2);
    double a = lo0, b = ld(mn);
    compare(cpu, eax, a, b);
    if (a < b)
        rc::stf32(mn, float(lo0));
    a = double(lo1);
    b = ld(mn + 4);
    compare(cpu, eax, a, b);
    if (a < b)
    {
        eax = bits(lo1);
        rc::st32(mn + 4, eax);
    }
    a = double(lo2);
    b = ld(mn + 8);
    compare(cpu, eax, a, b);
    if (a < b)
    {
        eax = bits(lo2);
        rc::st32(mn + 8, eax);
    }
    a = double(hi0);
    b = ld(mx);
    compare(cpu, eax, a, b);
    if (a > b)
    {
        ecx = bits(hi0);
        rc::st32(mx, ecx);
    }
    a = double(hi1);
    b = ld(mx + 4);
    compare(cpu, eax, a, b);
    if (a > b)
    {
        eax = bits(hi1);
        rc::st32(mx + 4, eax);
    }
    a = double(hi2);
    b = ld(mx + 8);
    compare(cpu, eax, a, b);
    if (a > b)
    {
        ecx = bits(hi2);
        rc::st32(mx + 8, ecx);
    }
    (void)p;
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.esp += 4;
}

// Guest 0x567740: the box min [ecx], max [edx] grown to hold the point at eax.
void expandPointNative(x86::CPU& cpu)
{
    const reg32 p = cpu.eax, mn = cpu.ecx, mx = cpu.edx;
    const double x = ld(p);
    const reg32 ybits = rc::ld32(p + 4);
    reg32 eax = rc::ld32(p + 8);
    const reg32 zbits = eax;
    reg32 ecx = mn;
    float y, z;
    memcpy(&y, &ybits, 4);
    memcpy(&z, &zbits, 4);
    double a = x, b = ld(mn);
    compare(cpu, eax, a, b);
    if (a < b)
        rc::stf32(mn, float(x));
    a = double(y);
    b = ld(mn + 4);
    compare(cpu, eax, a, b);
    if (a < b)
    {
        eax = ybits;
        rc::st32(mn + 4, eax);
    }
    a = double(z);
    b = ld(mn + 8);
    compare(cpu, eax, a, b);
    if (a < b)
    {
        eax = zbits;
        rc::st32(mn + 8, eax);
    }
    a = x;
    b = ld(mx);
    compare(cpu, eax, a, b);
    if (a > b)
        rc::stf32(mx, float(x));
    a = double(y);
    b = ld(mx + 4);
    compare(cpu, eax, a, b);
    if (a > b)
    {
        ecx = ybits;
        rc::st32(mx + 4, ecx);
    }
    a = double(z);
    b = ld(mx + 8);
    compare(cpu, eax, a, b);
    if (a > b)
    {
        eax = zbits;
        rc::st32(mx + 8, eax);
    }
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.esp += 4;
    cpu.flags.pf = !(a <= b);
}

// Guest 0x567710: min [ecx] and max [edx] both set to the point at eax (through the FPU).
void pointBoxNative(x86::CPU& cpu)
{
    const reg32 p = cpu.eax, mn = cpu.ecx, mx = cpu.edx;
    const double x = ld(p), y = ld(p + 4), z = ld(p + 8);
    const float zf = float(z);
    rc::stf32(mx, float(x));
    rc::stf32(mx + 4, float(y));
    rc::stf32(mx + 8, float(z));
    rc::stf32(mn + 8, zf);
    rc::stf32(mn, float(x));
    rc::stf32(mn + 4, float(y));
    cpu.esp += 4;
}

// ---- dispatch, shadow compare and perf ----

enum Id
{
    IdSin, IdSinRadians, IdSinCos, IdAsin, IdAtan, IdTruncate, IdTable, IdGraph, IdNormalize, IdNormalize4,
    IdNormalize2, IdLength4, IdExpandBox, IdExpandPoint, IdPointBox, IdCount
};

const char* const Names[IdCount] = {
    "sin 0x564b10", "sin radians 0x564bb0", "sincos 0x564bd0", "asin 0x564cd0", "atan 0x564db0",
    "truncate 0x4387e0", "table 0x583ea0", "graph 0x583f20", "normalize 0x567370", "normalize4 0x567450",
    "normalize2 0x567310", "length4 0x540830", "expand box 0x5677d0", "expand point 0x567740",
    "point box 0x567710",
};

const reg32 Addresses[IdCount] = {
    0x564b10, 0x564bb0, 0x564bd0, 0x564cd0, 0x564db0, 0x4387e0, 0x583ea0, 0x583f20, 0x567370, 0x567450, 0x567310,
    0x540830, 0x5677d0, 0x567740, 0x567710,
};

struct Span
{
    reg32 address;
    reg32 size;
};

const int MaxSpans = 3;
const uint32_t SampleMask = 63;         // one call in 64 is timed
const uint32_t ShadowLogLimit = 20;

uint32_t s_calls = 0;
uint64_t s_sampledUs = 0;
uint32_t s_shadowCalls = 0, s_shadowDiffering = 0, s_shadowLogged = 0;

typedef void (*Guest)(win32::WinApplication*, x86::CPU&);
typedef void (*Native)(x86::CPU&);

bool sameCpu(reg32 function, const x86::CPU& g, const x86::CPU& n, char* text, size_t size)
{
    const reg32 status = ~reg32(retstate::ConditionCodes);
    const struct { const char* name; reg32 a, b; } regs[] = {
        {"x87 depth", g.fpu.count, n.fpu.count},
        {"x87 status", g.fpu.status.word & status, n.fpu.status.word & status},
    };
    for (const auto& r : regs)
    {
        if (r.a != r.b)
        {
            snprintf(text, size, "%s guest 0x%08x native 0x%08x", r.name, unsigned(r.a), unsigned(r.b));
            return false;
        }
    }
    if (!retstate::sameRegisters(retstate::registers(function), g, n, text, size)
        || !retstate::sameFlags(retstate::flags(function), g, n, text, size))
        return false;
    for (int i = 0; i < 8; ++i)
    {
        // Two NaNs compare equal regardless of sign or payload.
        const double a = g.fpu.regs[i], b = n.fpu.regs[i];
        if (memcmp(&a, &b, sizeof(a)) != 0 && !(std::isnan(a) && std::isnan(b)))
        {
            snprintf(text, size, "x87 slot %d guest %.17g native %.17g", i, a, b);
            return false;
        }
    }
    return true;
}

void shadow(Id id, win32::WinApplication* app, x86::CPU& cpu, Native native, Guest guest, const Span* spans,
            int spanCount)
{
    reg8 before[MaxSpans][16], after[MaxSpans][16];
    for (int i = 0; i < spanCount; ++i)
        memcpy(before[i], rc::host(spans[i].address), spans[i].size);
    x86::CPU n = cpu;
    native(n);
    for (int i = 0; i < spanCount; ++i)
        memcpy(after[i], rc::host(spans[i].address), spans[i].size);
    for (int i = spanCount - 1; i >= 0; --i)
        memcpy(rc::host(spans[i].address), before[i], spans[i].size);
    const reg32 caller = rc::ld32(cpu.esp);
    guest(app, cpu);
    ++s_shadowCalls;
    char text[160];
    bool same = sameCpu(Addresses[id], cpu, n, text, sizeof(text));
    for (int i = 0; same && i < spanCount; ++i)
    {
        for (reg32 k = 0; k < spans[i].size; k += 4)
        {
            const reg32 g = rc::ld32(spans[i].address + k);
            reg32 v;
            memcpy(&v, &after[i][k], 4);
            float gf, vf;
            memcpy(&gf, &g, 4);
            memcpy(&vf, &v, 4);
            if (g != v && !(std::isnan(gf) && std::isnan(vf)))
            {
                snprintf(text, sizeof(text), "dword at 0x%08x guest 0x%08x native 0x%08x", unsigned(spans[i].address + k),
                         unsigned(g), unsigned(v));
                same = false;
                break;
            }
        }
    }
    if (!same)
    {
        ++s_shadowDiffering;
        if (s_shadowLogged < ShadowLogLimit)
        {
            ++s_shadowLogged;
            pf::logInfo("shadow_math: %s (caller 0x%08x): %s", Names[id], unsigned(caller), text);
        }
    }
}

inline void dispatch(Id id, win32::WinApplication* app, x86::CPU& cpu, Native native, Guest guest,
                     const Span* spans = nullptr, int spanCount = 0)
{
    const Settings& s = settings();
    if (!s.native)
    {
        guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0))
    {
        shadow(id, app, cpu, native, guest, spans, spanCount);
        return;
    }
    if (__builtin_expect(pf::perf::enabled(), 0) && (++s_calls & SampleMask) == 0)
    {
        const uint32_t start = pf::perf::now();
        native(cpu);
        s_sampledUs += pf::perf::now() - start;
        return;
    }
    native(cpu);
}

}

double sin16(reg32 angle)
{
    const Folded f = fold(angle);
    return sinPoly(f.x, f.negative ? ld(MinusOne) : ld(One));
}

void sinCos16(reg32 angle, float& sine, float& cosine)
{
    const SinCos r = sinCos(angle);
    sine = r.sine;
    cosine = r.cosine;
}

Usage takeUsage()
{
    Usage u = { s_calls, double(s_sampledUs) * double(SampleMask + 1) / 1000.0 };
    s_calls = 0;
    s_sampledUs = 0;
    if (settings().native && settings().shadow)
        pf::logInfo("shadow_math: %u calls compared, %u differ", unsigned(s_shadowCalls), unsigned(s_shadowDiffering));
    return u;
}

}

namespace native
{

using mathfn::Span;

void sub_564b10(win32::WinApplication* app, x86::CPU& cpu)
{
    mathfn::dispatch(mathfn::IdSin, app, cpu, &mathfn::sinNative, &sub_564b10_guest);
}

void sub_564bb0(win32::WinApplication* app, x86::CPU& cpu)
{
    mathfn::dispatch(mathfn::IdSinRadians, app, cpu, &mathfn::sinRadiansNative, &sub_564bb0_guest);
}

void sub_564bd0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Span spans[] = { {cpu.ecx, 4}, {cpu.edx, 4} };
    mathfn::dispatch(mathfn::IdSinCos, app, cpu, &mathfn::sinCosNative, &sub_564bd0_guest, spans, 2);
}

void sub_564cd0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Span spans[] = { {cpu.esp + 4, 4} };
    mathfn::dispatch(mathfn::IdAsin, app, cpu, &mathfn::asinNative, &sub_564cd0_guest, spans, 1);
}

void sub_564db0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Span spans[] = { {cpu.esp + 4, 8} };
    mathfn::dispatch(mathfn::IdAtan, app, cpu, &mathfn::atanNative, &sub_564db0_guest, spans, 1);
}

void sub_4387e0(win32::WinApplication* app, x86::CPU& cpu)
{
    mathfn::dispatch(mathfn::IdTruncate, app, cpu, &mathfn::truncateNative, &sub_4387e0_guest);
}

void sub_583ea0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Span spans[] = { {cpu.esp + 4, 4} };
    mathfn::dispatch(mathfn::IdTable, app, cpu, &mathfn::tableNative, &sub_583ea0_guest, spans, 1);
}

void sub_583f20(win32::WinApplication* app, x86::CPU& cpu)
{
    mathfn::dispatch(mathfn::IdGraph, app, cpu, &mathfn::graphNative, &sub_583f20_guest);
}

void sub_567370(win32::WinApplication* app, x86::CPU& cpu)
{
    const Span spans[] = { {cpu.ecx, 12} };
    mathfn::dispatch(mathfn::IdNormalize, app, cpu, &mathfn::normalizeNative, &sub_567370_guest, spans, 1);
}

void sub_567450(win32::WinApplication* app, x86::CPU& cpu)
{
    const Span spans[] = { {cpu.edx, 12} };
    mathfn::dispatch(mathfn::IdNormalize4, app, cpu, &mathfn::normalize4Native, &sub_567450_guest, spans, 1);
}

void sub_567310(win32::WinApplication* app, x86::CPU& cpu)
{
    const Span spans[] = { {cpu.ecx, 8}, {cpu.esp + 4, 4} };
    mathfn::dispatch(mathfn::IdNormalize2, app, cpu, &mathfn::normalize2Native, &sub_567310_guest, spans, 2);
}

void sub_540830(win32::WinApplication* app, x86::CPU& cpu)
{
    mathfn::dispatch(mathfn::IdLength4, app, cpu, &mathfn::length4Native, &sub_540830_guest);
}

void sub_5677d0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Span spans[] = { {cpu.ecx, 12}, {cpu.edx, 12}, {cpu.esp + 4, 4} };
    mathfn::dispatch(mathfn::IdExpandBox, app, cpu, &mathfn::expandBoxNative, &sub_5677d0_guest, spans, 3);
}

void sub_567740(win32::WinApplication* app, x86::CPU& cpu)
{
    const Span spans[] = { {cpu.ecx, 12}, {cpu.edx, 12} };
    mathfn::dispatch(mathfn::IdExpandPoint, app, cpu, &mathfn::expandPointNative, &sub_567740_guest, spans, 2);
}

void sub_567710(win32::WinApplication* app, x86::CPU& cpu)
{
    const Span spans[] = { {cpu.ecx, 12}, {cpu.edx, 12} };
    mathfn::dispatch(mathfn::IdPointBox, app, cpu, &mathfn::pointBoxNative, &sub_567710_guest, spans, 2);
}

}
}
