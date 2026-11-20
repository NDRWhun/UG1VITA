// Native AI planning functions, bit-exact with the translated guest code: every x87 chain in double in the guest's
// order, floats where the guest stores floats, the guest's stores at or above the entry stack pointer and the
// registers, flags and x87 state the translated code leaves. Settings: ai_native=off runs the guest code,
// shadow_ai=on runs the guest code, then the native code against the memory before it, and logs the first
// differences.

#include "aiplan.h"
#include "retstate.h"

#include <lib/guest.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <recomp.h>
#include <vector>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_5a5c30_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace aiplan
{

namespace
{

using x86::reg8;
using x86::reg16;
using x86::reg32;
using x86::sreg32;

const reg32 Zero = 0x6cc7a4;
const reg32 InverseFixed16 = 0x6cc83c;
const reg32 Routes = 0x77af30;             // route pointers by number
const reg32 RoutePointBase = 0x27c;        // the route's points (0x88 bytes each)
const reg32 RoutePointSize = 0x88;
const reg32 PointVtable = 0x6c8f90;        // future point: +8 lateral minimum, +0xc lateral maximum, +0x10 identity
const reg32 RebuildPhysicsInfo = 0x57f2b0;
const reg32 GetClosestPoint = 0x5a6150;
const reg32 CalculateCurvature = 0x5a6860;
const reg32 DistanceScale = 0x6b591c;      // 0.1
const reg32 TightCurve = 0x6cccb8;         // -0.12
const reg32 OpenCurve = 0x6ccc9c;          // 0.12
const reg32 NarrowLimit = 0x6b5924;        // 4
const reg32 WideLimit = 0x6b5928;          // 15
const reg32 SpreadScale = 0x6b5920;        // 3
const int FirstLane = 0xb, LaneCount = 6;

inline double ld(reg32 address) { return double(rc::ldf32(address)); }
inline bool pf(reg32 value) { return !x86::parity8(reg8(value)); }

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("ai_native", true), pf::Config::getBool("shadow_ai", false) };
        pf::logInfo("aiplan: AI planning %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// Guest 0x5a7960 / 0x5a78b0 (future point vtable 0x6c8f90, +8 / +0xc): the lateral minimum or maximum of the
// point at a lane, 0 without a route point.
double lateral(reg32 point, reg32 lane, bool maximum)
{
    const reg8 route = rc::ld8(point + 0x5c);
    const sreg32 index = int16_t(rc::ld16(point + 0x5e));
    if (route == 0xff || index == -1)
        return ld(Zero);
    const reg32 routeBase = int8_t(route) >= 0 ? rc::ld32(Routes + reg32(int8_t(route)) * 4) : 0;
    const reg32 routePoint = reg32(index) * RoutePointSize + routeBase + RoutePointBase;
    if (routePoint == 0)
        return ld(Zero);
    float base = 0;
    if (sreg32(lane) >= 0 && sreg32(lane) < 5)
        base = rc::ldf32(point + 4);
    else if (sreg32(lane) >= 0xb && sreg32(lane) < 0x11)
        base = rc::ldf32(point + lane * 4 - 0x24);
    const sreg32 offset = sreg32(int16_t(rc::ld16(routePoint + 0x3c + lane * 2))) * 256;
    const double shift = double(offset) * ld(InverseFixed16) + double(base);
    return maximum ? ld(routePoint + 0x28) - shift : -ld(routePoint + 0x24) - shift;
}

// edx after the guest's lateral maximum call on the point (the caller loads the vtable into edx first).
reg32 edxAfterMaximum(reg32 point)
{
    if (rc::ld8(point + 0x5c) != 0xff && int16_t(rc::ld16(point + 0x5e)) == -1)
        return (PointVtable & 0xffff0000u) | 0xffffu;
    return PointVtable;
}

inline void compare(x86::CPU& cpu, double a, double b)
{
    cpu.fpu.compare(a, b);
}

void callGuestAt(win32::WinApplication* app, x86::CPU& cpu, reg32 address)
{
    ::native::callGuest(app, cpu, address);
}

// Guest 0x5a5c30 (FuturePoints::FillInFuturePointMinAndMaxL): [esp+4] the future points, eax the car. False when
// a point has another vtable (the guest runs).
bool fillMinMaxNative(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 e = cpu.esp, points = rc::ld32(e + 4), car = cpu.eax;
    const reg32 count = rc::ld32(points + 4);
    for (reg32 i = 0; i < count; ++i)
    {
        const reg32 point = rc::ld32(points + 8 + i * 4);
        if (!point || rc::ld32(point) != PointVtable)
            return false;
    }
    const reg32 ebx0 = cpu.ebx, esi0 = cpu.esi, edi0 = cpu.edi, ebp0 = cpu.ebp;
    const reg32 ride = rc::ld32(car + 0x10);
    cpu.esi = car;
    cpu.ebp = points;
    if (rc::ld8(ride + 0x424) == 0xff)
    {
        cpu.edi = ride;
        cpu.esp = e - 0x3c;
        rc::st32(cpu.esp, 1);
        callGuestAt(app, cpu, RebuildPhysicsInfo);
        if (cpu.terminate)
            return true;
    }
    const sreg32 level = int8_t(rc::ld8(ride + 0x424));
    cpu.edi = reg32(level);
    rc::st32(e - 0xc, reg32(level));
    rc::st32(e - 0x3c, 1);
    rc::st32(e - 0x40, car + 0x60);
    rc::st32(e - 0x44, points);
    cpu.esp = e - 0x44;
    cpu.eax = 0;
    callGuestAt(app, cpu, GetClosestPoint);
    if (cpu.terminate)
        return true;
    float curvature = 0;
    if (cpu.eax != ~0u)
    {
        cpu.ecx = cpu.eax;
        cpu.eax = points;
        rc::st32(e - 0x3c, 0xbf800000u);
        rc::st32(e - 0x40, reg32(level));
        cpu.esp = e - 0x40;
        callGuestAt(app, cpu, CalculateCurvature);
        if (cpu.terminate)
            return true;
        curvature = float(cpu.fpu.pop());
    }

    // The last point: its own lateral limits.
    const reg32 lastPoint = rc::ld32(points + count * 4 + 4);
    const float lastMin = float(lateral(lastPoint, reg32(level), false));
    rc::stf32(lastPoint + 0x3c, float(lateral(lastPoint, reg32(level), true)));
    rc::stf32(lastPoint + 0x20, lastMin);
    for (int k = 0; k < LaneCount; ++k)
    {
        const reg32 lane = reg32(FirstLane + k);
        const float laneMin = float(lateral(lastPoint, lane, false));
        rc::stf32(lastPoint + 0x40 + k * 4, float(lateral(lastPoint, lane, true)));
        rc::stf32(lastPoint + 0x24 + k * 4, laneMin);
    }
    rc::st32(e + 4, 0);
    reg32 ecx = count - 2, edx = edxAfterMaximum(lastPoint);

    // Each earlier point limited by the one after it.
    if (sreg32(count - 2) >= 0)
    {
        for (reg32 j = count - 1; j >= 1; --j)
        {
            const reg32 next = rc::ld32(points + 8 + j * 4), point = rc::ld32(points + 4 + j * 4);
            const float distance = float((ld(next + 0x58) - ld(point + 0x58)) * ld(DistanceScale));
            compare(cpu, double(curvature), ld(TightCurve));
            const bool tight = double(curvature) <= ld(TightCurve);
            compare(cpu, double(curvature), ld(OpenCurve));
            const bool open = double(curvature) >= ld(OpenCurve);
            float pointMin = float(lateral(point, reg32(level), false));
            double pointMax = lateral(point, reg32(level), true);
            const float width = float(pointMax - double(pointMin));
            float spread = distance;
            if (tight)
            {
                compare(cpu, -ld(NarrowLimit), double(pointMin));
                if (-ld(NarrowLimit) < double(pointMin))
                {
                    compare(cpu, double(width), ld(WideLimit));
                    if (double(width) >= ld(WideLimit))
                        pointMin = 0;
                }
                spread = float(ld(SpreadScale) * double(distance));
            }
            double shift = double(distance);
            if (open)
            {
                compare(cpu, pointMax, ld(NarrowLimit));
                if (pointMax < ld(NarrowLimit))
                {
                    compare(cpu, double(width), ld(WideLimit));
                    if (double(width) >= ld(WideLimit))
                        pointMax = ld(Zero);
                }
                shift = ld(SpreadScale) * double(distance);
            }
            const double limitMin = ld(next + 0x20) - shift;
            compare(cpu, double(pointMin), limitMin);
            rc::stf32(point + 0x20, float(double(pointMin) > limitMin ? double(pointMin) : limitMin));
            const float limitMax = float(ld(next + 0x3c) + double(spread));
            rc::stf32(e + 4, limitMax);
            compare(cpu, pointMax, double(limitMax));
            rc::stf32(point + 0x3c, float(pointMax < double(limitMax) ? pointMax : double(limitMax)));
            for (int k = 0; k < LaneCount; ++k)
            {
                const reg32 lane = reg32(FirstLane + k);
                const float laneMin = float(lateral(point, lane, false));
                rc::stf32(e + 4, laneMin);
                const float laneMax = float(lateral(point, lane, true));
                const double nextMin = ld(next + 0x24 + k * 4) - double(distance);
                compare(cpu, double(laneMin), nextMin);
                rc::stf32(point + 0x24 + k * 4, float(double(laneMin) > nextMin ? double(laneMin) : nextMin));
                const double nextMax = ld(next + 0x40 + k * 4) + double(distance);
                compare(cpu, double(laneMax), nextMax);
                rc::stf32(point + 0x40 + k * 4, float(double(laneMax) < nextMax ? double(laneMax) : nextMax));
            }
            edx = edxAfterMaximum(point);
        }
        ecx = points + 8;
    }

    cpu.eax = 0;
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.ebx = ebx0;
    cpu.esi = esi0;
    cpu.edi = edi0;
    cpu.ebp = ebp0;
    const reg32 frame = e - 0x28;
    cpu.esp = e + 8;
    cpu.flags.cf = e < frame;
    cpu.flags.zf = e == 0;
    cpu.flags.sf = e >> 31;
    cpu.flags.of = ((frame ^ e) & (0x28u ^ e)) >> 31;
    cpu.flags.pf = pf(e);
    cpu.flags.af = ((frame ^ 0x28u ^ e) >> 4) & 1;
    return true;
}

// ---- shadow compare ----

const uint32_t ShadowLogLimit = 20;
uint32_t s_shadowCalls = 0, s_shadowDiffering = 0, s_shadowLogged = 0;

struct Span
{
    reg32 address;
    reg32 size;
};

void snapshot(const std::vector<Span>& spans, std::vector<reg8>& bytes)
{
    bytes.clear();
    for (const Span& s : spans)
        bytes.insert(bytes.end(), rc::host(s.address), rc::host(s.address) + s.size);
}

void restore(const std::vector<Span>& spans, const std::vector<reg8>& bytes)
{
    size_t at = 0;
    for (const Span& s : spans)
    {
        memcpy(rc::host(s.address), bytes.data() + at, s.size);
        at += s.size;
    }
}

void report(const char* what, reg32 caller, const char* text)
{
    ++s_shadowDiffering;
    if (s_shadowLogged < ShadowLogLimit)
    {
        ++s_shadowLogged;
        pf::logInfo("shadow_ai: %s (caller 0x%08x): %s", what, unsigned(caller), text);
    }
}

typedef void (*Guest)(win32::WinApplication*, x86::CPU&);
typedef bool (*Native)(win32::WinApplication*, x86::CPU&);

// The guest runs first; the memory of spans is put back and the native code runs from the same entry state.
void shadow(const char* what, reg32 function, win32::WinApplication* app, x86::CPU& cpu, Native native,
            Guest guest, const std::vector<Span>& spans)
{
    std::vector<reg8> before, guestAfter, nativeAfter;
    snapshot(spans, before);
    const x86::CPU entry = cpu;
    const reg32 caller = rc::ld32(cpu.esp);
    guest(app, cpu);
    if (cpu.terminate)
        return;
    const x86::CPU g = cpu;
    snapshot(spans, guestAfter);
    restore(spans, before);
    x86::CPU n = entry;
    if (!native(app, n))
    {
        restore(spans, guestAfter);
        return;
    }
    if (n.terminate)
    {
        cpu.terminate = true;
        return;
    }
    snapshot(spans, nativeAfter);
    restore(spans, guestAfter);
    ++s_shadowCalls;
    char text[160] = {};
    const struct { const char* name; reg32 a, b; } regs[] = {
        {"x87 depth", g.fpu.count, n.fpu.count},
    };
    for (const auto& r : regs)
    {
        if (r.a != r.b)
        {
            snprintf(text, sizeof(text), "%s guest 0x%08x native 0x%08x", r.name, unsigned(r.a), unsigned(r.b));
            report(what, caller, text);
            return;
        }
    }
    if (!retstate::sameRegisters(retstate::registers(function), g, n, text, sizeof(text))
        || !retstate::sameFlags(retstate::flags(function), g, n, text, sizeof(text)))
    {
        report(what, caller, text);
        return;
    }
    size_t at = 0;
    for (const Span& s : spans)
    {
        for (reg32 k = 0; k + 4 <= s.size; k += 4)
        {
            reg32 a, b;
            memcpy(&a, guestAfter.data() + at + k, 4);
            memcpy(&b, nativeAfter.data() + at + k, 4);
            if (a != b)
            {
                snprintf(text, sizeof(text), "dword at 0x%08x guest 0x%08x native 0x%08x", unsigned(s.address + k),
                         unsigned(a), unsigned(b));
                report(what, caller, text);
                return;
            }
        }
        at += s.size;
    }
}

std::vector<Span> fillMinMaxSpans(const x86::CPU& cpu)
{
    std::vector<Span> spans;
    spans.push_back(Span{cpu.esp + 4, 4});
    const reg32 points = rc::ld32(cpu.esp + 4), count = rc::ld32(points + 4);
    for (reg32 i = 0; i < count && i < 64; ++i)
    {
        const reg32 point = rc::ld32(points + 8 + i * 4);
        if (point)
            spans.push_back(Span{point + 0x20, 0x38});
    }
    return spans;
}

}

void logShadowTotal()
{
    if (settings().native && settings().shadow)
        pf::logInfo("shadow_ai: %u calls compared, %u differ", unsigned(s_shadowCalls), unsigned(s_shadowDiffering));
}

}

namespace native
{

void sub_5a5c30(win32::WinApplication* app, x86::CPU& cpu)
{
    const aiplan::Settings& s = aiplan::settings();
    if (!s.native)
    {
        sub_5a5c30_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0))
    {
        aiplan::shadow("fill min and max L 0x5a5c30", 0x5a5c30, app, cpu, &aiplan::fillMinMaxNative, &sub_5a5c30_guest,
                       aiplan::fillMinMaxSpans(cpu));
        return;
    }
    if (!aiplan::fillMinMaxNative(app, cpu))
        sub_5a5c30_guest(app, cpu);
}

}}
