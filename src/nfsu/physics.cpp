// Native car physics helpers (tools/recompiler NATIVE_OVERRIDES).
//
// 0x46D1C0 shift points of a drive train: edi = engine info, edx = drive train (shift speeds at +0xC on), [esp + 4] =
// drive train info, [esp + 8] = shift points. A result of the guest body is stored with its inputs (key) and written
// again for the same inputs: the two arrays, eax, ecx, edx, the x87 compare bits and esp as the guest body leaves them.
// shift_points_native=off always runs the guest body;
// shadow_shift_points=on runs it after each stored result is written and logs the first differences.
//
// 0x580240 theoretical top speed of a ride (eax): from the start speed [ride + 0x414] in steps of [0x6B5BD4] while the
// speed is at most the table maximum [ride + 0x418] and the table value (0x583EA0 on the table at ride + 0x410) is at
// least the drag term speed * speed * [ride + 0x2C0] / [ride + 0x170]; st0 = the last speed. The loop runs natively;
// the table call for the last speed runs as the guest calls it, then the exit compare and the pops as the guest
// body does them. top_speed_native=off runs the guest body; shadow_top_speed=on runs both and logs differences.
//
// 0x478570 PhysicsMover::IterateForces (edi = the mover): the clutch torque guess of the drive train, the ground force
// guess of each wheel, five rounds of the wheels' longitudinal force against the body's acceleration with the clutch
// torque update after each, then each wheel's final ground force and the drive train's final clutch torque. It runs
// natively, bit-exact with the translated guest code: x87 chains in double in the guest's order, floats where the guest
// stores floats, the table, angle and sine helpers as the guest calls them; ebx, esi, edi, ebp and esp as the guest
// leaves them. With the force frame mode [0x6F0874] = 3 the guest body runs; the final clutch torque runs as guest code
// when its switch [0x6FFF2C] is set. forces_native=off runs the guest body; shadow_forces=on runs both and compares
// the wheels, the drive train, the engine, the rigid body and the mover (total at the end of a benchmark).
//
// 0x585810 RigidBody::ComputeAuxiliaries and 0x585410 RigidBody::GetDerivative (ecx = the body), run once per substep
// for each full-physics car, run natively with the same rounding as the guest: x87 chains in double, floats where the
// guest stores floats, the matrix products and transforms through the same functions as the guest's D3DX calls.
// rigid_body_native=off runs the guest bodies; shadow_rigid_body=on runs both and compares the body (and the
// derivative), the registers retstate lists and the x87 depth (totals at the end of a benchmark).
//
// 0x588580 Suspension::ComputeAuxiliaries (ecx = the suspension), four times per substep for a full-physics car, runs
// natively with the terrain check 0x587070 as guest code. suspension_native=off runs the guest body;
// shadow_suspension=on runs both and compares the suspension and its wheel, the registers retstate lists and the x87
// depth (total at the end of a benchmark).

#include "physics.h"

#include <lib/guest.h>
#include <platform/config.h>
#include <platform/log.h>
#include <winapi/d3d9_internal.h>
#include <winapi/d3dx9.h>
#include <fpu.h>
#include <native.h>
#include <recomp.h>

#include "mathfn.h"
#include "retstate.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace nfsu
{

void sub_46d1c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_580240_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_478570_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_585810_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_585410_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_588580_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_587070(win32::WinApplication* app, x86::CPU& cpu);
void sub_583ea0(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

void sub_564db0(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

struct Settings
{
    bool cache;
    bool shadow;
    bool topSpeed;
    bool topSpeedShadow;
    bool forces;
    bool forcesShadow;
    bool body;
    bool bodyShadow;
    bool suspension;
    bool suspensionShadow;
};

const Settings& settings()
{
    static const Settings s = [] {
        const Settings r = { pf::Config::getBool("shift_points_native", true),
                             pf::Config::getBool("shadow_shift_points", false),
                             pf::Config::getBool("top_speed_native", true),
                             pf::Config::getBool("shadow_top_speed", false),
                             pf::Config::getBool("forces_native", true),
                             pf::Config::getBool("shadow_forces", false),
                             pf::Config::getBool("rigid_body_native", true),
                             pf::Config::getBool("shadow_rigid_body", false),
                             pf::Config::getBool("suspension_native", true),
                             pf::Config::getBool("shadow_suspension", false) };
        pf::logInfo("physics: shift points %s%s, top speed %s%s, forces %s%s, rigid body %s%s, suspension %s%s",
                    r.cache ? "kept per input" : "guest code", r.cache && r.shadow ? " (shadow compare)" : "",
                    r.topSpeed ? "native" : "guest code", r.topSpeed && r.topSpeedShadow ? " (shadow compare)" : "",
                    r.forces ? "native" : "guest code", r.forces && r.forcesShadow ? " (shadow compare)" : "",
                    r.body ? "native" : "guest code", r.body && r.bodyShadow ? " (shadow compare)" : "",
                    r.suspension ? "native" : "guest code",
                    r.suspension && r.suspensionShadow ? " (shadow compare)" : "");
        return r;
    }();
    return s;
}

const x86::reg32 LinearShiftPoints = 0x00700030;    // 0: shift points spread linearly over the rpm range
const x86::reg32 InfoGearCount = 0x18;               // drive train info: gear count, ratios from +0x28
const x86::reg32 InfoRatios = 0x28;
const x86::reg32 EngineWords = 4 + 16;               // engine info: rpm limits, then the torque table
const x86::reg32 MaxGears = 30;
const size_t MaxEntries = 64;
const x86::reg16 CompareBits = 0x4500;

struct ShiftPoints
{
    std::vector<x86::reg32> key;
    std::vector<x86::reg32> points;     // shift points [1 .. gears + 1]
    std::vector<x86::reg32> speeds;     // drive train +0xC [0 .. gears - 2]
    x86::reg16 compareBits;
    int eaxSource;                      // eax: EaxSpeeds, or the upper half of the info or engine pointer word
    x86::sreg32 espShift;               // esp after the body less esp at the call
};

enum { EaxSpeeds, EaxInfo, EaxEngine, EaxSources };

std::vector<ShiftPoints> s_entries;
size_t s_nextEntry = 0;
uint32_t s_calls = 0, s_hits = 0, s_compared = 0, s_differing = 0;

void key(x86::CPU& cpu, x86::reg32 info, x86::reg32 engine, x86::reg32 last, std::vector<x86::reg32>& k)
{
    k.clear();
    k.push_back(rc::ld32(LinearShiftPoints));
    k.push_back(cpu.fpu.control.word);
    k.push_back(rc::ld32(info));
    k.push_back(rc::ld32(info + InfoGearCount));
    for (x86::reg32 i = 0; i + 2 <= last; ++i)
        k.push_back(rc::ld32(info + InfoRatios + 4 * i));
    for (x86::reg32 i = 0; i < EngineWords; ++i)
        k.push_back(rc::ld32(engine + 4 * i));
}

x86::reg32 eaxOf(int source, x86::reg32 info, x86::reg32 engine, x86::reg32 speeds, x86::reg32 last,
                 x86::reg16 status)
{
    if (source == EaxSpeeds)
        return speeds + 4 * (last - 3);
    const x86::reg32 upper = source == EaxInfo ? info : rc::ld32(engine + 4);
    return (upper & 0xffff0000u) | status;
}

}

void sub_46d1c0(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    const x86::reg32 entry = cpu.esp;
    const x86::reg32 info = rc::ld32(entry + 4), points = rc::ld32(entry + 8);
    const x86::reg32 engine = cpu.edi, speeds = cpu.edx + 0xc;
    const x86::reg32 last = rc::ld32(info + InfoGearCount) + 1;
    if (!s.cache || x86::sreg32(last) <= 2 || last > MaxGears)
    {
        sub_46d1c0_guest(app, cpu);
        return;
    }
    if ((++s_calls & 1023) == 0)
        pf::logInfo("physics: shift points %u calls, %u from stored results, %u stored", unsigned(s_calls),
                    unsigned(s_hits), unsigned(s_entries.size()));
    static std::vector<x86::reg32> k;
    key(cpu, info, engine, last, k);
    const ShiftPoints* found = nullptr;
    for (const ShiftPoints& e : s_entries)
        if (e.key == k)
        {
            found = &e;
            break;
        }

    if (found && !s.shadow)
    {
        ++s_hits;
        for (x86::reg32 i = 0; i < last; ++i)
            rc::st32(points + 4 * (i + 1), found->points[i]);
        for (x86::reg32 i = 0; i + 2 < last; ++i)
            rc::st32(speeds + 4 * i, found->speeds[i]);
        cpu.fpu.status.word = x86::reg16((cpu.fpu.status.word & ~CompareBits) | found->compareBits);
        cpu.eax = eaxOf(found->eaxSource, info, engine, speeds, last, cpu.fpu.status.word);
        cpu.ecx = rc::ld32(engine + 4);
        cpu.edx = info + InfoRatios + 4 + 4 * (last - 2);
        cpu.esp = entry + found->espShift;
        return;
    }

    const x86::CPU before = cpu;
    sub_46d1c0_guest(app, cpu);
    if (cpu.terminate)
        return;
    ShiftPoints e;
    e.points.resize(last);
    for (x86::reg32 i = 0; i < last; ++i)
        e.points[i] = rc::ld32(points + 4 * (i + 1));
    e.speeds.resize(last - 2);
    for (x86::reg32 i = 0; i + 2 < last; ++i)
        e.speeds[i] = rc::ld32(speeds + 4 * i);
    e.compareBits = x86::reg16(cpu.fpu.status.word & CompareBits);
    e.espShift = x86::sreg32(cpu.esp - entry);
    e.eaxSource = EaxSources;
    for (int source = 0; source < EaxSources && e.eaxSource == EaxSources; ++source)
        if (cpu.eax == eaxOf(source, info, engine, speeds, last, cpu.fpu.status.word))
            e.eaxSource = source;

    if (found)
    {
        ++s_compared;
        const x86::reg16 status = x86::reg16((before.fpu.status.word & ~CompareBits) | found->compareBits);
        const bool same = e.points == found->points && e.speeds == found->speeds &&
                          cpu.fpu.status.word == status &&
                          cpu.eax == eaxOf(found->eaxSource, info, engine, speeds, last, status) &&
                          cpu.ecx == rc::ld32(engine + 4) && cpu.edx == info + InfoRatios + 4 + 4 * (last - 2) &&
                          cpu.esp == entry + found->espShift;
        if (!same && ++s_differing <= 8)
            pf::logInfo("shadow_shift_points: call %u differs (eax 0x%08x ecx 0x%08x edx 0x%08x status 0x%04x)",
                        unsigned(s_calls), unsigned(cpu.eax), unsigned(cpu.ecx), unsigned(cpu.edx),
                        unsigned(cpu.fpu.status.word));
        if ((s_compared & 1023) == 0)
            pf::logInfo("shadow_shift_points: %u calls compared, %u differ", unsigned(s_compared),
                        unsigned(s_differing));
        return;
    }

    // Kept only when the registers are those the stored result gives.
    const bool kept = e.eaxSource != EaxSources && cpu.ebx == before.ebx && cpu.ebp == before.ebp &&
                      cpu.esi == before.esi && cpu.edi == before.edi && cpu.fpu.count == before.fpu.count &&
                      (cpu.fpu.status.word & ~CompareBits) == (before.fpu.status.word & ~CompareBits) &&
                      cpu.ecx == rc::ld32(engine + 4) && cpu.edx == info + InfoRatios + 4 + 4 * (last - 2);
    if (!kept)
        return;
    e.key = k;
    if (s_entries.size() < MaxEntries)
        s_entries.push_back(std::move(e));
    else
    {
        s_entries[s_nextEntry] = std::move(e);
        s_nextEntry = (s_nextEntry + 1) % MaxEntries;
    }
}

namespace
{

const x86::reg32 RideRebuild = 0x424, RideStart = 0x414, RideTable = 0x410, RideDragA = 0x2c0, RideDragB = 0x170;
const x86::reg32 TopSpeedStep = 0x006b5bd4;
const int TopSpeedSlots = 5;
uint32_t s_topCompared = 0, s_topDiffering = 0;

// The value 0x583EA0 gives for the table at table and the argument (mathfn.cpp tableNative).
double tableValue(x86::reg32 table, float argument)
{
    double index = double(argument) - double(rc::ldf32(table + 4));
    index *= double(rc::ldf32(table + 0xc));
    const float stored = float(index);
    const x86::sreg32 i = x86::sreg32(x86::reg32(x86::truncInteger64(index)));
    const x86::reg32 values = rc::ld32(table + 0x10);
    if (i < 0)
        return double(rc::ldf32(values));
    const x86::reg32 entries = rc::ld32(table);
    if (!(i < x86::sreg32(entries - 1)))
        return double(rc::ldf32(values + entries * 4 - 4));
    const double whole = double(x86::sreg32(x86::reg32(x86::truncInteger64(double(stored)))));
    double frac = whole;
    if (whole > double(stored))
        frac -= 1.0;
    frac = double(stored) - frac;
    const x86::reg32 at = values + x86::reg32(i) * 4;
    double r = 1.0;
    r -= frac;
    r *= double(rc::ldf32(at));
    double t = frac;
    t *= double(rc::ldf32(at + 4));
    return r + t;
}

// False: the guest body runs instead (rebuild pending, or the start speed above the table maximum).
bool topSpeed(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 entry = cpu.esp, ride = cpu.eax;
    if (rc::ld8(ride + RideRebuild) == 0xff)
        return false;
    const x86::reg32 table = ride + RideTable;
    const double limit = double(rc::ldf32(table + 8));
    float speed = rc::ldf32(ride + RideStart);
    if (!(double(speed) <= limit))
        return false;
    const double dragA = double(rc::ldf32(ride + RideDragA)), dragB = double(rc::ldf32(ride + RideDragB));
    const double step = double(rc::ldf32(TopSpeedStep));
    float called;
    double compareA, compareB;
    for (;;)
    {
        called = speed;
        const double value = tableValue(table, speed);
        double drag = double(speed);
        drag *= dragA;
        drag *= double(speed);
        drag /= dragB;
        if (value < drag)
        {
            compareA = value, compareB = drag;
            break;
        }
        double next = step;
        next += double(speed);
        speed = float(next);
        if (!(next <= limit))
        {
            compareA = next, compareB = limit;
            break;
        }
    }

    const x86::reg32 esi0 = cpu.esi, edi0 = cpu.edi, count = cpu.fpu.count;
    rc::st32(entry - 4, rc::ld32(ride + RideStart));
    rc::st32(entry - 8, esi0);
    rc::st32(entry - 12, edi0);
    rc::stf32(entry - 16, called);
    rc::stf32(entry - 4, called);
    cpu.ecx = rc::ld32(entry - 4);
    cpu.esp = entry - 20;
    cpu.esi = table;
    cpu.edi = ride;
    cpu.fpu.count = count;
    sub_583ea0(app, cpu);
    if (cpu.terminate)
        return true;
    rc::stf32(entry - 4, speed);
    cpu.fpu.compare(compareA, compareB);
    cpu.eax = (cpu.eax & 0xffff0000u) | x86::reg16(cpu.fpu.status.word);
    cpu.ecx = rc::ld32(entry - 4);
    cpu.esi = esi0;
    cpu.edi = edi0;
    cpu.fpu.regs[(count + 1) & 7] = double(rc::ldf32(entry - 4));
    cpu.fpu.count = count + 1;
    return true;
}

}

void sub_580240(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    if (!s.topSpeed)
    {
        sub_580240_guest(app, cpu);
        return;
    }
    if (!s.topSpeedShadow)
    {
        if (!topSpeed(app, cpu))
            sub_580240_guest(app, cpu);
        return;
    }
    const x86::CPU entry = cpu;
    x86::reg32 slots[TopSpeedSlots];
    for (int i = 0; i < TopSpeedSlots; ++i)
        slots[i] = rc::ld32(entry.esp - 4 * (i + 1));
    if (!topSpeed(app, cpu))
    {
        sub_580240_guest(app, cpu);
        return;
    }
    if (cpu.terminate)
        return;
    const x86::CPU native = cpu;
    x86::reg32 nativeSlots[TopSpeedSlots];
    for (int i = 0; i < TopSpeedSlots; ++i)
    {
        nativeSlots[i] = rc::ld32(entry.esp - 4 * (i + 1));
        rc::st32(entry.esp - 4 * (i + 1), slots[i]);
    }
    cpu = entry;
    sub_580240_guest(app, cpu);
    if (cpu.terminate)
        return;
    ++s_topCompared;
    bool same = cpu.eax == native.eax && cpu.ecx == native.ecx && cpu.edx == native.edx && cpu.esp == native.esp &&
                cpu.esi == native.esi && cpu.edi == native.edi && cpu.fpu.count == native.fpu.count &&
                cpu.fpu.status.word == native.fpu.status.word &&
                std::memcmp(&cpu.fpu.regs[cpu.fpu.count & 7], &native.fpu.regs[native.fpu.count & 7],
                            sizeof(cpu.fpu.regs[0])) == 0 &&
                std::memcmp(&cpu.flags, &native.flags, sizeof(cpu.flags)) == 0;
    for (int i = 0; i < TopSpeedSlots; ++i)
        same = same && rc::ld32(entry.esp - 4 * (i + 1)) == nativeSlots[i];
    if (!same && ++s_topDiffering <= 8)
        pf::logInfo("shadow_top_speed: call %u differs: eax %08x/%08x ecx %08x/%08x edx %08x/%08x esp %08x/%08x "
                    "status %04x/%04x st0 %.9g/%.9g",
                    unsigned(s_topCompared), unsigned(cpu.eax), unsigned(native.eax), unsigned(cpu.ecx),
                    unsigned(native.ecx), unsigned(cpu.edx), unsigned(native.edx), unsigned(cpu.esp),
                    unsigned(native.esp), unsigned(cpu.fpu.status.word), unsigned(native.fpu.status.word),
                    double(cpu.fpu.regs[cpu.fpu.count & 7]), double(native.fpu.regs[native.fpu.count & 7]));
    if ((s_topCompared & 255) == 0)
        pf::logInfo("shadow_top_speed: %u calls compared, %u differ", unsigned(s_topCompared),
                    unsigned(s_topDiffering));
}

namespace
{

using x86::reg32;
using x86::sreg32;

// Float constants in .rdata and game switches.
const reg32 Zero = 0x6cc7a4;
const reg32 One = 0x6cc7bc;
const reg32 Half = 0x6cc7dc;
const reg32 RpmScale = 0x6ccb78;
const reg32 AngleToRadians = 0x6cc980;
const reg32 RadiansToAngle = 0x6cca4c;
const reg32 AngleWrap = 0x6cc97c;
const reg32 AngleTurn = 0x6cc978;
const reg32 LoadScale = 0x6f0794;
const reg32 SlipLimit = 0x6f0780;
const reg32 FrictionTable = 0x6ef2f8;       // 0x94 bytes per surface; the static friction 4 bytes before
const reg32 ForceFrameMode = 0x6f0874;
const reg32 LateralMode = 0x6f0868;
const reg32 FinalClutchSwitch = 0x6fff2c;
const reg32 FinalizeClutch = 0x46e440;

inline double ld(reg32 address) { return double(rc::ldf32(address)); }
inline void stf(reg32 address, double value) { rc::stf32(address, float(value)); }
inline reg32 bitsOf(float value) { reg32 r; memcpy(&r, &value, 4); return r; }
inline float floatOf(reg32 value) { float r; memcpy(&r, &value, 4); return r; }

// A 4x4 float matrix as 0x565230 transposes it (two of its values pass through the x87).
void transposed(reg32 source, float* m)
{
    for (reg32 r = 0; r < 4; ++r)
        for (reg32 c = 0; c < 4; ++c)
            m[c * 4 + r] = rc::ldf32(source + (r * 4 + c) * 4);
    m[4] = float(double(m[4]));
    m[8] = float(double(m[8]));
}

// D3DXVec3Transform (0x5ABDAF): (v, 1) * m.
void transform3(reg32 v, const float* m, float* r)
{
    const float in[4] = {rc::ldf32(v), rc::ldf32(v + 4), rc::ldf32(v + 8), 1.0f};
    win32::d3dx9::transform(in, m, r);
}

class Forces
{
public:
    Forces(win32::WinApplication* app, x86::CPU& cpu)
        : m_app(app), m_cpu(cpu), m_mover(cpu.edi), m_entry(cpu.esp), m_scratch((cpu.esp - 0x400) & ~0xfu)
    {
    }

    // False when the guest body has to run (nothing written).
    bool run()
    {
        if (rc::ld32(ForceFrameMode) == 3)
            return false;
        const reg32 ebx = m_cpu.ebx, esi = m_cpu.esi, ebp = m_cpu.ebp, edi = m_cpu.edi;
        const reg32 count = m_cpu.fpu.count;
        guessClutch(rc::ld32(m_mover + 0x4c));
        const reg32 pilot = rc::ld32(rc::ld32(m_mover + 0x5c) + 0x3c);
        const reg32 aided = reg32(sreg32(int8_t(rc::ld8(pilot + 0x1c))));
        for (int i = 0; i < 4; ++i)
            guessGround(wheel(i), aided);

        const reg32 body = rc::ld32(m_mover + 0x20);
        float m[16], v[4];
        transposed(body + 0x30, m);
        transform3(body + 0x110, m, v);
        for (int round = 0; round < 5; ++round)
            longitudinal(body, v);

        for (int i = 0; i < 4; ++i)
            finalizeGround(wheel(i));
        if (rc::ld32(FinalClutchSwitch) != 0)
        {
            m_cpu.esi = rc::ld32(m_mover + 0x4c);
            m_cpu.esp = m_scratch;
            ::native::callGuest(m_app, m_cpu, FinalizeClutch);
            if (m_cpu.terminate)
                return true;
        }
        m_cpu.ebx = ebx;
        m_cpu.esi = esi;
        m_cpu.ebp = ebp;
        m_cpu.edi = edi;
        m_cpu.esp = m_entry + 4;
        m_cpu.fpu.count = count;
        return true;
    }

private:
    reg32 wheel(int i) const { return rc::ld32(m_mover + 0x38 + 4 * reg32(i)); }
    reg32 contact(int i) const { return rc::ld32(m_mover + 0x28 + 4 * reg32(i)); }

    // 0x564DB0: the 16-bit angle of (x, y), called as the guest calls it.
    reg32 angle(reg32 x, reg32 y)
    {
        rc::st32(m_scratch + 4, x);
        rc::st32(m_scratch + 8, y);
        m_cpu.esp = m_scratch;
        sub_564db0(m_app, m_cpu);
        return m_cpu.eax & 0xffffu;
    }

    // 0x46CFF0 Engine::GetTotalEngineTorque.
    static double engineTorque(reg32 e)
    {
        double t = tableValue(e + 0x24, float(ld(e + 0x14) * ld(RpmScale))) * ld(e + 0xa0);
        if (rc::ld32(e + 0x74) != 0)
            t *= ld(0x700024);
        const double nitrous = ld(e + 0x9c) == ld(Zero) ? ld(e + 0x80) * ld(e + 0x78) : ld(Zero);
        float drive = float(t * nitrous);
        if (rc::ld32(e + 0x84) != 0)
        {
            const reg32 ride = rc::ld32(rc::ld32(e + 0x1c));
            const double boost = tableValue(e + 0x60, float(ld(e + 0x14) * ld(RpmScale))) * ld(ride + 0x2ac);
            drive = float(boost + double(drive));
            if (rc::ld8(0x78a344) != 0 || rc::ld32(0x7361a8) != 0)
            {
                const double limit = ld(0x70002c) + ld(e + 0x88);
                const double now = double(sreg32(rc::ld32(0x73ad34))) * ld(0x6cca38);
                if (now <= limit)
                    drive = float(ld(0x6b7238) * double(drive));
            }
        }
        if (ld(e + 0x8c) > ld(0x6ffff0))
        {
            double fade = (ld(e + 0x8c) - ld(0x6ffff0)) / (ld(One) - ld(0x6ffff0));
            fade = (ld(One) - fade) / ld(0x6b720c);
            drive = float(fade * double(drive));
        }
        const float share = ld(e + 0x9c) == ld(Zero) ? float(ld(e + 0x80) * ld(e + 0x78)) : 0.0f;
        double r = -(tableValue(e + 0x38, float(ld(e + 0x14) * ld(RpmScale))) * (ld(One) - double(share)));
        if (ld(e + 0x9c) > ld(Zero) && rc::ld32(rc::ld32(e + 0x20) + 0x3c) != 1)
            r *= ld(0x6b7210);
        r += double(drive);
        if (r < ld(Zero))
        {
            if (rc::ld32(rc::ld32(e + 0x20) + 0x3c) == 1)
                return r * ld(0x6b723c);
            if (rc::ld32(rc::ld32(e + 0x1c) + 0x3c0) == 1)
                r *= ld(0x6cca20);
        }
        return r;
    }

    // The share of an axle's torque that goes to wheel b (a the other wheel), 0x46E020.
    static double axleShare(reg32 a, reg32 b, reg32 split, double s)
    {
        if (split == 1)
            return ld(One) - s;
        if (split == 2)
            return s;
        const double sum = ld(a + 0x28) + ld(b + 0x28);
        if (!(std::fabs(sum) > ld(0x6ccb7c)))
            return ld(Half);
        const double r = ld(b + 0x28) / sum;
        if (r < s)
            return ld(One) - ld(0x700044) * ld(Half);
        if (r > ld(One) - s)
            return ld(0x700044) * ld(Half);
        return r;
    }

    // The locked-axle share of an axle's wheels: [0x700044] with both wheels on the ground, else 0.5.
    static float lockShare(reg32 a, reg32 b)
    {
        return rc::ld8(a + 0x155) != 0 && rc::ld8(b + 0x155) != 0 ? rc::ldf32(0x700044) : 0.5f;
    }

    // 0x46E020 DriveTrain::ApplyClutchTorque.
    static void applyClutch(reg32 d, float torque)
    {
        if (rc::ld32(d + 0x3c) == 1)
            return;
        const double t = double(torque);
        stf(d + 0x70, t + ld(d + 0x70));
        const reg32 e = rc::ld32(d + 0x20);
        stf(e + 0x7c, ld(e + 0x7c) - t);
        const reg32 info = rc::ld32(d + 0x18), gear = rc::ld32(d + 0x3c);
        const double g = t * ld(info + gear * 4 + 0x20);
        const float front = float((ld(One) - ld(info + 0x10)) * ld(info + 8) * g);
        const double rear = g * ld(info + 0xc) * ld(info + 0x10);
        const reg32 w0 = rc::ld32(d + 0x24), w1 = rc::ld32(d + 0x28);
        const reg32 split = rc::ld32(rc::ld32(d + 0x34) + 0x3c8);
        const double p = axleShare(w0, w1, split, double(lockShare(w0, w1)));
        if (rc::ld32(w0 + 0x10c) == 0)
            stf(w0 + 0x108, (ld(One) - p) * double(front) + ld(w0 + 0x108));
        if (rc::ld32(w1 + 0x10c) == 0)
            stf(w1 + 0x108, p * double(front) + ld(w1 + 0x108));
        const reg32 w2 = rc::ld32(d + 0x2c), w3 = rc::ld32(d + 0x30);
        const double q = axleShare(w3, w2, split, double(lockShare(w2, w3)));
        if (rc::ld32(w2 + 0x10c) == 0)
            stf(w2 + 0x108, q * rear + ld(w2 + 0x108));
        if (rc::ld32(w3 + 0x10c) == 0)
            stf(w3 + 0x108, (ld(One) - q) * rear + ld(w3 + 0x108));
    }

    // 0x46E270 DriveTrain::GuessClutchTorque.
    static void guessClutch(reg32 d)
    {
        rc::st32(d + 0x70, 0);
        const reg32 mode = rc::ld32(d + 0x44);
        if (mode == 3)
        {
            applyClutch(d, float(engineTorque(rc::ld32(d + 0x20))));
            return;
        }
        if (mode != 0 && mode != 2)
            return;
        const reg32 e = rc::ld32(d + 0x20);
        double slip = ld(e + 0x14) - ld(d + 0x4c);
        const double limit = ld(0x6fff28);
        if (slip > limit)
            slip = limit;
        else
        {
            const float low = float(-limit);
            if (slip < double(low))
                slip = double(low);
        }
        double stiffness = ld(rc::ld32(d + 0x18) + 0x14);
        const double rate = (ld(d + 0x4c) - ld(e + 0x14)) * ld(RpmScale);
        const double last = ld(d + 0x58);
        if (!(last == ld(Zero)) && rate * last < ld(Zero) && std::fabs(rate) > ld(0x6ccaa4) &&
            std::fabs(last) > ld(0x6ccaa4))
            stiffness *= ld(0x6b7248);
        stf(d + 0x58, rate);
        applyClutch(d, float(stiffness * ld(d + 0x14) * slip));
    }

    // (torque - inertia * spin) / radius of a wheel, 0x46D9F0.
    static double wheelAcceleration(reg32 w)
    {
        return (ld(w + 0x108) - ld(rc::ld32(w + 0x14) + 0x10) * ld(w + 0x118)) / ld(w + 0x2c);
    }

    // 0x46D9F0 DriveTrain::GetDifferentialAngularAcceleration.
    static double differentialAcceleration(reg32 d)
    {
        const reg32 info = rc::ld32(d + 0x18);
        double r = ld(Zero);
        if (!(ld(info + 0x10) < ld(0x6cc8bc)))
            r = (wheelAcceleration(rc::ld32(d + 0x30)) + wheelAcceleration(rc::ld32(d + 0x2c))) * ld(Half) *
                ld(info + 0xc);
        if (ld(info + 0x10) <= ld(0x6cc96c))
        {
            r += (wheelAcceleration(rc::ld32(d + 0x28)) + wheelAcceleration(rc::ld32(d + 0x24))) * ld(Half) *
                 ld(info + 8);
            if (!(ld(info + 0x10) < ld(0x6cc8bc)))
                r *= ld(Half);
        }
        return r * ld(info + rc::ld32(d + 0x3c) * 4 + 0x20);
    }

    // 0x46E360 DriveTrain::ModifyClutchTorque.
    static void modifyClutch(reg32 d)
    {
        if (rc::ld32(d + 0x44) != 3)
            return;
        const reg32 e = rc::ld32(d + 0x20);
        const double torque = engineTorque(e) + ld(e + 0x7c);
        double inertia = ld(rc::ld32(e + 0x18) + 0xc);
        const reg32 mode = rc::ld32(rc::ld32(e + 0x20) + 0x44);
        if (mode == 2 || mode == 0)
            inertia *= ld(0x6b7240);
        const float engine = float(torque / inertia);
        const double gap = double(engine) - differentialAcceleration(d);
        double inverse = ld(One) / ld(rc::ld32(rc::ld32(d + 0x20) + 0x18) + 0xc);
        const reg32 info = rc::ld32(d + 0x18), gear = rc::ld32(d + 0x3c);
        if (ld(info + 0x10) <= ld(0x6cc96c))
        {
            const double g = ld(info + gear * 4 + 0x20) * ld(info + 8);
            inverse += (ld(One) - ld(info + 0x10)) / ld(rc::ld32(d + 0x24) + 0x2c) * g * g * ld(Half);
        }
        if (!(ld(info + 0x10) < ld(0x6cc8bc)))
        {
            const double g = ld(info + gear * 4 + 0x20) * ld(info + 0xc);
            inverse += g / ld(rc::ld32(d + 0x2c) + 0x2c) * g * ld(info + 0x10) * ld(Half);
        }
        applyClutch(d, float(gap / inverse));
    }

    // 0x5872B0 Wheel::GetPilotFactor.
    static double pilotFactor(reg32 w)
    {
        if (rc::ld32(w + 0x10c) != 0 || ld(w + 0x28) < ld(Zero) ||
            sreg32(rc::ld32(rc::ld32(w + 0x14) + 0x18)) < 2)
            return ld(One);
        double v = (ld(rc::ld32(w + 0x20) + 0x390) - ld(0x6cca60)) * ld(0x6cca5c);
        if (v > ld(One))
            v = ld(One);
        else if (v < ld(Zero))
            v = ld(Zero);
        return v * (ld(One) - ld(0x6f0798)) + ld(0x6f0798);
    }

    // 0x587220 Wheel::CheckForBrakeLock.
    static void checkBrakeLock(reg32 w, float load)
    {
        const reg32 info = rc::ld32(w + 0x1c);
        const double brake = (ld(info + 8) * ld(w + 0x174) * ld(0x6f0788) + ld(info + 4) * ld(w + 0x170) *
                              ld(0x6f0784)) * ld(0x6f078c);
        const double hold = std::fabs(ld(w + 0x28)) * ld(0x6f0790) + ld(rc::ld32(w + 0x14) + 0x10) * double(load);
        if (brake > hold)
        {
            rc::st32(w + 0x10c, brake > ld(One) ? 1 : 0);
            rc::st32(w + 0x28, 0);
        }
        else
            rc::st32(w + 0x10c, 0);
    }

    // 0x587740 Wheel::GetLateralForce.
    static double lateralForce(reg32 w, float load, float slip)
    {
        if (rc::ld32(LateralMode) == 0)
            return tableValue(0x7043d8, slip) * ld(w + 0x158) * ld(LoadScale) * ld(0x6f07b8);
        const reg32 info = rc::ld32(w + 0x18);
        const double at = double(slip) * ld(0x6cc9f4) / ld(info) * ld(0x6ccbb4) * ld(Half);
        const float stored = float(at);
        const reg32 n = reg32(x86::truncInteger64(at));
        const float scaled = float(ld(0x6f0870) * double(load) / ld(info + 4));
        if (sreg32(n) >= 6)
            return tableValue(rc::ld32(0x6f0864), scaled) * ld(info + 8) * ld(0x6f086c);
        const float a = float(tableValue(rc::ld32(0x6f084c + n * 4), scaled));
        const double b = tableValue(rc::ld32(0x6f0850 + n * 4), scaled);
        const double frac = double(stored) - double(sreg32(n));
        return (frac * (b - double(a)) + double(a)) * ld(info + 8) * ld(0x6f086c);
    }

    // The wheel's ground velocity in its own frame (0x565230 transpose without the translation, 0x5ABDAF).
    static void groundVelocity(reg32 w, float* v)
    {
        float m[16];
        transposed(w + 0x70, m);
        m[3] = m[7] = m[11] = 0;
        transform3(w + 0xf0, m, v);
    }

    // 0x587330 Wheel::GuessGroundForce (second argument: the driver aid flag).
    void guessGround(reg32 w, reg32 aided)
    {
        const reg32 info = rc::ld32(w + 0x1c);
        double brake = ld(info + 4) * ld(w + 0x170) * ld(0x6f0784);
        const float handbrake = float(ld(info + 8) * ld(w + 0x174) * ld(0x6f0788));
        const bool free = rc::ld32(w + 0x10c) == 0;
        if (ld(w + 0x28) > ld(Zero))
            brake = -brake;
        if (free)
            stf(w + 0x108, brake + ld(w + 0x108));
        double hand = double(handbrake);
        if (ld(w + 0x28) > ld(Zero))
            hand = -hand;
        if (free)
            stf(w + 0x108, hand + ld(w + 0x108));

        float v[4];
        groundVelocity(w, v);
        const reg32 vx = bitsOf(v[0]), vy = bitsOf(v[1]);
        const double a = double(sreg32(angle(vx, vy))) * ld(AngleToRadians);
        float slipAngle = float(a);
        if (a > ld(AngleWrap))
            slipAngle = float(double(slipAngle) - ld(AngleTurn));
        if (double(v[0]) < ld(Zero))
        {
            const double b = double(slipAngle) + ld(AngleWrap);
            slipAngle = float(b);
            if (b > ld(AngleWrap))
                slipAngle = float(double(slipAngle) - ld(AngleTurn));
        }
        const double slip = ld(rc::ld32(w + 0x14) + 0x10) * ld(w + 0x28) - double(v[0]);
        stf(w + 0x160, slip);
        stf(w + 0x164, slip);
        if (-ld(SlipLimit) > ld(w + 0x160) && ld(w + 0x108) < ld(Zero) && double(handbrake) == ld(Zero) &&
            aided == 0)
            stf(w + 0x108, ld(w + 0x108) * ld(0x6cca10));

        const float sideSquared = float(double(v[1]) * double(v[1]));
        const reg32 slipBits = rc::ld32(w + 0x160);
        const float slipF = floatOf(slipBits);
        const double speed = std::sqrt(double(slipF) * double(slipF) + double(sideSquared));
        float total = float(speed);
        bool rolling = speed == ld(Zero) || (double(v[1]) == ld(Zero) && double(v[0]) == ld(Zero));
        if (!rolling)
        {
            const reg32 surface = rc::ld32(w + 0x150) * 0x94;
            const reg32 tire = rc::ld32(w + 0x18), car = rc::ld32(w + 0x20);
            float grip = float(ld(FrictionTable + surface) * ld(tire + 0x10) * ld(w + 0x180) * ld(w + 0x17c) *
                               ld(w + 0x178));
            if (rc::ld32(car + 0x3c0) == 1)
                grip = float(pilotFactor(w) * double(grip));
            total = float(ld(LoadScale) / double(total) * double(grip) * ld(w + 0x158));
            const double q = ld(LoadScale) / std::sqrt(double(v[0]) * double(v[0]) + double(sideSquared)) *
                             double(grip) * ld(w + 0x158);
            checkBrakeLock(w, float(q * std::fabs(double(v[0]))));
            rolling = !(std::fabs(double(slipF)) > ld(SlipLimit) ||
                        std::fabs(double(slipAngle)) > ld(rc::ld32(w + 0x18)) || rc::ld32(w + 0x10c) != 0);
            if (!rolling)
            {
                rc::st8(w + 0x154, 1);
                const float drive = float(double(slipF) * double(total));
                rc::stf32(w + 0x118, drive);
                const float side = float(-(double(total) * double(v[1])));
                rc::stf32(w + 0x11c, side);
                if (ld(rc::ld32(w + 0x20) + 0x390) * ld(0x6cc9f0) < ld(One) && double(grip) > ld(0x6cc7b8))
                {
                    const double inverse = ld(One) / double(grip);
                    stf(w + 0x118, double(drive) * inverse);
                    stf(w + 0x11c, inverse * double(side));
                }
                rc::st32(w + 0x100, slipBits);
                rc::st32(w + 0x104, vy);
                return;
            }
        }
        rc::st8(w + 0x154, 0);
        stf(w + 0x118, ld(w + 0x108) / ld(rc::ld32(w + 0x14) + 0x10));
        const double forward = double(v[0]) > ld(Zero) ? 1.0 : -1.0;
        const double turn = double(slipAngle) > ld(Zero) ? -1.0 : 1.0;
        const double lateral = lateralForce(w, float(ld(LoadScale) * ld(w + 0x158)),
                                            float(std::fabs(double(slipAngle))));
        rc::st32(w + 0x100, 0);
        rc::st32(w + 0x104, 0);
        stf(w + 0x11c, lateral * turn * forward);
    }

    // One round of 0x478570's loop: the wheels' forces against the body's acceleration, the wheels' spin, then
    // 0x46E360 DriveTrain::ModifyClutchTorque.
    void longitudinal(reg32 body, const float* v)
    {
        const reg32 w0 = wheel(0), c0 = contact(0);
        const float a0 = float(ld(w0 + 0xc0) * ld(w0 + 0x11c) + ld(w0 + 0x118) * ld(w0 + 0xb0));
        const float b0 = float(ld(w0 + 0xb4) * ld(w0 + 0x118) + ld(w0 + 0xc4) * ld(w0 + 0x11c));
        float forceX = float(double(v[0]) + double(a0));
        float forceY = float(double(v[1]) + double(b0));
        double torque = double(b0) * ld(c0 + 0x90) - double(a0) * ld(c0 + 0x94);
        for (int i = 1; i < 4; ++i)
        {
            const reg32 w = wheel(i), c = contact(i);
            const double a = ld(w + 0xc0) * ld(w + 0x11c) + ld(w + 0x118) * ld(w + 0xb0);
            const double b = ld(w + 0xb4) * ld(w + 0x118) + ld(w + 0xc4) * ld(w + 0x11c);
            forceX = float(double(forceX) + a);
            forceY = float(double(forceY) + b);
            torque += b * ld(c + 0x90);
            torque -= a * ld(c + 0x94);
        }
        const double inverseMass = ld(One) / ld(body + 0x144);
        const float accelX = float(double(forceX) * inverseMass);
        const float accelY = float(inverseMass * double(forceY));
        const float spin = float(torque / (ld(One) / ld(body + 0x178)));
        for (int i = 0; i < 4; ++i)
        {
            const reg32 w = wheel(i), c = contact(i);
            if (rc::ld8(w + 0x154) != 0 || rc::ld32(w + 0x10c) != 0)
                continue;
            const double x = double(accelX) - double(spin) * ld(c + 0x94);
            const double y = double(spin) * ld(c + 0x90) + double(accelY);
            const reg32 info = rc::ld32(w + 0x14);
            const double z = (ld(w + 0x108) - ld(info + 0x10) * ld(w + 0x118)) / ld(w + 0x2c) * ld(info + 0x10);
            const double r = (z - (y * ld(w + 0xc0) + x * ld(w + 0xb0))) / ld(info + 0x10) * ld(w + 0x2c) +
                             ld(w + 0x118);
            stf(w + 0x118, r);
        }
        modifyClutch(rc::ld32(m_mover + 0x4c));
    }

    // 0x587830 Wheel::FinalizeGroundForce.
    void finalizeGround(reg32 w)
    {
        bool boosted = false;
        if (ld(w + 0x108) > ld(Zero) && rc::ld32(w + 0x10c) == 0)
        {
            boosted = true;
            stf(w + 0x118, ld(0x6f0878) * ld(w + 0x118));
        }
        const double fx = ld(w + 0x118), fy = ld(w + 0x11c);
        const float force = float(std::sqrt(fy * fy + fx * fx));
        const reg32 surface = rc::ld32(w + 0x150) * 0x94;
        const reg32 tire = rc::ld32(w + 0x18), car = rc::ld32(w + 0x20);
        float grip = float(ld(FrictionTable - 4 + surface) * ld(tire + 0xc) * ld(w + 0x180) * ld(w + 0x17c) *
                           ld(w + 0x178) * ld(w + 0x158) * ld(LoadScale));
        if (rc::ld32(car + 0x3c0) == 1)
            grip = float(pilotFactor(w) * double(grip));
        if (double(force) > double(grip) && double(force) > ld(0x6cc8bc))
        {
            const double s = double(grip) / double(force);
            const double side = ld(w + 0x11c);
            stf(w + 0x118, s * ld(w + 0x118));
            stf(w + 0x11c, side * s);
        }
        else if (boosted)
            stf(w + 0x118, ld(w + 0x118) / ld(0x6f0878));
        if (double(grip) > ld(0x6cc8bc))
        {
            const double used = double(force) / double(grip);
            stf(w + 0x168, used);
            if (used > ld(One))
                rc::st32(w + 0x168, 0x3f800000u);
        }
        else
            rc::st32(w + 0x168, 0x3f800000u);
        if (rc::ld8(w + 0x154) != 0 || rc::ld32(w + 0x10c) != 0)
            rc::st32(w + 0x168, 0x3f800000u);
        if (rc::ld8(w + 0x155) == 0)
            rc::st32(w + 0x168, 0);

        float v[4];
        groundVelocity(w, v);
        double a = double(sreg32(angle(bitsOf(v[0]), bitsOf(v[1])))) * ld(AngleToRadians);
        if (a > ld(AngleWrap))
            a -= ld(AngleTurn);
        if (double(v[0]) < ld(Zero))
        {
            a += ld(AngleWrap);
            if (a > ld(AngleWrap))
                a -= ld(AngleTurn);
        }
        const reg32 turn = reg32(x86::truncInteger64(a * ld(RadiansToAngle)));
        const double drag = mathfn::sin16(turn & 0xffffu) * ld(w + 0x11c);
        if (std::fabs(double(v[0])) > ld(One))
            stf(w + 0x118, ld(w + 0x118) - drag * ld(Half));

        // The force into the world through the wheel's frame (force frame mode 1, 2 or 4) or as it is.
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const reg32 frame = w + 0x70;
        const reg32 mode = rc::ld32(ForceFrameMode);
        if (mode == 1 || mode == 4)
        {
            const int rows = mode == 1 ? 3 : 2;
            for (int r = 0; r < rows; ++r)
            {
                m[r * 4] = rc::ldf32(frame + 0x10 * reg32(r));
                for (int c = 1; c < 4; ++c)
                    m[r * 4 + c] = float(ld(frame + 0x10 * reg32(r) + 4 * reg32(c)));
            }
            if (mode == 4)
            {
                m[8] = rc::ldf32(w + 0x140);
                m[9] = float(ld(w + 0x144));
                m[10] = float(ld(w + 0x148));
                m[11] = 0;
            }
        }
        else if (mode == 2)
        {
            for (int r = 0; r < 2; ++r)
            {
                const reg32 row = frame + 0x10 * reg32(r);
                const double d = -((ld(w + 0x148) * ld(row + 8) + ld(w + 0x144) * ld(row + 4)) +
                                   ld(row) * ld(w + 0x140));
                m[r * 4] = float(d * ld(w + 0x140) + ld(row));
                m[r * 4 + 1] = float(d * ld(w + 0x144) + ld(row + 4));
                m[r * 4 + 2] = float(d * ld(w + 0x148) + ld(row + 8));
                m[r * 4 + 3] = 0;
            }
            m[8] = rc::ldf32(w + 0x140);
            m[9] = float(ld(w + 0x144));
            m[10] = float(ld(w + 0x148));
            m[11] = 0;
        }
        const float in[4] = {rc::ldf32(w + 0x118), rc::ldf32(w + 0x11c), rc::ldf32(w + 0x158), 1.0f};
        float out[4];
        win32::d3dx9::transform(in, m, out);
        for (reg32 k = 0; k < 4; ++k)
            rc::stf32(w + 0x120 + 4 * k, out[k]);
    }

    win32::WinApplication* const m_app;
    x86::CPU& m_cpu;
    const reg32 m_mover, m_entry, m_scratch;
};

// ---- rigid body ----

// The length of the float vector at v as the guest sums it: (x * x + y * y) + z * z.
double length3(reg32 v)
{
    const double x = ld(v), y = ld(v + 4), z = ld(v + 8);
    return std::sqrt(x * x + y * y + z * z);
}

// D3DXMatrixMultiply (0x5AC69C) of two 4x4 float matrices.
void multiply(const float* a, const float* b, float* r)
{
    win32::d3d9::multiplyFast(a, b, r);
}

// 0x584CD0 RigidBody::ApplyForce: the force f at the point p (both in the world frame, relative to the centre) into
// the body's force sum (+0x110) and torque sum (+0x120), each limited in length. Returns the torque length, which
// the guest compares last.
double applyForce(reg32 body, const float* f, const float* p)
{
    double fx = double(f[0]), fy = double(f[1]);
    float fz = f[2];
    const double length = std::sqrt(double(f[2]) * double(f[2]) + double(f[1]) * double(f[1]) + fx * fx);
    if (length > ld(0x6b5b44))
    {
        const double s = ld(0x6b5b44) / length;
        fx *= s;
        fy *= s;
        fz = float(double(fz) * s);
    }
    stf(body + 0x110, fx + ld(body + 0x110));
    stf(body + 0x114, fy + ld(body + 0x114));
    stf(body + 0x118, double(fz) + ld(body + 0x118));

    double tx = double(f[2]) * double(p[1]) - double(f[1]) * double(p[2]);
    double ty = double(f[0]) * double(p[2]) - double(f[2]) * double(p[0]);
    float tz = float(double(f[1]) * double(p[0]) - double(f[0]) * double(p[1]));
    const double torque = std::sqrt(ty * ty + double(tz) * double(tz) + tx * tx);
    if (torque > ld(0x6b5b48))
    {
        const double s = ld(0x6b5b48) / torque;
        tx *= s;
        ty *= s;
        tz = float(double(tz) * s);
    }
    stf(body + 0x120, tx + ld(body + 0x120));
    stf(body + 0x124, ty + ld(body + 0x124));
    stf(body + 0x128, double(tz) + ld(body + 0x128));
    return torque;
}

// The velocity (+0xA0) from the momentum (+0x70) and the inverse mass (+0x148).
void bodyVelocity(reg32 b)
{
    const double inverseMass = ld(b + 0x148);
    stf(b + 0xa0, inverseMass * ld(b + 0x70));
    stf(b + 0xa4, ld(b + 0x74) * inverseMass);
    stf(b + 0xa8, ld(b + 0x78) * inverseMass);
}

// 0x585810 RigidBody::ComputeAuxiliaries (ecx = the body): the velocity with the momentum limited, the force scaled
// by the inverse mass, the angular velocity through the world inertia and limited, the clamped +0x90, the copy of the
// orientation and position, the cleared force and torque sums, then gravity when +0x140 is set. eax, ecx, edx and the
// x87 condition codes as the guest leaves them; esp past the return address.
void bodyAuxiliaries(x86::CPU& cpu)
{
    const reg32 b = cpu.ecx, entry = cpu.esp;
    bodyVelocity(b);
    const double speed = length3(b + 0xa0);
    if (speed > ld(0x6f0904) && ld(b + 0x144) > rc::ldf64(0x6ccc48))
    {
        const double s = ld(0x6f0904) / double(float(speed));
        stf(b + 0x70, s * ld(b + 0x70));
        stf(b + 0x74, s * ld(b + 0x74));
        stf(b + 0x78, s * ld(b + 0x78));
        bodyVelocity(b);
    }
    const double scale = ld(b + 0x148) * ld(0x6ccc44);
    stf(b + 0xb0, scale * ld(b + 0x110));
    stf(b + 0xb4, ld(b + 0x114) * scale);
    stf(b + 0xb8, ld(b + 0x118) * scale);

    float t[16], m1[16], m2[16], w[4];
    transposed(b + 0x30, t);
    const reg32 inertia = rc::ld32(b + 0x138) != 0 ? b + 0x190 : b + 0x150;
    float source[16], orientation[16];
    memcpy(source, rc::host(inertia), 64);
    memcpy(orientation, rc::host(b + 0x30), 64);
    multiply(source, orientation, m1);
    multiply(t, m1, m2);
    transform3(b + 0x80, m2, w);
    for (reg32 k = 0; k < 4; ++k)
        rc::stf32(b + 0xc0 + 4 * k, w[k]);
    const double spin = length3(b + 0xc0);
    if (spin > ld(0x6b5b88))
    {
        const double s = ld(0x6b5b88) / spin;
        stf(b + 0xc0, s * ld(b + 0xc0));
        stf(b + 0xc4, ld(b + 0xc4) * s);
        stf(b + 0xc8, ld(b + 0xc8) * s);
    }
    cpu.fpu.compare(ld(b + 0x90), ld(Zero));
    if (ld(b + 0x90) < ld(Zero))
        rc::st32(b + 0x90, 0);

    for (reg32 row = 0; row < 3; ++row)
    {
        const reg32 from = b + 0x30 + 0x10 * row, to = b + 0xd0 + 0x10 * row;
        rc::st32(to, rc::ld32(from));
        stf(to + 4, ld(from + 4));
        stf(to + 8, ld(from + 8));
        rc::st32(to + 0xc, rc::ld32(from + 0xc));
    }
    rc::st32(b + 0x100, rc::ld32(b + 0x20));
    stf(b + 0x104, ld(b + 0x24));
    stf(b + 0x108, ld(b + 0x28));
    rc::st32(b + 0x10c, 0x3f800000u);
    rc::st32(b + 0x110, 0);
    rc::st32(b + 0x114, 0);
    rc::st32(b + 0x118, 0);
    rc::st32(b + 0x120, 0);
    stf(b + 0x124, ld(b + 0x114));
    stf(b + 0x128, ld(b + 0x118));
    cpu.esp = entry + 4;
    if (rc::ld32(b + 0x140) == 0)
    {
        cpu.eax = 0;
        cpu.ecx = 0;
        cpu.edx = rc::ld32(b + 0x20);
        return;
    }
    const float force[3] = {0.0f, 0.0f, float(-(ld(0x7000ac) * ld(b + 0x144)))};
    const float point[3] = {0.0f, 0.0f, 0.0f};
    cpu.fpu.compare(applyForce(b, force, point), ld(0x6b5b48));
    // The guest's force vector: its esp (the 16-byte aligned frame less 0xF0) plus 0x10.
    cpu.ecx = ((entry - 4) & ~0xfu) - 0xe0;
    cpu.edx = b;
    cpu.eax = (bitsOf(force[2]) & 0xffff0000u) | cpu.fpu.status.word;
}

// ---- suspension ----

// A 4x4 float matrix stored row by row as 0x401CD0, 0x586770 and 0x586820 copy it (the middle two values of each row
// pass through the x87).
void storeMatrix(const float* m, reg32 to)
{
    for (reg32 r = 0; r < 4; ++r)
    {
        rc::stf32(to + 0x10 * r, m[r * 4]);
        rc::stf32(to + 0x10 * r + 4, float(double(m[r * 4 + 1])));
        rc::stf32(to + 0x10 * r + 8, float(double(m[r * 4 + 2])));
        rc::stf32(to + 0x10 * r + 0xc, m[r * 4 + 3]);
    }
}

// 0x674898 then 0x564B10: the sine of the 16-bit angle truncated from value, with the range compare 0x564B10
// makes last.
double sineOf(x86::CPU& cpu, double value)
{
    const reg32 angle = reg32(x86::truncInteger64(value)) & 0xffffu;
    const double x = double(angle) * ld(AngleToRadians);
    cpu.fpu.compare(x, x >= ld(0x6cca90) ? ld(0x6cca90) : ld(0x6cca40));
    return mathfn::sin16(angle);
}

// 0x588580 Suspension::ComputeAuxiliaries (ecx = the suspension): the attachment point in the world, the terrain
// check 0x587070 (as guest code, from a scratch stack), the compression and its rate, the suspension and wheel
// matrices with the steering turn, the wheel's ground velocity, and the spring force. eax, ecx, edx and the x87
// condition codes as the guest leaves them; esp past the return address.
void suspensionAuxiliaries(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 s = cpu.ecx, entry = cpu.esp;
    const reg32 ebx = cpu.ebx, esi = cpu.esi, edi = cpu.edi, ebp = cpu.ebp;
    const reg32 w = rc::ld32(s + 0xa0), body = rc::ld32(s + 0xa4);

    float orientation[16];
    memcpy(orientation, rc::host(body + 0x30), 64);
    float p[4];
    transform3(s + 0x90, orientation, p);
    p[0] = float(double(p[0]) + ld(body + 0x20));
    p[1] = float(ld(body + 0x24) + double(p[1]));
    p[2] = float(ld(body + 0x28) + double(p[2]));

    const reg32 scratch = (entry - 0x400) & ~0xfu;
    const reg32 point = scratch + 0x40;
    for (reg32 k = 0; k < 3; ++k)
        rc::stf32(point + 4 * k, p[k]);
    rc::st32(scratch + 4, w);
    rc::st32(scratch + 8, w + 0x150);
    rc::st32(scratch + 0xc, body + 0x30);
    cpu.esp = scratch;
    cpu.esi = w + 0x140;
    cpu.edi = point;
    sub_587070(app, cpu);
    const double height = double(cpu.fpu.st(0)) + ld(rc::ld32(w + 0x14) + 0x10);
    cpu.fpu.pop();

    const reg32 info = rc::ld32(s + 0x14);
    const reg32 previous = rc::ld32(s + 0x1c);
    reg32 overshoot = 0;
    reg32 contact;
    cpu.fpu.compare(height, ld(info + 0x14));
    if (height > ld(info + 0x14))
    {
        rc::st32(s + 0x1c, rc::ld32(info + 0x14));
        const double over = height - ld(s + 0x1c);
        contact = 1;
        overshoot = bitsOf(float(over));
        cpu.fpu.compare(over, ld(0x6f0740));
        if (over > ld(0x6f0740))
            overshoot = rc::ld32(0x6f0740);
    }
    else
    {
        cpu.fpu.compare(height, ld(info + 0x18));
        if (height < ld(info + 0x18))
        {
            rc::st32(s + 0x1c, rc::ld32(info + 0x18));
            contact = 0;
        }
        else
        {
            stf(s + 0x1c, height);
            contact = 1;
        }
    }
    rc::st32(s + 0x80, contact);
    cpu.fpu.compare(ld(0x6f0738), ld(0x6ccab0));
    if (ld(0x6f0738) > ld(0x6ccab0))
        stf(s + 0x20, (ld(s + 0x1c) - double(floatOf(previous))) / ld(0x6f0738));

    rc::st32(s + 0x30, rc::ld32(s + 0x90));
    stf(s + 0x34, ld(s + 0x94));
    stf(s + 0x38, ld(s + 0x98));
    stf(s + 0x38, ld(s + 0x38) + ld(s + 0x1c));
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float steer = rc::ldf32(w + 0x16c);
    cpu.fpu.compare(double(steer), ld(Zero));
    if (!(double(steer) == ld(Zero)))
    {
        const double turn = sineOf(cpu, ld(w + 0x24) * ld(RadiansToAngle)) * ld(0x6f0748) * double(steer) *
                            ld(RadiansToAngle);
        const float c = float(sineOf(cpu, turn + ld(0x6ccba8)));
        const double sine = sineOf(cpu, double(float(turn)));
        float r[16] = {c, float(sine), 0, 0, float(-sine), c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float product[16];
        multiply(r, m, product);
        memcpy(m, product, 64);
    }
    m[12] = rc::ldf32(s + 0x30);
    m[13] = float(ld(s + 0x34));
    m[14] = float(ld(s + 0x38));
    m[15] = 1.0f;
    storeMatrix(m, s + 0x40);
    const reg32 frames = rc::ld32(s + 0xa8);
    if (frames != 0)
    {
        float x[16], product[16];
        memcpy(x, rc::host(rc::ld32(frames + 8) + (rc::ld32(s + 0xac) << 6)), 64);
        multiply(x, m, product);
        memcpy(m, product, 64);
    }
    storeMatrix(m, w + 0xb0);

    float v[4];
    transform3(s + 0x30, orientation, v);
    const double wx = ld(body + 0xc0), wy = ld(body + 0xc4), wz = ld(body + 0xc8);
    const double a = double(v[2]) * wy - double(v[1]) * wz;
    const float bv = float(double(v[0]) * wz - double(v[2]) * wx);
    const float cv = float(double(v[1]) * wx - double(v[0]) * wy);
    stf(w + 0xf0, a + ld(body + 0xa0));
    stf(w + 0xf4, ld(body + 0xa4) + double(bv));
    stf(w + 0xf8, ld(body + 0xa8) + double(cv));
    float world[16], product[16];
    memcpy(world, rc::host(body + 0xd0), 64);
    multiply(m, world, product);
    storeMatrix(product, w + 0x70);

    if (contact != 0)
    {
        const double compression = ld(s + 0x1c) > ld(Zero) ? ld(s + 0x1c) : ld(Zero);
        const double stiffness = (compression * ld(info) + ld(One)) * ld(info + 4);
        const reg32 side = ld(s + 0x20) > ld(Zero) ? 0 : 1;
        const reg32 other = rc::ld32(s + 0x18);
        double force = ld(info + side * 4 + 8) * ld(s + 0x20) + stiffness * ld(s + 0x1c) + ld(s + 0x88);
        force += (ld(s + 0x1c) - ld(other + 0x1c)) * ld(info + 0x10) + ld(0x734538) * double(floatOf(overshoot));
        stf(s + 0x24, force);
        cpu.fpu.compare(force, ld(Zero));
        if (force < ld(Zero))
            rc::st32(s + 0x24, 0);
        cpu.eax = cpu.fpu.status.word;
        cpu.ecx = info;
        cpu.edx = other;
    }
    else
    {
        rc::st32(s + 0x24, 0);
        cpu.eax = bitsOf(product[12]);
        cpu.ecx = bitsOf(product[15]);
        cpu.edx = bitsOf(product[15]);
    }
    cpu.ebx = ebx;
    cpu.esi = esi;
    cpu.edi = edi;
    cpu.ebp = ebp;
    cpu.esp = entry + 4;
}

// 0x585360 VectorStarMatrix: the 4x4 matrix (rows of 4 floats at r) of the cross product of the vector v with each
// row of the matrix m.
void vectorStar(reg32 m, reg32 v, reg32 r)
{
    const double v0 = ld(v), v1 = ld(v + 4), v2 = ld(v + 8);
    for (reg32 row = 0; row < 3; ++row)
    {
        const reg32 a = m + 0x10 * row;
        stf(r + 0x10 * row, ld(a + 8) * v1 - v2 * ld(a + 4));
        stf(r + 0x10 * row + 4, v2 * ld(a) - ld(a + 8) * v0);
        stf(r + 0x10 * row + 8, v0 * ld(a + 4) - ld(a) * v1);
        rc::st32(r + 0x10 * row + 0xc, 0);
    }
    rc::st32(r + 0x30, 0);
    rc::st32(r + 0x34, 0);
    rc::st32(r + 0x38, 0);
    rc::st32(r + 0x3c, 0x3f800000u);
}

// value * (x, y, z) added to the three floats at sum, as the guest rounds them: the first sum in double, the other
// two products stored as floats first.
void addScaled(reg32 sum, double value, reg32 v)
{
    const double x = value * ld(v);
    const float y = float(ld(v + 4) * value);
    const float z = float(ld(v + 8) * value);
    stf(sum, x + ld(sum));
    stf(sum + 4, double(y) + ld(sum + 4));
    stf(sum + 8, double(z) + ld(sum + 8));
}

// 0x585410 RigidBody::GetDerivative (ecx = the body, d = the derivative): the downforce-side force correction when
// +0x1E4 is set, then velocity, the spin matrix, force, torque with the table damping, and +0x70. The two table
// lookups run through 0x583EA0 as the guest calls them (from a scratch stack), so eax, ecx and edx are those the guest
// leaves; esp past the argument.
void bodyDerivative(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 b = cpu.ecx, entry = cpu.esp, d = rc::ld32(entry + 4), esi = cpu.esi;
    const reg32 scratch = (entry - 0x400) & ~0xfu;
    auto table = [&](reg32 values, float argument) {
        rc::stf32(scratch + 4, argument);
        cpu.esp = scratch;
        cpu.esi = values;
        sub_583ea0(app, cpu);
        const double r = double(cpu.fpu.st(0));
        cpu.fpu.pop();
        return r;
    };
    if (!(ld(b + 0x1e4) == ld(Zero)))
    {
        double k = ld(b + 0x1e8) * ld(b + 0x1e4) + ld(0x6ccbd0);
        if (rc::ld32(b + 0x13c) != 0)
            k *= ld(0x6b5b84);
        float pitch = rc::ldf32(b + 0x38);
        double f;
        if (double(pitch) > ld(Zero))
        {
            if (double(pitch) > ld(0x6b5b60))
                pitch = rc::ldf32(0x6b5b60);
            f = ld(One) - (ld(One) - ld(0x6b5b5c)) / ld(0x6b5b60) * double(pitch);
        }
        else
        {
            pitch = float(-double(pitch));
            if (double(pitch) > ld(0x6b5b58))
                pitch = rc::ldf32(0x6b5b58);
            f = (ld(0x6b5b54) - ld(One)) / ld(0x6b5b58) * double(pitch) + ld(One);
        }
        k *= f;
        const double vx = ld(b + 0xa0), vy = ld(b + 0xa4);
        const double planar = std::sqrt(vx * vx + vy * vy);
        if (planar * ld(0x6ccbcc) > k &&
            ld(b + 0x38) * ld(b + 0x118) + ld(b + 0x34) * ld(b + 0x114) + ld(b + 0x30) * ld(b + 0x110) > ld(Zero))
        {
            float t[16], local[4], world[4];
            transposed(b + 0x30, t);
            transform3(b + 0x110, t, local);
            double g = -(ld(0x6b5b68) * double(pitch));
            if (g > ld(0x6b5b64))
                g = ld(0x6b5b64);
            local[0] = float(g);
            float orientation[16];
            memcpy(orientation, rc::host(b + 0x30), 64);
            const float in[4] = {local[0], local[1], local[2], 1.0f};
            win32::d3dx9::transform(in, orientation, world);
            rc::stf32(b + 0x110, world[0]);
            rc::stf32(b + 0x114, world[1]);
            rc::stf32(b + 0x118, world[2]);
        }
    }

    rc::st32(d, rc::ld32(b + 0xa0));
    stf(d + 4, ld(b + 0xa4));
    stf(d + 8, ld(b + 0xa8));
    vectorStar(b + 0x30, b + 0xc0, d + 0x10);
    rc::st32(d + 0x50, rc::ld32(b + 0x110));
    stf(d + 0x54, ld(b + 0x114));
    stf(d + 0x58, ld(b + 0x118));
    rc::st32(d + 0x60, rc::ld32(b + 0x120));
    stf(d + 0x64, ld(b + 0x124));
    stf(d + 0x68, ld(b + 0x128));

    const float spin = float(length3(b + 0xc0));
    cpu.eax = rc::ld32(b + 0x134);
    cpu.ecx = b + 0xc0;
    cpu.edx = rc::ld32(b + 0x110);
    if (rc::ld32(b + 0x134) != 0)
    {
        cpu.ecx = bitsOf(spin);
        addScaled(d + 0x60, -table(0x704478, spin), b + 0x80);
    }
    float speed = float(length3(b + 0xa0) / ld(0x6b5b6c));
    const double spinShare = double(spin) / ld(0x6b5b70);
    if (spinShare > double(speed))
        speed = float(spinShare);
    cpu.edx = bitsOf(speed);
    const double damping = table(0x70448c, speed);
    cpu.fpu.compare(damping, ld(b + 0x90));
    if (damping > ld(b + 0x90))
        rc::st32(d + 0x70, rc::ld32(0x6b5b7c));
    else
        stf(d + 0x70, -ld(0x6b5b80));
    if (rc::ld32(b + 0x130) != 0)
    {
        addScaled(d + 0x60, -(ld(0x6b5b78) * ld(b + 0x90)), b + 0x80);
        addScaled(d + 0x50, -(ld(0x6b5b74) * ld(b + 0x90)), b + 0x70);
    }
    if (!(ld(b + 0x1e0) == ld(Zero)))
    {
        cpu.fpu.compare(ld(b + 0x128), ld(b + 0x1e0));
        if (ld(b + 0x128) > ld(b + 0x1e0))
        {
            cpu.ecx = rc::ld32(b + 0x1e0);
            rc::st32(d + 0x68, cpu.ecx);
        }
        else
        {
            cpu.fpu.compare(-ld(b + 0x1e0), ld(b + 0x128));
            if (-ld(b + 0x1e0) > ld(b + 0x128))
                stf(d + 0x68, -ld(b + 0x1e0));
        }
    }
    else
        cpu.fpu.compare(ld(b + 0x1e0), ld(Zero));
    cpu.eax = d;
    cpu.esi = esi;
    cpu.esp = entry + 8;
}

// ---- shadow compare ----

const uint32_t ShadowLogLimit = 20;
uint32_t s_forcesCompared = 0, s_forcesDiffering = 0, s_forcesLogged = 0;

struct Span
{
    reg32 address;
    reg32 size;
};

// The mover, its rigid body, wheels, drive train and engine.
std::vector<Span> forcesSpans(reg32 mover)
{
    std::vector<Span> spans;
    spans.push_back(Span{mover, 0x60});
    spans.push_back(Span{rc::ld32(mover + 0x20), 0x200});
    for (reg32 i = 0; i < 4; ++i)
    {
        spans.push_back(Span{rc::ld32(mover + 0x38 + 4 * i), 0x180});
        spans.push_back(Span{rc::ld32(mover + 0x28 + 4 * i), 0x100});
    }
    const reg32 d = rc::ld32(mover + 0x4c);
    spans.push_back(Span{d, 0x100});
    spans.push_back(Span{rc::ld32(d + 0x20), 0x100});
    return spans;
}

void save(const std::vector<Span>& spans, std::vector<x86::reg8>& bytes)
{
    bytes.clear();
    for (const Span& s : spans)
        bytes.insert(bytes.end(), rc::host(s.address), rc::host(s.address) + s.size);
}

void restore(const std::vector<Span>& spans, const std::vector<x86::reg8>& bytes)
{
    size_t at = 0;
    for (const Span& s : spans)
    {
        memcpy(rc::host(s.address), bytes.data() + at, s.size);
        at += s.size;
    }
}

void forcesDifference(reg32 caller, const char* text)
{
    ++s_forcesDiffering;
    if (s_forcesLogged < ShadowLogLimit)
    {
        ++s_forcesLogged;
        pf::logInfo("shadow_forces: IterateForces 0x478570 (caller 0x%08x): %s", unsigned(caller), text);
    }
}

// The guest runs first; the spans are put back and the native code runs from the same entry state, then the guest's
// memory is put back again.
void shadowForces(win32::WinApplication* app, x86::CPU& cpu)
{
    static std::vector<x86::reg8> before, guestAfter, nativeAfter;
    const std::vector<Span> spans = forcesSpans(cpu.edi);
    save(spans, before);
    const x86::CPU entry = cpu;
    const reg32 caller = rc::ld32(cpu.esp);
    sub_478570_guest(app, cpu);
    if (cpu.terminate)
        return;
    save(spans, guestAfter);
    restore(spans, before);
    x86::CPU n = entry;
    Forces forces(app, n);
    const bool ran = forces.run();
    save(spans, nativeAfter);
    restore(spans, guestAfter);
    if (!ran)
        return;
    if (n.terminate)
    {
        cpu.terminate = true;
        return;
    }
    ++s_forcesCompared;
    char text[160] = {};
    if (cpu.fpu.count != n.fpu.count)
    {
        snprintf(text, sizeof(text), "x87 depth guest %u native %u", unsigned(cpu.fpu.count), unsigned(n.fpu.count));
        forcesDifference(caller, text);
        return;
    }
    if (!retstate::sameFlags(retstate::flags(0x478570), cpu, n, text, sizeof(text)) ||
        !retstate::sameRegisters(retstate::registers(0x478570), cpu, n, text, sizeof(text)))
    {
        forcesDifference(caller, text);
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
                snprintf(text, sizeof(text), "dword at 0x%08x (+0x%x) guest 0x%08x native 0x%08x",
                         unsigned(s.address + k), unsigned(k), unsigned(a), unsigned(b));
                forcesDifference(caller, text);
                return;
            }
        }
        at += s.size;
    }
}

struct ShadowCount
{
    const char* setting;
    const char* function;
    uint32_t compared, differing, logged;
};

ShadowCount s_bodyAuxShadow = {"shadow_rigid_body", "RigidBody::ComputeAuxiliaries 0x585810", 0, 0, 0};
ShadowCount s_bodyDerivShadow = {"shadow_rigid_body", "RigidBody::GetDerivative 0x585410", 0, 0, 0};

// The guest runs first; the spans are put back and native runs from the same entry state, then the guest's memory is
// put back again. The x87 depth, the registers and flags retstate lists for address, and the span bytes are compared.
template <typename Native>
void shadowRun(win32::WinApplication* app, x86::CPU& cpu, void (*guest)(win32::WinApplication*, x86::CPU&),
               reg32 address, const std::vector<Span>& spans, ShadowCount& count, Native native)
{
    static std::vector<x86::reg8> before, guestAfter, nativeAfter;
    save(spans, before);
    const x86::CPU entry = cpu;
    const reg32 caller = rc::ld32(cpu.esp);
    guest(app, cpu);
    if (cpu.terminate)
        return;
    save(spans, guestAfter);
    restore(spans, before);
    x86::CPU n = entry;
    native(n);
    save(spans, nativeAfter);
    restore(spans, guestAfter);
    ++count.compared;
    char text[160] = {};
    bool same = true;
    if (cpu.fpu.count != n.fpu.count)
    {
        snprintf(text, sizeof(text), "x87 depth guest %u native %u", unsigned(cpu.fpu.count), unsigned(n.fpu.count));
        same = false;
    }
    size_t at = 0;
    for (size_t i = 0; same && i < spans.size(); ++i)
    {
        const Span& s = spans[i];
        for (reg32 k = 0; k + 4 <= s.size; k += 4)
        {
            reg32 a, b;
            memcpy(&a, guestAfter.data() + at + k, 4);
            memcpy(&b, nativeAfter.data() + at + k, 4);
            if (a != b)
            {
                snprintf(text, sizeof(text), "dword at 0x%08x (+0x%x) guest 0x%08x native 0x%08x",
                         unsigned(s.address + k), unsigned(k), unsigned(a), unsigned(b));
                same = false;
                break;
            }
        }
        at += s.size;
    }
    if (same && (!retstate::sameFlags(retstate::flags(address), cpu, n, text, sizeof(text)) ||
                 !retstate::sameRegisters(retstate::registers(address), cpu, n, text, sizeof(text))))
        same = false;
    if (same)
        return;
    ++count.differing;
    if (count.logged < ShadowLogLimit)
    {
        ++count.logged;
        pf::logInfo("%s: %s (caller 0x%08x): %s", count.setting, count.function, unsigned(caller), text);
    }
}

const reg32 BodySize = 0x200, DerivativeSize = 0x74, SuspensionSize = 0xb0, WheelSize = 0x190;

ShadowCount s_suspensionShadow = {"shadow_suspension", "Suspension::ComputeAuxiliaries 0x588580", 0, 0, 0};

}

// Suspension::ComputeAuxiliaries (guest 0x588580).
void sub_588580(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    if (!s.suspension)
    {
        sub_588580_guest(app, cpu);
        return;
    }
    auto native = [app](x86::CPU& c) { suspensionAuxiliaries(app, c); };
    if (__builtin_expect(s.suspensionShadow, 0))
    {
        shadowRun(app, cpu, sub_588580_guest, 0x588580,
                  {Span{cpu.ecx, SuspensionSize}, Span{rc::ld32(cpu.ecx + 0xa0), WheelSize}}, s_suspensionShadow,
                  native);
        return;
    }
    native(cpu);
}

// RigidBody::ComputeAuxiliaries (guest 0x585810).
void sub_585810(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    if (!s.body)
    {
        sub_585810_guest(app, cpu);
        return;
    }
    auto native = [](x86::CPU& c) { bodyAuxiliaries(c); };
    if (__builtin_expect(s.bodyShadow, 0))
    {
        shadowRun(app, cpu, sub_585810_guest, 0x585810, {Span{cpu.ecx, BodySize}}, s_bodyAuxShadow, native);
        return;
    }
    native(cpu);
}

// RigidBody::GetDerivative (guest 0x585410).
void sub_585410(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    if (!s.body)
    {
        sub_585410_guest(app, cpu);
        return;
    }
    auto native = [app](x86::CPU& c) { bodyDerivative(app, c); };
    if (__builtin_expect(s.bodyShadow, 0))
    {
        shadowRun(app, cpu, sub_585410_guest, 0x585410,
                  {Span{cpu.ecx, BodySize}, Span{rc::ld32(cpu.esp + 4), DerivativeSize}}, s_bodyDerivShadow, native);
        return;
    }
    native(cpu);
}

// PhysicsMover::IterateForces (guest 0x478570).
void sub_478570(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    if (!s.forces)
    {
        sub_478570_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.forcesShadow, 0))
    {
        shadowForces(app, cpu);
        return;
    }
    Forces forces(app, cpu);
    if (!forces.run())
        sub_478570_guest(app, cpu);
}

}

namespace physics
{

void logShadowTotal()
{
    if (native::settings().forces && native::settings().forcesShadow)
        pf::logInfo("shadow_forces: %u calls compared, %u differ", unsigned(native::s_forcesCompared),
                    unsigned(native::s_forcesDiffering));
    if (native::settings().body && native::settings().bodyShadow)
        for (const native::ShadowCount* c : {&native::s_bodyAuxShadow, &native::s_bodyDerivShadow})
            pf::logInfo("%s: %s: %u calls compared, %u differ", c->setting, c->function, unsigned(c->compared),
                        unsigned(c->differing));
    if (native::settings().suspension && native::settings().suspensionShadow)
        for (const native::ShadowCount* c : {&native::s_suspensionShadow})
            pf::logInfo("%s: %s: %u calls compared, %u differ", c->setting, c->function, unsigned(c->compared),
                        unsigned(c->differing));
}

}}
