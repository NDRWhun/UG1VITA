// Native car rendering functions (cars.h). Each port takes the CPU as the guest call leaves it (esp at the return
// address slot), keeps its values in host variables, calls other guest functions with the guest's stack and registers,
// and sets the registers, esp and x87 state the guest code leaves at its return.

#include "cars.h"
#include "draw.h"
#include "retstate.h"

#include <platform/config.h>
#include <platform/log.h>
#include <platform/sync.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <recomp.h>
#include <vector>
#include <winapi/d3d9_internal.h>
#include <winapi/d3d9_native.h>
#include <winapi/d3dx9.h>

namespace nfsu
{

// Generated functions: the overridden guest bodies.
void sub_569460_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_56cf10_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_56d0c0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_56d700_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_571050_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_570db0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_570d00_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_570950_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

// Native overrides the ports call with the guest convention.
void sub_564b10(win32::WinApplication* app, x86::CPU& cpu);
void sub_564bd0(win32::WinApplication* app, x86::CPU& cpu);
void sub_564db0(win32::WinApplication* app, x86::CPU& cpu);
void sub_567370(win32::WinApplication* app, x86::CPU& cpu);
void sub_440a40(win32::WinApplication* app, x86::CPU& cpu);
void sub_56dc30(win32::WinApplication* app, x86::CPU& cpu);
void sub_5447c0(win32::WinApplication* app, x86::CPU& cpu);
void sub_40db20(win32::WinApplication* app, x86::CPU& cpu);
void sub_674898(win32::WinApplication* app, x86::CPU& cpu);
void sub_40c910(win32::WinApplication* app, x86::CPU& cpu);

}

namespace cars
{

bool g_tap = false;

namespace
{

using x86::reg8;
using x86::reg16;
using x86::reg32;
using x86::sreg8;
using x86::sreg16;
using x86::sreg32;
namespace bridge = win32::d3d9::bridge;
typedef void (*Function)(win32::WinApplication*, x86::CPU&);

// Float constants in .rdata.
const reg32 Zero = 0x6cc7a4;
const reg32 One = 0x6cc7bc;

// Guest data.
const reg32 Arena = 0x7355b4;           // frame arena: cursor, limit, ..., overflow flag +0x10, overflow bytes +0x14
const reg32 ArenaLimit = 0x7355b8;
const reg32 ArenaOverflow = 0x7355c4;
const reg32 ArenaOverflowBytes = 0x7355c8;

const uint32_t ShadowLogLimit = 20;

inline double ld(reg32 address) { return double(rc::ldf32(address)); }
inline reg32 bits(float value) { reg32 r; memcpy(&r, &value, 4); return r; }
inline float real(reg32 value) { float r; memcpy(&r, &value, 4); return r; }
float real(float) = delete;
float real(double) = delete;
// A float loaded and stored again through the FPU.
inline reg32 refloat(reg32 value) { return bits(float(double(real(value)))); }

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("cars_native", true), pf::Config::getBool("shadow_cars", false) };
        pf::logInfo("cars: car rendering %s%s", r.native ? "native" : "guest", r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// ---- guest stores ----

struct Store
{
    reg32 address, size;
};

// Shadow compares: the native run logs its stores outside the stack (the 64 KiB below the entry esp).
bool s_logStores = false;
reg32 s_stackTop = 0;
std::vector<Store> s_stores;

inline void logStore(reg32 address, reg32 size)
{
    if (__builtin_expect(s_logStores, 0) && s_stackTop - address - 1 >= 0x10000u)
        s_stores.push_back(Store{address, size});
}
inline void st32(reg32 address, reg32 value)
{
    logStore(address, 4);
    rc::st32(address, value);
}
inline void st8(reg32 address, reg8 value)
{
    logStore(address, 1);
    rc::st8(address, value);
}
inline void stf32(reg32 address, float value)
{
    st32(address, bits(value));
}

// ---- calls of other functions ----

// Memory a called function writes that its caller reads afterwards.
struct Output
{
    reg32 address, size;
};

// The stack argument dwords of a called function and the memory it writes for its caller.
uint32_t calleeArguments(reg32 address)
{
    switch (address)
    {
    case 0x440a40: return 5;
    case 0x56dc30: return 12;
    case 0x5447c0: return 6;
    case 0x40c910: return 5;
    default: return 0;
    }
}

void calleeOutputs(reg32 address, const x86::CPU& cpu, std::vector<Output>& outputs)
{
    outputs.clear();
    const reg32 esp = cpu.esp;
    switch (address)
    {
    case 0x440a40:
        // Terrain type, normal and the found flag, when given.
        if (const reg32 p = rc::ld32(esp + 0xc))
            outputs.push_back(Output{p, 4});
        if (const reg32 p = rc::ld32(esp + 0x10))
            outputs.push_back(Output{p, 12});
        if (const reg32 p = rc::ld32(esp + 0x14))
            outputs.push_back(Output{p, 1});
        break;
    default:
        break;
    }
}

// The registers a called function reads (retstate bits), compared at the call.
uint8_t calleeRegisters(reg32 address)
{
    switch (address)
    {
    case 0x5447c0: return retstate::Eax;
    case 0x40c910: return retstate::Eax;
    default: return 0;
    }
}

const reg32& registerOf(const x86::CPU& cpu, int index)
{
    const reg32* const r[8] = { &cpu.eax, &cpu.ecx, &cpu.edx, &cpu.ebx, &cpu.esp, &cpu.ebp, &cpu.esi, &cpu.edi };
    return *r[index];
}

// Memory a called function reads that its caller wrote for it, compared at the call.
void calleeInputs(reg32 address, const x86::CPU& cpu, std::vector<Output>& inputs)
{
    inputs.clear();
    const reg32 esp = cpu.esp;
    switch (address)
    {
    case 0x5447c0:
        // The flare matrix.
        inputs.push_back(Output{cpu.eax, 0x40});
        break;
    case 0x40c910:
        // The model matrix.
        inputs.push_back(Output{rc::ld32(esp + 0xc), 0x40});
        break;
    case 0x56dc30:
        // The body matrix and the two matrix arrays of the wheels.
        inputs.push_back(Output{rc::ld32(esp + 0x10), 0x40});
        inputs.push_back(Output{rc::ld32(esp + 0x14), 0x100});
        inputs.push_back(Output{rc::ld32(esp + 0x18), 0x100});
        break;
    default:
        break;
    }
}

// A call recorded while a shadow compare runs the guest code: what it was called with and what it left.
struct TapCall
{
    reg32 address, esp, depth, position;
    reg32 registers[8];
    std::vector<reg32> arguments;
    std::vector<Output> inputs;
    std::vector<reg8> inputBytes;
    x86::CPU after;
    std::vector<Output> outputs;
    std::vector<reg8> bytes;
};

enum TapMode
{
    TapOff,
    TapRecord,      // the guest run: calls of the tapped functions run and are recorded
    TapReplay       // the native run: calls are compared with the recorded ones and do not run
};

// One shadow compare; a compare of an outer function runs the compares of the functions it calls inside its recorded
// calls, one level deeper.
struct Shadow
{
    bridge::Capture capture;
    std::vector<draw::Span> spans;
    std::vector<reg8> before, guestAfter, nativeAfter;
    std::vector<TapCall> taps;
    TapMode mode = TapOff;
    bool inTap = false;
    int thread = 0;                 // the thread of the guest run
    uint32_t next = 0;              // replay: the next recorded call
    bool differs = false;           // replay: the first difference
    char text[200] = {};
};

const int ShadowLevels = 3;
Shadow s_shadows[ShadowLevels];
int s_level = -1;                   // the innermost running compare
uint32_t s_depth = 0;               // a shadow compare's guest run is running
uint32_t s_calls = 0, s_differing = 0;
uint32_t s_portCalls[PortCount] = {}, s_portDiffering[PortCount] = {}, s_portLogged[PortCount] = {};

inline Shadow* current()
{
    return s_level >= 0 ? &s_shadows[s_level] : nullptr;
}

void tapDiffers(const char* format, unsigned a, unsigned b, unsigned c)
{
    Shadow& s = *current();
    if (s.differs)
        return;
    s.differs = true;
    snprintf(s.text, sizeof(s.text), format, a, b, c);
}

// A call of another guest function from native code, esp at its return address slot (already stored): runs it, or in
// a shadow compare's native run compares it with the recorded call and leaves the recorded state.
void callee(reg32 address, Function function, win32::WinApplication* app, x86::CPU& cpu)
{
    Shadow* level = current();
    if (__builtin_expect(!level || level->mode != TapReplay, 1))
    {
        function(app, cpu);
        return;
    }
    Shadow& s = *level;
    const uint32_t index = s.next++;
    if (index >= s.taps.size())
    {
        tapDiffers("call %u of 0x%08x: not made by the guest%s", index, address, 0);
        cpu.esp += 4;
        return;
    }
    const TapCall& t = s.taps[index];
    if (t.address != address)
        tapDiffers("call %u: guest 0x%08x native 0x%08x", index, t.address, address);
    else if (t.esp != cpu.esp)
        tapDiffers("call %u: esp guest 0x%08x native 0x%08x", index, t.esp, cpu.esp);
    else if (t.depth != cpu.fpu.count)
        tapDiffers("call %u: x87 depth guest %u native %u", index, t.depth, cpu.fpu.count);
    else if (t.position != s.capture.replayed)
        tapDiffers("call %u: after device call %u guest, %u native", index, t.position, s.capture.replayed);
    else
    {
        const uint8_t mask = calleeRegisters(address);
        for (int r = 0; r < 8; ++r)
        {
            if ((mask & (1 << r)) && registerOf(cpu, r) != t.registers[r])
            {
                tapDiffers("call %u: register %u guest 0x%08x", index, unsigned(r), t.registers[r]);
                break;
            }
        }
        for (uint32_t i = 0; i < t.arguments.size(); ++i)
        {
            const reg32 a = rc::ld32(cpu.esp + 4 + 4 * i);
            if (a != t.arguments[i])
            {
                tapDiffers("call %u: argument %u guest 0x%08x", index, i, t.arguments[i]);
                break;
            }
        }
        size_t at = 0;
        for (const Output& in : t.inputs)
        {
            for (reg32 i = 0; i < in.size; ++i, ++at)
            {
                if (rc::ld8(in.address + i) != t.inputBytes[at])
                {
                    tapDiffers("call %u: input byte at 0x%08x guest 0x%02x", index, in.address + i,
                               t.inputBytes[at]);
                    break;
                }
            }
        }
    }
    const bool terminate = cpu.terminate;
    cpu = t.after;
    cpu.terminate = terminate;
    size_t at = 0;
    for (const Output& o : t.outputs)
    {
        memcpy(rc::host(o.address), t.bytes.data() + at, o.size);
        at += o.size;
    }
}

// Calls function as a guest call at return address slot esp, as callee does.
inline void call(reg32 address, Function function, win32::WinApplication* app, x86::CPU& cpu, reg32 esp, reg32 ret)
{
    rc::st32(esp, ret);
    cpu.esp = esp;
    callee(address, function, app, cpu);
}

// ---- math helpers ----

// The x87 compare of a and b, with the status word fnstsw stores.
inline reg16 compare(x86::CPU& cpu, double a, double b)
{
    cpu.fpu.compare(a, b);
    return cpu.fpu.status.word;
}

// D3DXMatrixMultiply and D3DXVec3Transform as their overrides compute them.
inline void multiply(const float* a, const float* b, float* out)
{
    win32::d3d9::multiplyFast(a, b, out);
}

inline void transform3(const float v[3], const float* m, float out[4])
{
    const float in[4] = { v[0], v[1], v[2], 1.0f };
    win32::d3dx9::transform(in, m, out);
}

// ---- CarPartCuller::CullParts 0x569460 ----

const reg32 PartPlanes = 0x70467c;      // 14 part groups of 0x50 bytes: mode at -4, plane count, planes, distances
const reg32 PartPlanesEnd = 0x704adc;
const reg32 PartNormalize = 0x7345d4;   // the plane normals are normalised before use when set
const reg32 PartAngleScale = 0x6f1a7c;

// eax = the car position, [esp+4] = the culler (0x20 bytes per group), [esp+8] = the 16-bit car angle. ret 8.
void partCull(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 s = cpu.esp;
    const reg32 position = cpu.eax;
    const reg32 culler = rc::ld32(s + 4);
    reg32 angle = rc::ld32(s + 8);
    const reg32 f = (((s - 4) & ~15u) - 0xf4) - 12;     // esp of the body

    // 0x40b930: the rotation by -angle about z.
    const reg32 turn = 0u - angle;
    cpu.eax = turn + 0x4000;
    cpu.esp = f - 0xc;
    native::sub_564b10(app, cpu);
    const reg32 c = bits(float(double(cpu.fpu.pop())));
    cpu.eax = turn;
    cpu.esp = f - 0xc;
    native::sub_564b10(app, cpu);
    const double sine = double(cpu.fpu.pop());
    const reg32 sn = bits(float(sine)), msn = bits(float(-sine));
    const reg32 rotation[16] = { c, sn, 0, 0, msn, c, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000 };
    const reg32 identity[16] = { 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000 };
    float r[16], m[16], p[3], moved[4];
    float id[16];
    memcpy(r, rotation, 64);
    memcpy(id, identity, 64);
    multiply(id, r, m);
    memcpy(p, rc::host(position), 12);
    transform3(p, m, moved);

    reg32 cursor = rc::ld32(Arena) + 0x10;
    reg32 eax = 0, ecx = 0;
    reg32 group = culler;
    for (reg32 e = PartPlanes; e < PartPlanesEnd; e += 0x50, group += 0x20)
    {
        const reg32 saved = rc::ld32(e + 0x34);
        reg32 distance = saved;     // the first plane's distance as this group reads it
        if (cursor >= rc::ld32(ArenaLimit))
        {
            eax = rc::ld32(ArenaOverflowBytes) + 0x10;
            st32(ArenaOverflow, 1);
            st32(ArenaOverflowBytes, eax);
        }
        else
        {
            eax = 0x10;
            st32(Arena, rc::ld32(Arena) + 0x10);
            cursor += 0x10;
        }
        const bool moving = !(e < 0x70485c || e > 0x7048ac);
        if (e <= 0x7046cc)
        {
            if (reg16(angle) > 0x8000)
            {
                angle = 0u - angle;
                st32(s + 8, angle);
            }
            eax = reg16(angle);
            distance = bits(float(double(sreg32(eax)) / ld(PartAngleScale) + double(real(saved))));
        }
        const reg32 count = rc::ld32(e);
        eax = count;
        reg8 visible = 1;
        if (sreg32(count) > 0)
        {
            float point[3];
            if (moving)
                memcpy(point, moved, 12);
            else
                memcpy(point, rc::host(position), 12);
            const float dx = float(double(point[0]) - ld(group));
            const float dy = float(double(point[1]) - ld(group + 4));
            const float dz = float(double(point[2]) - ld(group + 8));
            const reg32 mode = rc::ld32(e - 4);
            if (mode == 0 || mode == 1)
            {
                const reg32 normal = mode == 0 ? f + 0x50 : f + 0x40;
                reg32 k = 0;
                for (; k < count; ++k)
                {
                    const reg32 plane = e + 4 + 0x10 * k;
                    rc::st32(normal, refloat(rc::ld32(plane)));
                    rc::st32(normal + 4, refloat(rc::ld32(plane + 4)));
                    rc::st32(normal + 8, rc::ld32(plane + 8));
                    if (rc::ld32(PartNormalize) != 0)
                    {
                        cpu.ecx = normal;
                        cpu.edx = normal;
                        cpu.esp = f - 4;
                        native::sub_567370(app, cpu);
                    }
                    const reg32 at = e + 0x34 + 4 * k;
                    double dot = ld(normal + 8) * double(dz);
                    dot += ld(normal + 4) * double(dy);
                    dot += ld(normal) * double(dx);
                    dot -= double(real(k == 0 ? distance : rc::ld32(at)));
                    const reg16 status = compare(cpu, dot, ld(Zero));
                    eax = (at & 0xffff0000u) | status;
                    if (mode == 0 ? dot < ld(Zero) : !(dot < ld(Zero) || std::isnan(dot)))
                        break;
                    eax = count;
                }
                if (mode == 0 ? k == count : k < count)
                    visible = 0;
            }
        }
        eax = (eax & 0xffffff00u) | visible;
        ecx = saved;
        st8(group + 0x18, visible);
        const reg32 counter = visible ? group + 0x14 : group + 0x10;
        st32(counter, rc::ld32(counter) + 1);
    }
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.edx = culler + 14 * 0x20;
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = s + 12;
}

// ---- car shadow ----

const reg32 HullTable = 0x7305b0;       // pointers to the points being sorted
const reg32 HullPoints = 0x73c680;      // the hull, 0x10 bytes per point
const reg32 ShadowVector = 0x73c670;    // light direction (sh_Setup)
const reg32 ShadowScale = 0x7306b4;     // 1 / its z
const reg32 PlanePoint = 0x73cab0;
const reg32 PlaneNormal = 0x73c660;
const reg32 PlaneLimit = 0x6b5e04;
const reg32 GroundTolerance = 0x6b5d68;
const reg32 Corners = 0x73cac0;         // three points per hull point
const reg32 CornerScale = 0x6b5e08;
const reg32 CornerBlend = 0x6b5e0c;
const reg32 Sun = 0x735518;
const reg32 NoSunX = 0x6ccbe8;
const reg32 SunLift = 0x6b5d6c;

// Guest 0x56cd90: compares the points that two table entries point at by x, then by y descending: 1, -1 or 0.
sreg32 comparePoints(x86::CPU& cpu, reg32 a, reg32 b)
{
    const reg32 p = rc::ld32(a), q = rc::ld32(b);
    const double zero = ld(Zero);
    const double d = ld(p) - ld(q);
    cpu.fpu.compare(d, zero);
    if (d > zero)
        return 1;
    cpu.fpu.compare(d, zero);
    if (d < zero)
        return -1;
    const double e = ld(q + 4) - ld(p + 4);
    cpu.fpu.compare(e, zero);
    if (e > zero)
        return 1;
    cpu.fpu.compare(e, zero);
    return e < zero ? -1 : 0;
}

// Guest 0x672ad0: the selection sort qsort uses for short runs (lo, hi inclusive).
void shortSort(x86::CPU& cpu, reg32 lo, reg32 hi, reg32 width, bool reverse)
{
    const auto less = [&](reg32 a, reg32 b) { return reverse ? comparePoints(cpu, b, a) : comparePoints(cpu, a, b); };
    while (hi > lo)
    {
        reg32 max = lo;
        for (reg32 p = lo + width; p <= hi; p += width)
            if (less(p, max) > 0)
                max = p;
        if (max != hi)
        {
            for (reg32 i = 0; i < width; ++i)
            {
                const reg8 x = rc::ld8(max + i), y = rc::ld8(hi + i);
                st8(max + i, y);
                st8(hi + i, x);
            }
        }
        hi -= width;
    }
}

void swapBytes(reg32 a, reg32 b, reg32 width)
{
    if (a == b)
        return;
    for (reg32 i = 0; i < width; ++i)
    {
        const reg8 x = rc::ld8(a + i), y = rc::ld8(b + i);
        st8(b + i, x);
        st8(a + i, y);
    }
}

// Guest 0x672b40 (the CRT qsort) with the comparator 0x56cd90, or 0x56cdf0 (its arguments swapped) when reverse.
void sortPoints(x86::CPU& cpu, reg32 base, reg32 count, reg32 width, bool reverse)
{
    const auto cmp = [&](reg32 a, reg32 b) { return reverse ? comparePoints(cpu, b, a) : comparePoints(cpu, a, b); };
    if (count < 2 || width == 0)
        return;
    reg32 lostk[30], histk[30];
    sreg32 stkptr = 0;
    reg32 lo = base, hi = base + width * (count - 1);
    for (;;)
    {
        const reg32 size = (hi - lo) / width + 1;
        if (size <= 8)
        {
            shortSort(cpu, lo, hi, width, reverse);
        }
        else
        {
            reg32 mid = lo + (size >> 1) * width;
            if (cmp(lo, mid) > 0)
                swapBytes(lo, mid, width);
            if (cmp(lo, hi) > 0)
                swapBytes(lo, hi, width);
            if (cmp(mid, hi) > 0)
                swapBytes(mid, hi, width);
            reg32 loguy = lo, higuy = hi;
            for (;;)
            {
                if (mid > loguy)
                {
                    do
                        loguy += width;
                    while (loguy < mid && cmp(loguy, mid) <= 0);
                }
                if (mid <= loguy)
                {
                    do
                        loguy += width;
                    while (loguy <= hi && cmp(loguy, mid) <= 0);
                }
                do
                    higuy -= width;
                while (higuy > mid && cmp(higuy, mid) > 0);
                if (higuy < loguy)
                    break;
                swapBytes(loguy, higuy, width);
                if (mid == higuy)
                    mid = loguy;
            }
            higuy += width;
            if (mid < higuy)
            {
                do
                    higuy -= width;
                while (higuy > mid && cmp(higuy, mid) == 0);
            }
            if (mid >= higuy)
            {
                do
                    higuy -= width;
                while (higuy > lo && cmp(higuy, mid) == 0);
            }
            if (sreg32(higuy - lo) >= sreg32(hi - loguy))
            {
                if (lo < higuy)
                {
                    lostk[stkptr] = lo;
                    histk[stkptr] = higuy;
                    ++stkptr;
                }
                if (loguy < hi)
                {
                    lo = loguy;
                    continue;
                }
            }
            else
            {
                if (loguy < hi)
                {
                    lostk[stkptr] = loguy;
                    histk[stkptr] = hi;
                    ++stkptr;
                }
                if (lo < higuy)
                {
                    hi = higuy;
                    continue;
                }
            }
        }
        --stkptr;
        if (stkptr < 0)
            return;
        lo = lostk[stkptr];
        hi = histk[stkptr];
    }
}

// Guest 0x56ce10: sorts count point pointers at table, then keeps one side of the hull in order; the number kept.
reg32 hullChain(x86::CPU& cpu, reg32 table, sreg32 count, bool reverse)
{
    sortPoints(cpu, table, reg32(count), 4, reverse);
    sreg32 kept = 1;
    if (count <= 2)
        return reg32(kept);
    const double zero = ld(Zero);
    for (sreg32 b = 2; b < count; ++b)
    {
        sreg32 c = kept;
        if (kept >= 1)
        {
            const reg32 p = rc::ld32(table + 4 * reg32(b));
            for (;;)
            {
                const reg32 cur = rc::ld32(table + 4 * reg32(c));
                const reg32 prev = rc::ld32(table + 4 * reg32(c) - 4);
                const double x = (ld(prev + 4) - ld(cur + 4)) * (ld(p) - ld(cur));
                const double y = (ld(p + 4) - ld(cur + 4)) * (ld(prev) - ld(cur));
                const double r = x - y;
                cpu.fpu.compare(r, zero);
                if (!(r < zero || r == zero))
                    break;
                --c;
                if (c < 1)
                    break;
            }
        }
        const reg32 moved = rc::ld32(table + 4 * reg32(c + 1));
        kept = c + 1;
        st32(table + 4 * reg32(kept), rc::ld32(table + 4 * reg32(b)));
        st32(table + 4 * reg32(b), moved);
    }
    return reg32(kept);
}

// Guest 0x56ceb0: the hull point at p lies within the limit of the shadow plane.
bool nearPlane(x86::CPU& cpu, reg32 p)
{
    const double a = ld(p) - ld(PlanePoint);
    const double b = ld(p + 4) - ld(PlanePoint + 4);
    const double c = ld(p + 8) - ld(PlanePoint + 8);
    double d = a * ld(PlaneNormal);
    d += ld(PlaneNormal + 8) * c;
    d += ld(PlaneNormal + 4) * b;
    cpu.fpu.compare(d, ld(Zero));
    if (d < ld(Zero))
        d = -d;
    cpu.fpu.compare(d, ld(PlaneLimit));
    return d < ld(PlaneLimit);
}

// Guest 0x56cf10: the convex hull of [esp+8] points at [esp+4] (count updated), placed on the ground through the
// topology coordinate [esp+0xc]: height [esp+0x10] as the guess, plus [esp+0x14]; with [esp+0x1c] one ground query
// for all points. The points kept go to HullPoints. cdecl.
void shadowHull(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 s = cpu.esp;
    const reg32 ebp = s - 4;
    const reg32 points = rc::ld32(s + 4);
    const reg32 countAt = rc::ld32(s + 8);
    const reg32 coordinate = rc::ld32(s + 0xc);
    const reg32 found = ebp + 0xf;
    const sreg32 n = sreg32(rc::ld32(countAt));
    for (sreg32 i = 0; i < n; ++i)
        st32(HullTable + 4 * reg32(i), points + 0x10 * reg32(i));
    const reg32 lower = hullChain(cpu, HullTable, n, false);
    st32(HullTable + 4 * reg32(n), rc::ld32(HullTable));
    st32(s + 8, lower);
    const reg32 upper = hullChain(cpu, HullTable + 4 * lower, n - sreg32(lower) + 1, true);
    const reg32 total = upper + lower;
    st32(countAt, total);
    const double tolerance = ld(GroundTolerance);
    reg32 rejected = 0;
    reg32 out = HullPoints;
    if (rc::ld32(s + 0x18) != 0)
    {
        const reg32 first = rc::ld32(HullTable);
        st32(HullPoints + 4, refloat(rc::ld32(first + 4)));
        st32(HullPoints, rc::ld32(first));
        st32(HullPoints + 8, rc::ld32(s + 0x10));
        const reg32 sp = ebp - 0x24;
        rc::st32(sp, coordinate);
        rc::st32(sp + 4, HullPoints);
        rc::st32(sp + 8, 0);
        rc::st32(sp + 0xc, 0);
        rc::st32(sp + 0x10, found);
        call(0x440a40, &native::sub_440a40, app, cpu, sp - 4, 0x56cfb7);
        const double ground = double(cpu.fpu.pop()) + ld(s + 0x14);
        const reg32 height = bits(float(ground));
        rc::st32(ebp - 4, height);
        st32(s + 0x18, 0);
        for (sreg32 i = 0; i < sreg32(total); ++i)
        {
            const reg32 p = rc::ld32(HullTable + 4 * reg32(i));
            st32(out + 4, refloat(rc::ld32(p + 4)));
            st32(out, rc::ld32(p));
            st32(out + 8, height);
            bool keep = false;
            if (rc::ld8(found) != 0)
            {
                const double d = std::fabs(double(real(height)) - ld(s + 0x10));
                cpu.fpu.compare(d, tolerance);
                if (d < tolerance || d == tolerance)
                    keep = nearPlane(cpu, out);
            }
            if (keep)
                out += 0x10;
            else
                st32(s + 0x18, ++rejected);
        }
        cpu.eax = rejected;
        cpu.ecx = total - rejected;
        st32(countAt, total - rejected);
    }
    else
    {
        st32(s + 0x18, 0);
        cpu.ecx = 0;
        // 0x440a40 pops its five arguments (ret 0x14).
        const reg32 sp = ebp - 0x24;
        for (sreg32 i = 0; i < sreg32(total); ++i)
        {
            const reg32 p = rc::ld32(HullTable + 4 * reg32(i));
            st32(out, rc::ld32(p));
            st32(out + 4, refloat(rc::ld32(p + 4)));
            st32(out + 8, rc::ld32(s + 0x10));
            rc::st32(sp, coordinate);
            rc::st32(sp + 4, out);
            rc::st32(sp + 8, 0);
            rc::st32(sp + 0xc, 0);
            rc::st32(sp + 0x10, found);
            call(0x440a40, &native::sub_440a40, app, cpu, sp - 4, 0x56d069);
            const double v = double(cpu.fpu.pop()) + ld(s + 0x14);
            st32(out + 8, bits(float(v)));
            bool keep = false;
            if (rc::ld8(found) != 0)
            {
                const double d = std::fabs(v - ld(s + 0x10));
                cpu.fpu.compare(d, tolerance);
                if (d < tolerance || d == tolerance)
                    keep = nearPlane(cpu, out);
            }
            if (keep)
                out += 0x10;
            else
                st32(s + 0x18, ++rejected);
        }
        cpu.eax = total - rejected;
        st32(countAt, total - rejected);
    }
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = s + 4;
}

// Guest 0x56d0c0: three smoothed points per hull point ([esp+4] points at HullPoints) to Corners; eax = their
// number. cdecl.
void shadowCorners(win32::WinApplication* app, x86::CPU& cpu)
{
    (void)app;
    const x86::CPU entry = cpu;
    const sreg32 n = sreg32(rc::ld32(cpu.esp + 4));
    const double k1 = ld(CornerScale), k2 = ld(CornerBlend);
    // The difference to the point before, scaled, alternating between two buffers.
    float buffer[2][3];
    {
        const reg32 last = ShadowVector + 0x10 * reg32(n);
        const float dx = float(ld(HullPoints) - ld(last));
        const double dy = ld(HullPoints + 4) - ld(last + 4);
        const float dz = float(ld(HullPoints + 8) - ld(last + 8));
        buffer[1][0] = float(double(dx) * k1);
        buffer[1][1] = float(dy * k1);
        buffer[1][2] = float(double(dz) * k1);
    }
    for (sreg32 c = 0; c < n; ++c)
    {
        const reg32 cur = HullPoints + 0x10 * reg32(c);
        const reg32 next = HullPoints + 0x10 * reg32(c + 1 == n ? 0 : c + 1);
        float* a = buffer[c & 1];
        const float* b = buffer[(c & 1) ^ 1];
        a[0] = float(ld(next) - ld(cur));
        a[1] = float(ld(next + 4) - ld(cur + 4));
        a[2] = float(ld(next + 8) - ld(cur + 8));
        for (int i = 0; i < 3; ++i)
            a[i] = float(k1 * double(a[i]));
        const reg32 out = Corners + 0x30 * reg32(c);
        for (int i = 0; i < 3; ++i)
            stf32(out + 4 * i, float(ld(cur + 4 * i) - double(b[i])));
        for (int i = 0; i < 3; ++i)
            stf32(out + 0x20 + 4 * i, float(ld(cur + 4 * i) + double(a[i])));
        const double ex = double(a[0]) - double(b[0]);
        const double ey = double(a[1]) - double(b[1]);
        const float ez = float(double(a[2]) - double(b[2]));
        stf32(out + 0x10, float(ex * k2 + ld(cur)));
        stf32(out + 0x14, float(ey * k2 + ld(cur + 4)));
        stf32(out + 0x18, float(double(ez) * k2 + ld(cur + 8)));
    }
    cpu.eax = n > 0 ? reg32(3 * n) : 0;
    cpu.ecx = reg32(n);
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = entry.esp + 4;
}

// Guest 0x56d700 (sh_Setup): the direction from the light to the car position at eax, tilted down to at least the
// minimum angle, to ShadowVector, and 1 / its z to ShadowScale.
void shadowSetup(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 s = cpu.esp;
    const reg32 v = cpu.eax;
    const reg32 f = ((s - 4) & ~15u) - 0x30;
    const reg32 sun = rc::ld32(Sun);
    double ax, ay;
    reg32 az;
    if (sun == 0)
    {
        ax = ld(NoSunX);
        ay = ld(Zero);
        az = 0x45dac000;
    }
    else
    {
        ax = ld(sun + 0x2c);
        ay = ld(sun + 0x30);
        az = bits(float(ld(SunLift) + ld(sun + 0x34)));
    }
    const reg32 x = bits(float(ld(v) - ax));
    const double y = ld(v + 4) - ay;
    const double z = ld(v + 8) - double(real(az));
    st32(ShadowVector, x);
    stf32(ShadowVector + 4, float(y));
    stf32(ShadowVector + 8, float(z));
    double planar = ld(ShadowVector + 4) * ld(ShadowVector + 4);
    planar += ld(ShadowVector) * ld(ShadowVector);
    const reg32 sp = f - 8;
    rc::st32(sp + 4, bits(float(-ld(ShadowVector + 8))));
    rc::st32(sp, bits(float(std::sqrt(planar))));
    cpu.ecx = sun;
    rc::st32(sp - 4, 0x56d79e);
    cpu.esp = sp - 4;
    native::sub_564db0(app, cpu);
    if (reg16(cpu.eax) < 0x1a90)
    {
        cpu.eax = 0x1a90;
        cpu.edx = f + 8;
        cpu.ecx = f + 0xc;
        rc::st32(f - 4, 0x56d7bd);
        cpu.esp = f - 4;
        native::sub_564bd0(app, cpu);
        const double sine = ld(f + 0xc), cosine = ld(f + 8);
        double length = ld(ShadowVector + 8) * ld(ShadowVector + 8);
        length += ld(ShadowVector + 4) * ld(ShadowVector + 4);
        length += ld(ShadowVector) * ld(ShadowVector);
        length = std::sqrt(length);
        stf32(ShadowVector + 8, float(-(sine * length)));
        const double inverse = ld(One) / (std::fabs(ld(ShadowVector + 4)) + std::fabs(ld(ShadowVector)));
        stf32(ShadowVector, float(ld(ShadowVector) * inverse * length * cosine));
        stf32(ShadowVector + 4, float(inverse * ld(ShadowVector + 4) * length * cosine));
    }
    const double vz = ld(ShadowVector + 8);
    cpu.fpu.compare(vz, ld(Zero));
    if (vz == ld(Zero))
        st32(ShadowScale, 0);
    else
        stf32(ShadowScale, float(ld(One) / vz));
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = s + 4;
}

// ---- RenderCars 0x571050 and its helpers ----

const reg32 CarList = 0x7361f8;         // the list head of the cars
const reg32 RenderedFlag = 0x73462c;
const reg32 GameFlow = 0x7361f0;
const reg32 FrameDelta = 0x73457c;
const reg32 BlinkStill = 0x6cc8bc;
const reg32 BlinkPhase = 0x7364dc;
const reg32 WorldTime = 0x73ad34;
const reg32 TimeScale = 0x6cca38;
const reg32 Views = 0x740460;           // the eViews, 0x60 bytes each
const reg32 DynamicLightCount = 0x6f1d40;
const reg32 MinusOne = 0x6cc8a8;
const reg32 Half = 0x6cc7dc;
const reg32 DamageScale = 0x73460c;
const reg32 DamageScale2 = 0x734608;
const reg32 DragCameras = 0x7361bc;     // two camera records: view index at +0x18, mode at +0x60

// Guest 0x4280a0 (ecx = the car): the car blinks after a reset; 1 or 0.
reg32 isResetBlinking(x86::CPU& cpu, reg32 car)
{
    const reg32 reset = rc::ld32(car + 0x884);
    if (reset == 0 || reset == 0x7fffffffu)
        return 0;
    const double t = double(sreg32(rc::ld32(WorldTime) - reset)) * ld(TimeScale);
    cpu.fpu.compare(t, ld(car + 0x888));
    return t < ld(car + 0x888) ? 1 : 0;
}

// Guest 0x570b90 (edi = the car, [esp+4] = the view): the car is not drawn this frame (blinking or poofed). Sets eax,
// ecx and edx as the guest leaves them.
void isBlinkingOrPoofed(x86::CPU& cpu, reg32 car, reg32 view)
{
    reg32 ecx = rc::ld32(GameFlow);
    reg32 eax = rc::ld32(ecx + 0x3c);
    if (eax != 0)
    {
        reg32 edx = rc::ld32(eax + 0x20);
        cpu.edx = edx;
        if (edx != 0)
        {
            edx = rc::ld32(eax + 0x1c);
            cpu.edx = edx;
            if (edx != 0)
            {
                cpu.eax = 0;
                cpu.ecx = ecx;
                return;
            }
        }
    }
    eax = rc::ld32(ecx + 0x40);
    if (eax != 0)
    {
        ecx = rc::ld32(eax + 0x1c);
        if (ecx != 0)
        {
            cpu.eax = 0;
            cpu.ecx = ecx;
            return;
        }
    }
    ecx = rc::ld32(view + 4);
    const reg32 edx = 1u << (ecx & 31);
    reg32 result = rc::ld8(car + 0x8a8);
    if (!(edx & rc::ld32(car + 0x530)))
    {
        const reg32 blinking = isResetBlinking(cpu, car);
        ecx = car;
        if (blinking != 0 || (rc::ld32(car + 0x538) & edx) != 0)
        {
            const double d = std::fabs(ld(FrameDelta));
            cpu.fpu.compare(d, ld(BlinkStill));
            if (!(d < ld(BlinkStill) || d == ld(BlinkStill)))
                result = rc::ld32(BlinkPhase) & 1;
        }
    }
    cpu.eax = result;
    cpu.ecx = ecx;
    cpu.edx = edx;
}

// Guest 0x571440 (UpdateCarDynamicLights, stdcall [esp+8] = the car): the car's dynamic light records from its
// matrix and position. Sets eax and edx as the guest leaves them.
void updateDynamicLights(x86::CPU& cpu, reg32 car)
{
    const reg32 count = rc::ld32(DynamicLightCount);
    if (count == 0)
        return;
    const reg32 matrix = car + 0x90, position = car + 0x60;
    reg32 edx = 0;
    for (reg32 i = 0; i < count;)
    {
        const reg32 light = i * 0x68 + rc::ld32(car + 0x4d4) + 0x19e0;
        st32(light + 0x14, rc::ld32(0x6f1d34 + 4 * i));
        st32(light + 0x24, rc::ld32(0x6f1d0c + 4 * i));
        const double sign = i == 0 ? ld(One) : ld(MinusOne);
        stf32(light + 0x28, float(sign * ld(matrix)));
        stf32(light + 0x2c, float(sign * ld(matrix + 4)));
        stf32(light + 0x30, float(sign * ld(matrix + 8)));
        ++i;
        const double reach = ld(0x6f1d28 + 4 * i);
        stf32(light + 0x18, float(ld(light + 0x28) * reach + ld(position)));
        stf32(light + 0x1c, float(reach * ld(light + 0x2c) + ld(position + 4)));
        stf32(light + 0x20, float(ld(light + 0x30) * reach + ld(position + 8)));
        st32(light + 0x34, rc::ld32(0x6f1d10 + 4 * i));
        st32(light + 0x38, rc::ld32(0x73464c + 4 * i));
        edx = rc::ld16(0x6f1d3a + 2 * i);
        stf32(light + 0x3c, float(double(sreg32(edx))));
        edx = rc::ld32(0x6f1d20 + 4 * i);
        st32(light + 0x40, edx);
        logStore(light + 0x44, 2);
        rc::st16(light + 0x44, 0);
    }
    cpu.eax = position;
    cpu.edx = edx;
}

// Guest 0x401cd0: copies the matrix at ecx to eax, the middle two floats of each row through the FPU. edx = the last
// row's w.
void copyMatrix(x86::CPU& cpu, reg32 from, reg32 to)
{
    for (reg32 row = 0; row < 0x40; row += 0x10)
    {
        st32(to + row, rc::ld32(from + row));
        st32(to + row + 4, refloat(rc::ld32(from + row + 4)));
        st32(to + row + 8, refloat(rc::ld32(from + row + 8)));
        st32(to + row + 0xc, rc::ld32(from + row + 0xc));
    }
    cpu.edx = rc::ld32(from + 0x3c);
}

// Guest 0x42a120 (ecx = the view index, edx and eax = two arrays of 4 matrices, [esp+4] = the car): the wheel
// matrices for the render. Sets eax, ecx and edx as the guest leaves them.
void wheelMatrices(x86::CPU& cpu, reg32 index, reg32 first, reg32 second, reg32 car)
{
    if (!((1u << (index & 31)) & rc::ld32(car + 0x530)))
    {
        for (reg32 k = 0; k < 4; ++k)
        {
            copyMatrix(cpu, car + 0xf0 + 0x40 * k, first + 0x40 * k);
            copyMatrix(cpu, car + 0x1f0 + 0x40 * k, second + 0x40 * k);
        }
        cpu.eax = second + 0xc0;
        cpu.ecx = car + 0x2b0;
        return;
    }
    const reg32 one = 0x3f800000;
    for (reg32 k = 0; k < 4; ++k)
    {
        const reg32 position = rc::ld32(car + 0x20) + 0x10 + 0x30 * k;
        for (const reg32 m : { first + 0x40 * k, second + 0x40 * k })
        {
            for (reg32 i = 0; i < 16; ++i)
                st32(m + 4 * i, i % 5 == 0 ? one : 0);
            st32(m + 0x30, rc::ld32(position));
            st32(m + 0x34, refloat(rc::ld32(position + 4)));
            st32(m + 0x38, refloat(rc::ld32(position + 8)));
        }
    }
    cpu.eax = second;
    cpu.ecx = rc::ld32(rc::ld32(car + 0x20) + 0xa0);
    cpu.edx = first;
}

// Guest 0x429ba0 (DamageAbove, ecx = the car, edx = the camera position, eax = its direction): the car's
// [+0x8e4]. eax gets the status word fnstsw stores.
void damageAbove(x86::CPU& cpu, reg32 car, reg32 point, reg32 direction)
{
    const float tz = float(ld(point + 8));
    const float tx = float(ld(point) - ld(car + 0x60));
    const double dy = ld(point + 4) - ld(car + 0x64);
    const double dz = double(tz) - ld(car + 0x68);
    double sum = dz * dz;
    sum += double(tx) * double(tx);
    sum += dy * dy;
    const double inverse = ld(One) / std::sqrt(sum);
    const float nx = float(double(tx) * inverse);
    const float ny = float(inverse * dy);
    const double nz = dz * inverse;
    double v = nz * ld(direction + 8);
    v += double(nx) * ld(direction);
    v += double(ny) * ld(direction + 4);
    v = (v + ld(One)) * ld(Half);
    const double side = double(ny) * ld(direction) - double(nx) * ld(direction + 4);
    const reg16 status = compare(cpu, side, ld(Zero));
    if (!(side < ld(Zero) || std::isnan(side)))
        v *= ld(MinusOne);
    st32(car + 0x8e4, bits(float(v)));
    cpu.eax = (cpu.eax & 0xffff0000u) | status;
}

// The virtual call [[ecx]+0x24] of a camera mover (OutsidePOV): al; the guest code for other targets.
void outsidePov(win32::WinApplication* app, x86::CPU& cpu, reg32 mover, reg32 sp)
{
    const reg32 target = rc::ld32(rc::ld32(mover) + 0x24);
    cpu.ecx = mover;
    cpu.eax = rc::ld32(mover);
    if (target == 0x577fd0)
    {
        cpu.eax = (cpu.eax & 0xffffff00u) | 1;
        return;
    }
    if (target == 0x489600)
    {
        // DriveCameraMover::OutsidePOV.
        const reg32 car = rc::ld32(mover + 0x20);
        reg8 dl = 0;
        if (car != 0)
            dl = rc::ld8(car + 0x29) == 0 ? 1 : 0;
        cpu.edx = (cpu.edx & 0xffffff00u) | dl;
        const reg32 mode = rc::ld32(mover + 0x30);
        reg32 result = 1;
        if (mode != 1 && mode != 0 && mode != 4)
        {
            result = 0;
            if (dl != 0)
            {
                const double v = ld(mover + 0x38);
                cpu.fpu.compare(v, ld(Zero));
                if (v > ld(Zero))
                    result = 1;
            }
        }
        cpu.eax = result;
        return;
    }
    rc::st32(sp - 4, 0x571223);
    cpu.esp = sp - 4;
    app->dynamic_call(target, cpu);
}

// Guest 0x571050 (RenderCars, [esp+4] = the view): every car of the list that the view shows, through
// CarRenderInfo::Render 0x56dc30.
void renderCars(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 s = cpu.esp;
    const reg32 view = rc::ld32(s + 4);
    const reg32 b = (((s - 4) & ~15u) - 0x214) - 0xc;     // esp of the body
    const reg32 camera = rc::ld32(view + 0x40);
    rc::st32(b + 0x1c, camera);
    const reg32 list = rc::ld32(CarList);
    st32(RenderedFlag, 0);
    cpu.eax = list;
    if (list != 0)
    {
        reg32 car = rc::ld32(list);
        sreg32 budget = 0x63;
        while (car != list)
        {
            bool drawn = false;
            cpu.ecx = camera;
            if (rc::ld32(camera + 0x280) != car)
            {
                cpu.eax = (cpu.eax & 0xffffff00u) | rc::ld8(car + 0x4d0);
                if (rc::ld8(car + 0x4d0) != 0)
                {
                    rc::st32(b - 4, view);
                    isBlinkingOrPoofed(cpu, car, view);
                    if (cpu.eax == 0)
                    {
                        cpu.ecx = rc::ld32(view + 4);
                        cpu.eax = rc::ld32(car + 0x534);
                        cpu.edx = 1u << (cpu.ecx & 31);
                        if ((cpu.edx & cpu.eax) != 0)
                        {
                            cpu.eax = reg32(budget);
                            drawn = budget > 0;
                        }
                    }
                }
            }
            if (drawn)
            {
                const reg32 info = rc::ld32(car + 0x4d4);
                updateDynamicLights(cpu, car);
                --budget;
                rc::st32(b + 0x18, reg32(budget));
                const reg32 first = b + 0x120, second = b + 0x20;
                wheelMatrices(cpu, rc::ld32(view + 4), first, second, car);
                const reg32 bit = 1u << (rc::ld32(view + 4) & 31);
                const bool moved = (rc::ld32(car + 0x530) & bit) != 0;
                const reg32 matrix = moved ? car + 0x4e0 : car + 0x90;
                const reg32 position = moved ? car + 0x520 : car + 0x50;
                const reg32 sp = b - 0x30;
                const reg32 args[12] = { info, view, position, matrix, first, second, 0, 0, 0,
                                         rc::ld32(DamageScale), rc::ld32(DamageScale2), 0 };
                for (reg32 i = 0; i < 12; ++i)
                    rc::st32(sp + 4 * i, args[i]);
                cpu.eax = position;
                cpu.ecx = matrix;
                cpu.edx = first;
                cpu.ebx = view;
                cpu.esi = info;
                cpu.edi = car;
                call(0x56dc30, &native::sub_56dc30, app, cpu, sp - 4, 0x57119f);
                if (cpu.terminate)
                    return;
                bool damage = true;
                reg32 record = rc::ld32(DragCameras);
                cpu.eax = record;
                if (record != 0)
                {
                    const reg32 index = rc::ld32(view + 4);
                    cpu.ecx = index;
                    bool match = index == rc::ld32(record + 0x18);
                    if (!match)
                    {
                        record = rc::ld32(DragCameras + 4);
                        cpu.eax = record;
                        match = record != 0 && index == rc::ld32(record + 0x18);
                    }
                    if (match)
                    {
                        const reg32 mode = rc::ld32(record + 0x60);
                        cpu.eax = mode;
                        damage = !(mode == 6 || mode == 7);
                    }
                }
                if (damage)
                {
                    rc::st32(b - 4, view);
                    cpu.eax = camera + 0x50;
                    cpu.ecx = car;
                    cpu.edx = camera + 0x40;
                    damageAbove(cpu, car, camera + 0x40, camera + 0x50);
                }
            }
            else
            {
                const reg32 owner = rc::ld32(car + 0x18);
                cpu.eax = owner;
                if (owner != 0)
                {
                    const reg32 head = rc::ld32(owner + 0x18) * 0x60 + Views + 0x44;
                    const reg32 node = rc::ld32(head);
                    cpu.eax = head;
                    cpu.ecx = node;
                    if (node != head && node != 0)
                        cpu.edx = node - 4;
                    if (node != head && node != 0 && node - 4 != 0)
                    {
                        cpu.edx = node;
                        outsidePov(app, cpu, node - 4, b);
                        if (cpu.terminate)
                            return;
                        if ((cpu.eax & 0xff) == 0)
                        {
                            cpu.ecx = rc::ld32(car + 0x4d4);
                            updateDynamicLights(cpu, car);
                        }
                    }
                }
            }
            car = rc::ld32(car);
            if (car == rc::ld32(CarList))
                break;
        }
    }
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = s + 4;
}

// ---- RenderCarReflections 0x570db0 ----

const reg32 ReflectionsOn = 0x6f1ad0;
const reg32 GameState = 0x77a920;
const reg32 PlayerNumbers = 0x78a410;   // the player numbers of the views 4 and 1

// Guest 0x41ff50 (GetCarByNumber, eax = the world, edi = the number): the car, or 0.
reg32 carByNumber(reg32 world, reg32 number)
{
    const sreg32 count = sreg32(rc::ld32(world + 0x18));
    for (sreg32 i = 0; i < count; ++i)
    {
        const reg32 car = rc::ld32(world + 0x28 + 4 * reg32(i));
        if (reg32(sreg32(sreg16(rc::ld16(car + 0xa)))) == number)
            return car;
    }
    return 0;
}

// Guest 0x421c40 (UsingNisCameras, ecx = the race coordinator).
bool usingNisCameras(reg32 race)
{
    const reg32 a = rc::ld32(race + 0x3c);
    if (a != 0 && rc::ld32(a + 0x20) != 0 && rc::ld32(a + 0x1c) != 0)
        return true;
    const reg32 b = rc::ld32(race + 0x40);
    return b != 0 && rc::ld32(b + 0x1c) != 0;
}

// Guest 0x570db0 (RenderCarReflections, esi = the view): the reflected cars through CarRenderInfo::Render, the
// player's own car only unless NIS or drag cameras run.
void renderReflections(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 s = cpu.esp;
    const reg32 view = cpu.esi;
    const reg32 b = (((s - 4) & ~15u) - 0x2a8) - 8;       // esp of the body
    const auto leave = [&]
    {
        cpu.ebx = entry.ebx;
        cpu.edi = entry.edi;
        cpu.ebp = entry.ebp;
        cpu.esi = view;
        cpu.esp = s + 4;
    };
    cpu.eax = rc::ld32(ReflectionsOn);
    if (cpu.eax == 0 || rc::ld32(GameState) != 6)
        return leave();
    cpu.eax = rc::ld32(GameFlow);
    if (rc::ld32(cpu.eax) == 1)
        return leave();
    const reg32 index = rc::ld32(view + 4);
    const reg32 cameras = rc::ld32(DragCameras + 4 * (index != 4 ? 1 : 0));
    const reg32 number = rc::ld32(index == 4 ? PlayerNumbers : PlayerNumbers + 4);
    const reg32 player = carByNumber(rc::ld32(CarList), number);
    const sreg32 own = sreg16(rc::ld16(player + 8));
    const reg32 mover = rc::ld32(cameras + 0x27c);
    bool all = usingNisCameras(rc::ld32(GameFlow)) || rc::ld32(cameras + 0x60) != 0;
    if (!all && mover != 0)
    {
        const reg32 kind = rc::ld32(mover + 0xc);
        all = kind == 2 || kind == 4;
    }
    const reg32 lod = all ? 2 : 3;
    sreg32 begin = all ? 0 : own;
    const sreg32 end = all ? sreg32(rc::ld32(rc::ld32(CarList) + 0x20)) : own + 1;
    const reg32 camera = rc::ld32(view + 0x40);
    cpu.eax = reg32(begin);
    cpu.ecx = reg32(end);
    cpu.edx = camera;
    if (begin >= end)
        return leave();
    for (sreg32 i = begin; i < end; ++i)
    {
        const reg32 car = rc::ld32(rc::ld32(CarList) + 0x28 + 4 * reg32(i));
        bool shown = rc::ld32(camera + 0x280) != car && rc::ld8(car + 0x4d0) != 0;
        if (shown)
        {
            rc::st32(b - 4, view);
            isBlinkingOrPoofed(cpu, car, view);
            shown = cpu.eax == 0 && ((1u << (rc::ld32(view + 4) & 31)) & rc::ld32(car + 0x534)) != 0;
        }
        if (shown)
        {
            const reg32 first = b + 0x1b0, second = b + 0xb0, matrix = b + 0x30;
            wheelMatrices(cpu, rc::ld32(view + 4), first, second, car);
            const reg32 bit = 1u << (rc::ld32(view + 4) & 31);
            const reg32 info = rc::ld32(car + 0x4d4);
            const bool moved = (rc::ld32(car + 0x530) & bit) != 0;
            copyMatrix(cpu, moved ? car + 0x4e0 : car + 0x90, matrix);
            if (sreg32(rc::ld32(car + 0x3cc)) >= 3)
            {
                for (reg32 k = 0; k < 3; ++k)
                    rc::st32(matrix + 0x20 + 4 * k, bits(float(-ld(matrix + 0x20 + 4 * k))));
            }
            else
            {
                const reg32 flip = b + 0x70;
                const reg32 identity[16] = { 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000, 0, 0, 0, 0,
                                             0x3f800000 };
                memcpy(rc::host(flip), identity, 64);
                rc::st32(flip + 0x28, bits(float(-ld(flip + 0x28))));
                float m[16], n[16], r[16];
                memcpy(m, rc::host(matrix), 64);
                memcpy(n, rc::host(flip), 64);
                multiply(m, n, r);
                memcpy(rc::host(matrix), r, 64);
            }
            reg32 near = lod;
            if (lod == 2)
                near = (own != i ? 1 : 0) + 1;
            const reg32 position = moved ? car + 0x520 : car + 0x50;
            const reg32 sp = b - 0x30;
            const reg32 args[12] = { info, view, position, matrix, first, second, 0x401, 0, 1, near, lod, 0 };
            for (reg32 k = 0; k < 12; ++k)
                rc::st32(sp + 4 * k, args[k]);
            cpu.eax = position;
            cpu.ecx = matrix;
            cpu.edx = first;
            cpu.ebx = info;
            cpu.edi = car;
            call(0x56dc30, &native::sub_56dc30, app, cpu, sp - 4, 0x57101c);
            if (cpu.terminate)
                return;
        }
        cpu.ecx = reg32(i + 1);
        cpu.eax = 0x28 + 4 * reg32(i + 1);
        cpu.edx = reg32(end);
    }
    leave();
}

// ---- RenderFlares (cars) 0x570d00 ----

const reg32 FlaresOn = 0x6f41a0;
const reg32 CarFlaresOn = 0x700ea0;
const reg32 PixelScaleMode = 0x7364a0;
const reg32 PixelScale = 0x6b5e00;
const reg32 FlareForce = 0x7345e4;      // four switches that light the flare groups
const reg32 BrakeLevel = 0x6b5d9c;
const reg32 HeadLevel = 0x6b5da0;
const reg32 HeadLevelOff = 0x6b5da4;
const reg32 ReverseScale = 0x6b5dac;
const reg32 ReverseMin = 0x6b5db0;
const reg32 Three = 0x40400000;

// _ftol2 of v as the override computes it: edx:eax.
void ftol(win32::WinApplication* app, x86::CPU& cpu, double v, reg32 slot)
{
    cpu.fpu.push(x86::Float(v));
    cpu.esp = slot;
    native::sub_674898(app, cpu);
    cpu.esp = slot;
}

// Guest 0x545180 (GetPixelSize, ecx = the point, edx = the view, [esp+4] = the radius): the size in pixels of a
// sphere, 0 behind the camera. eax (edx) as _ftol2 leaves them; slot: the return address slot of the call.
void pixelSize(win32::WinApplication* app, x86::CPU& cpu, reg32 point, reg32 view, reg32 radius, reg32 slot)
{
    const reg32 camera = rc::ld32(view + 0x40);
    const double dx = ld(point) - ld(camera + 0x40);
    const double dy = ld(point + 4) - ld(camera + 0x44);
    const double dz = ld(point + 8) - ld(camera + 0x48);
    double depth = dz * ld(camera + 0x58);
    depth += dy * ld(camera + 0x54);
    depth += dx * ld(camera + 0x50);
    const double r = double(real(radius));
    cpu.fpu.compare(-r, depth);
    if (!(-r < depth || -r == depth || __builtin_isunordered(-r, depth)))
    {
        cpu.eax = 0;
        return;
    }
    double length = dz * dz;
    length += dy * dy;
    length += dx * dx;
    const double distance = std::sqrt(length) - r;
    const double half = r * ld(Half);
    cpu.fpu.compare(distance, half);
    double size;
    if (distance < half || distance == half || __builtin_isunordered(distance, half))
        size = ld(view + 0xc) + ld(view + 0xc);
    else
        size = ld(view + 0xc) / distance * r;
    ftol(app, cpu, size, slot - 4);
}

// Guest 0x570c20 (ShouldCarBeRendered, ecx = the car, eax = the camera, esi = the view): eax 1 or 0, ecx and edx as
// the guest leaves them.
reg32 shouldRender(x86::CPU& cpu, reg32 car, reg32 camera, reg32 view)
{
    cpu.ecx = car;
    if (rc::ld32(camera + 0x280) == car || rc::ld8(car + 0x4d0) == 0)
        return cpu.eax = 0;
    isBlinkingOrPoofed(cpu, car, view);
    if (cpu.eax != 0)
        return cpu.eax = 0;
    cpu.ecx = rc::ld32(view + 4);
    cpu.edx = 1u << (cpu.ecx & 31);
    return cpu.eax = (rc::ld32(car + 0x534) & cpu.edx) != 0 ? 1 : 0;
}

// Guest 0x56d8d0 (CarRenderInfo::RenderFlare, eax = the position, [esp+4] = the render info, the view, the matrix,
// the flare switches, the mode): the car's light flares through 0x5447c0. ret 0x14.
void renderFlare(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 s = cpu.esp;
    const reg32 b = (((s - 4) & ~15u) - 0x34) - 0xc;      // esp of the body
    const reg32 position = cpu.eax;
    const reg32 info = rc::ld32(s + 4), view = rc::ld32(s + 8), source = rc::ld32(s + 0xc);
    reg32 switches = rc::ld32(s + 0x10);
    const reg32 mode = rc::ld32(s + 0x14);
    const auto leave = [&]
    {
        cpu.ebx = entry.ebx;
        cpu.esi = entry.esi;
        cpu.edi = entry.edi;
        cpu.ebp = entry.ebp;
        cpu.esp = s + 0x18;
    };
    const reg32 car = rc::ld32(info);
    const reg32 lights = car != 0 ? rc::ld32(car + 0x30) : 0;
    const reg32 matrix = rc::ld32(Arena);
    cpu.ecx = rc::ld32(ArenaLimit);
    cpu.eax = matrix + 0x40;
    if (matrix + 0x40 >= cpu.ecx)
    {
        cpu.eax = rc::ld32(ArenaOverflowBytes) + 0x40;
        st32(ArenaOverflow, 1);
        st32(ArenaOverflowBytes, cpu.eax);
        return leave();
    }
    st32(Arena, matrix + 0x40);
    if (matrix == 0)
        return leave();
    copyMatrix(cpu, source, matrix);
    st32(matrix + 0x30, rc::ld32(position));
    st32(matrix + 0x34, rc::ld32(position + 4));
    st32(matrix + 0x38, rc::ld32(position + 8));
    st32(matrix + 0x3c, 0x3f800000);
    rc::st32(b - 4, Three);
    cpu.ecx = position;
    cpu.edx = view;
    pixelSize(app, cpu, position, view, Three, b - 8);
    sreg32 pixels = sreg32(cpu.eax);
    if (rc::ld32(PixelScaleMode) == 3)
    {
        ftol(app, cpu, double(pixels) * ld(PixelScale), b - 4);
        pixels = sreg32(cpu.eax);
    }
    if (pixels < 4)
        return leave();
    rc::st32(b - 4, view);
    cpu.eax = matrix;
    cpu.ecx = info + 0x20;
    cpu.edx = info + 0x10;
    cpu.esp = b - 8;
    rc::st32(b - 8, 0x56d9a8);
    native::sub_40db20(app, cpu);
    if (cpu.eax == 0)
        return leave();
    // The flare intensities: brake (two), head (three).
    reg32 brake = 0, head;
    if (car != 0 && rc::ld32(rc::ld32(car + 0x14) + 4) == 3)
        head = rc::ld32(HeadLevelOff);
    else
    {
        brake = rc::ld32(BrakeLevel);
        head = rc::ld32(HeadLevel);
    }
    reg32 level[5] = { brake, brake, head, head, head };   // [esp+0x14], 0x18, 0x1c, 0x20, 0x24
    for (reg32 k = 0; k < 4; ++k)
        if (rc::ld32(FlareForce + 4 * k) != 0)
            switches |= 1u << k;
    cpu.edx = head;
    bool braking = (switches & 1) != 0;
    if (!braking && car != 0)
    {
        // Guest 0x5693b0.
        cpu.ecx = rc::ld32(car + 0x14);
        const reg8 al = rc::ld32(cpu.ecx + 4) == 3 ? 1 : rc::ld8(car + 0x855);
        cpu.eax = (car & 0xffffff00u) | al;
        braking = al != 0;
    }
    if (braking)
        level[0] = level[1] = bits(float(double(real(brake)) + ld(One)));
    if (switches & 2)
    {
        level[2] = level[3] = level[4] = bits(float(double(real(head)) + ld(One)));
    }
    else
    {
        cpu.ecx = lights;
        if (lights != 0)
        {
            const double reverse = ld(lights + 0x1e4);
            cpu.fpu.compare(reverse, ld(ReverseMin));
            if (reverse > ld(ReverseMin))
                level[2] = level[3] = level[4] = bits(float(ld(ReverseScale) * reverse + double(real(head))));
        }
    }
    cpu.ecx = info;
    const reg32 owner = rc::ld32(info + 4);
    const reg32 part = owner != 0 ? rc::ld32(owner + 0x71c) : 0;
    const reg32 kind = part != 0 ? reg32(sreg32(sreg8(rc::ld8(part + 0x10)))) : 0x78;
    const reg32 head0 = info + 0x500;
    for (reg32 flare = rc::ld32(head0); flare != head0; flare = rc::ld32(flare))
    {
        const reg32 hash = rc::ld32(flare + 8);
        reg32 intensity = 0;
        bool braker = false, header = false;
        if (hash == 0x9db90133u)
        {
            intensity = level[0];
            braker = true;
        }
        else if (hash == 0xd09091c6u)
        {
            intensity = level[1];
            braker = true;
        }
        else if (hash == 0x31a66786u)
        {
            intensity = level[2];
            header = true;
        }
        else if (hash == 0xa2a2fc7cu)
        {
            intensity = level[3];
            header = true;
        }
        else if (hash == 0xbf700a79u)
        {
            intensity = level[4];
            header = true;
        }
        cpu.edx = braker ? 1 : 0;
        cpu.ecx = header ? 1 : 0;
        cpu.eax = kind;
        if ((kind == 0xe && header) || (kind == 0xd && braker))
            continue;
        const double v = double(real(intensity));
        cpu.fpu.compare(v, ld(Zero));
        if (!(v > ld(Zero)))
            continue;
        cpu.fpu.compare(v, ld(One));
        if (v > ld(One))
            intensity = 0x3f800000;
        rc::st32(b + 0xc, intensity);
        const reg32 sp = b - 0x18;
        reg32 args[6] = { view, flare, intensity, 0, 0, 0 };
        cpu.edx = mode != 0 ? view : intensity;
        cpu.ecx = mode != 0 ? intensity : cpu.ecx;
        if (mode != 0)
        {
            if (car != 0 && sreg32(rc::ld32(car + 0x3cc)) < 3)
            {
                args[3] = 1;
                args[4] = 2;
            }
            else
            {
                const float v3[3] = { rc::ldf32(flare + 0x10), float(ld(flare + 0x14)), float(-ld(flare + 0x18)) };
                float m[16], out[4];
                memcpy(m, rc::host(matrix), 64);
                transform3(v3, m, out);
                memcpy(rc::host(b + 0x30), out, 16);
                args[3] = 2;
                args[4] = 2;
                args[5] = bits(out[2]);
            }
        }
        for (reg32 k = 0; k < 6; ++k)
            rc::st32(sp + 4 * k, args[k]);
        cpu.eax = matrix;
        cpu.ebx = head0;
        cpu.esi = flare;
        cpu.edi = matrix;
        call(0x5447c0, &native::sub_5447c0, app, cpu, sp - 4, 0x56dc0c);
        if (cpu.terminate)
            return;
    }
    leave();
}

// Guest 0x570d00 (RenderFlares, eax = the view, [esp+4] = the mode): the light flares of every car the view shows.
void renderFlares(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 s = cpu.esp;
    const reg32 view = cpu.eax;
    const reg32 mode = rc::ld32(s + 4);
    const auto leave = [&]
    {
        cpu.ebx = entry.ebx;
        cpu.esi = entry.esi;
        cpu.edi = entry.edi;
        cpu.ebp = entry.ebp;
        cpu.esp = s + 4;
    };
    cpu.eax = rc::ld32(FlaresOn);
    if (cpu.eax == 0)
        return leave();
    cpu.eax = rc::ld32(CarFlaresOn);
    if (cpu.eax == 0)
        return leave();
    const reg32 camera = rc::ld32(view + 0x40);
    cpu.eax = camera;
    // The guest keeps the camera in the slot of its pushed ecx and pops it into ecx.
    cpu.ecx = camera;
    reg32 list = rc::ld32(CarList);
    if (list == 0)
        return leave();
    const reg32 body = s - 0x14;
    sreg32 budget = 0x63;
    for (reg32 car = rc::ld32(list); car != list; car = rc::ld32(car))
    {
        if (shouldRender(cpu, car, camera, view) == 0 || budget <= 0)
            continue;
        const reg32 bit = 1u << (rc::ld32(view + 4) & 31);
        const reg32 info = rc::ld32(car + 0x4d4);
        --budget;
        const bool moved = (rc::ld32(car + 0x530) & bit) != 0;
        const reg32 matrix = moved ? car + 0x4e0 : car + 0x90;
        const reg32 position = moved ? car + 0x520 : car + 0x50;
        const reg32 sp = body - 0x14;
        const reg32 args[5] = { info, view, matrix, 0, mode };
        for (reg32 k = 0; k < 5; ++k)
            rc::st32(sp + 4 * k, args[k]);
        rc::st32(sp - 4, 0x570d96);
        cpu.eax = position;
        cpu.ecx = matrix;
        cpu.edx = info;
        cpu.ebx = mode;
        cpu.ebp = reg32(budget);
        cpu.esi = view;
        cpu.edi = car;
        cpu.esp = sp - 4;
        renderFlare(app, cpu);
        if (cpu.terminate)
            return;
        list = rc::ld32(CarList);
    }
    cpu.ecx = camera;
    leave();
}

// ---- 0x570950: the cars of the render list at CarRenderList ----

const reg32 CarRenderList = 0x73d6d8;
const reg32 LodMinimum = 0x6f1b38;      // per kind: the second LOD at +0, the first at +8
const reg32 ForceLights = 0x73464c;
const reg32 ForceLights2 = 0x734648;

// Guest 0x5708f0 (ecx = the LOD kind, ten cdecl arguments): CarRenderInfo::Render with the kind's LODs raised to
// the minimums. a: the arguments; sp: esp at the call of 0x5708f0 (its return address slot).
void renderListed(win32::WinApplication* app, x86::CPU& cpu, reg32 kind, const reg32 a[10], reg32 sp)
{
    reg32 second = rc::ld32(kind * 4 + LodMinimum);
    if (sreg32(second) < sreg32(rc::ld32(DamageScale2)))
        second = rc::ld32(DamageScale2);
    reg32 first = rc::ld32(kind * 4 + LodMinimum + 8);
    if (sreg32(first) < sreg32(rc::ld32(DamageScale)))
        first = rc::ld32(DamageScale);
    const reg32 args[12] = { a[0], a[1], a[2], a[3], a[4], a[5], a[7], a[8], 0, first, second, a[9] };
    const reg32 at = sp - 0x30;
    for (reg32 k = 0; k < 12; ++k)
        rc::st32(at + 4 * k, args[k]);
    cpu.eax = a[1];
    cpu.ecx = a[0];
    cpu.edx = a[2];
    call(0x56dc30, &native::sub_56dc30, app, cpu, at - 4, 0x57094c);
}

// Guest 0x570950 ([esp+4] = the view, then two render switches and two light switches): the cars of the render
// list, each through its model (0x40c910) or CarRenderInfo::Render.
void renderList(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::CPU entry = cpu;
    const reg32 s = cpu.esp;
    const reg32 ebp = s - 4;
    const reg32 b = (((s - 4) & ~15u) - 0x224) - 0xc;     // esp of the body
    const reg32 view = rc::ld32(s + 4);
    const reg32 scaleMode = rc::ld32(PixelScaleMode);
    rc::st32(b + 0x18, scaleMode);
    rc::st32(b + 0x1c, CarRenderList);
    cpu.eax = CarRenderList;
    for (reg32 item = rc::ld32(CarRenderList); item != CarRenderList; item = rc::ld32(item))
    {
        const reg32 info = rc::ld32(item + 8);
        if (info == 0 || rc::ld32(item + 0x994) == 0)
            continue;
        cpu.eax = rc::ld32(item + 0x994);
        cpu.edx = view;
        const reg32 index = rc::ld32(view + 4);
        cpu.eax = index;
        const reg32 reflected = rc::ld32(item + 0x99c);
        if (rc::ld32(item + 0x730) != index)
        {
            cpu.ecx = reflected;
            if (reflected == 0 || index != 4)
                continue;
        }
        cpu.eax = reflected;
        if (reflected != 0 && sreg32(scaleMode) >= 3)
            continue;
        const reg32 on = reflected != 0 ? rc::ld32(ebp + 0x10) : rc::ld32(ebp + 0xc);
        cpu.ecx = on;
        if (on == 0)
            continue;
        reg32 flags = reflected != 0 ? 0x41 : 0;
        if (rc::ld32(ebp + 0x14) == 0)
            flags |= 0x20;
        if (rc::ld32(ebp + 0x18) == 0)
            flags |= 0x40;
        if (rc::ld32(ForceLights) != 0)
            flags |= 0x20;
        if (rc::ld32(ForceLights2) != 0)
            flags |= 0x40;
        const reg32 model = rc::ld32(item + 0x990);
        cpu.eax = model;
        if (model != 0)
        {
            const reg32 sp = b - 0x14;
            const reg32 args[5] = { view, model, item + 0x750, 0, 0 };
            for (reg32 k = 0; k < 5; ++k)
                rc::st32(sp + 4 * k, args[k]);
            cpu.eax = flags;
            cpu.ecx = item + 0x750;
            cpu.edx = view;
            cpu.ebx = info;
            cpu.esi = item;
            cpu.edi = flags;
            call(0x40c910, &native::sub_40c910, app, cpu, sp - 4, 0x570a3e);
            if (cpu.terminate)
                return;
            continue;
        }
        const reg32 vector = b + 0x20, first = b + 0x130, second = b + 0x30;
        rc::st32(vector, rc::ld32(item + 0x740));
        rc::st32(vector + 4, refloat(rc::ld32(item + 0x744)));
        rc::st32(vector + 8, refloat(rc::ld32(item + 0x748)));
        for (reg32 k = 0; k < 4; ++k)
            copyMatrix(cpu, item + 0x790 + 0x40 * k, first + 0x40 * k);
        for (reg32 k = 0; k < 4; ++k)
            copyMatrix(cpu, item + 0x890 + 0x40 * k, second + 0x40 * k);
        const reg32 kind = rc::ld32(item + 0x99c);
        reg32 lod = 0;
        if (kind == 0)
            lod = (rc::ld32(item + 0x9a4) != 0 ? 8u : 0u) | (rc::ld32(item + 0x9a0) != 0 ? 3u : 0u);
        st32(RenderedFlag, kind);
        const reg32 a[10] = { info, view, vector, item + 0x750, first, second, rc::ld32(item + 0x998), flags, lod,
                              rc::ld32(item + 0x9a8) };
        const reg32 sp = b - 0x28;
        for (reg32 k = 0; k < 10; ++k)
            rc::st32(sp + 4 * k, a[k]);
        rc::st32(sp - 4, 0x570b75);
        cpu.ebx = info;
        cpu.esi = item;
        cpu.edi = flags;
        renderListed(app, cpu, kind, a, sp - 4);
        if (cpu.terminate)
            return;
        cpu.esp = sp;
    }
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = s + 4;
}

// ---- dispatch and shadow compare ----

struct PortInfo
{
    reg32 address;
    const char* name;
    Function guest;
    Function native;
};

const PortInfo s_ports[PortCount] = {
    {0x569460, "part cull 0x569460", &sub_569460_guest, &partCull},
    {0x56cf10, "shadow hull 0x56cf10", &sub_56cf10_guest, &shadowHull},
    {0x56d0c0, "shadow corners 0x56d0c0", &sub_56d0c0_guest, &shadowCorners},
    {0x56d700, "shadow setup 0x56d700", &sub_56d700_guest, &shadowSetup},
    {0x571050, "render cars 0x571050", &sub_571050_guest, &renderCars},
    {0x570db0, "car reflections 0x570db0", &sub_570db0_guest, &renderReflections},
    {0x570d00, "car flares 0x570d00", &sub_570d00_guest, &renderFlares},
    {0x570950, "render list cars 0x570950", &sub_570950_guest, &renderList},
};

// Guest memory a port may store to outside its stack frame, from its entry state.
void portSpans(Port port, const x86::CPU& cpu, std::vector<draw::Span>& spans)
{
    spans.clear();
    const reg32 esp = cpu.esp;
    switch (port)
    {
    case PartCull:
        spans.push_back(draw::Span{Arena, 0x18});
        spans.push_back(draw::Span{rc::ld32(esp + 4), 14 * 0x20});
        spans.push_back(draw::Span{esp + 4, 8});
        spans.push_back(draw::Span{PartPlanes - 4, PartPlanesEnd - PartPlanes});
        break;
    case ShadowHull:
    {
        const reg32 countAt = rc::ld32(esp + 8);
        const reg32 n = rc::ld32(countAt);
        const reg32 entries = sreg32(n) > 0 && n < 0x40 ? n + 1 : 1;
        spans.push_back(draw::Span{HullTable, 4 * entries});
        spans.push_back(draw::Span{HullPoints, 0x10 * (entries - 1)});
        spans.push_back(draw::Span{countAt, 4});
        spans.push_back(draw::Span{esp + 4, 0x18});
        break;
    }
    case ShadowCorners:
    {
        const reg32 n = rc::ld32(esp + 4);
        if (sreg32(n) > 0 && n < 0x40)
            spans.push_back(draw::Span{Corners, 0x30 * n});
        break;
    }
    case ShadowSetup:
        spans.push_back(draw::Span{ShadowVector, 12});
        spans.push_back(draw::Span{ShadowScale, 4});
        break;
    case RenderList:
        spans.push_back(draw::Span{RenderedFlag, 4});
        break;
    case CarFlares:
    {
        spans.push_back(draw::Span{Arena, 0x18});
        const reg32 cursor = rc::ld32(Arena), limit = rc::ld32(ArenaLimit);
        if (cursor < limit)
            spans.push_back(draw::Span{cursor, limit - cursor < 0x1000 ? limit - cursor : 0x1000});
        break;
    }
    case RenderCarsPort:
    {
        spans.push_back(draw::Span{RenderedFlag, 4});
        const reg32 list = rc::ld32(CarList);
        const reg32 lights = rc::ld32(DynamicLightCount);
        if (list == 0)
            break;
        reg32 car = rc::ld32(list);
        for (int n = 0; car != list && n < 0x100; ++n, car = rc::ld32(car))
        {
            const reg32 info = rc::ld32(car + 0x4d4);
            if (info != 0 && lights != 0 && lights < 0x10)
                spans.push_back(draw::Span{info + 0x19e0, 0x68 * lights});
            spans.push_back(draw::Span{car + 0x8e4, 4});
        }
        break;
    }
    default:
        break;
    }
}

bool sameStores(const std::vector<draw::Span>& spans, const std::vector<reg8>& guest, const std::vector<reg8>& native,
                char* text, size_t size)
{
    size_t at = 0;
    for (const draw::Span& span : spans)
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
    for (const Store& store : s_stores)
    {
        bool inside = false;
        for (const draw::Span& span : spans)
            inside |= store.address - span.address < span.size && store.address + store.size - span.address <= span.size;
        if (!inside)
        {
            snprintf(text, size, "native stored at 0x%08x outside the guest's stores", unsigned(store.address));
            return false;
        }
    }
    return true;
}

bool sameStack(const x86::CPU& g, const x86::CPU& n, char* text, size_t size)
{
    const uint32_t depth = g.fpu.count < 8 ? g.fpu.count : 8;
    for (uint32_t i = 0; i < depth; ++i)
    {
        const double a = double(const_cast<x86::CPU&>(g).fpu.st(int(i)));
        const double b = double(const_cast<x86::CPU&>(n).fpu.st(int(i)));
        if (memcmp(&a, &b, sizeof(a)) != 0 && !(std::isnan(a) && std::isnan(b)))
        {
            snprintf(text, size, "x87 st%u guest %.17g native %.17g", unsigned(i), a, b);
            return false;
        }
    }
    return true;
}

void shadow(Port port, win32::WinApplication* app, x86::CPU& cpu)
{
    const PortInfo& info = s_ports[port];
    if (s_level + 1 >= ShadowLevels)
    {
        info.native(app, cpu);
        return;
    }
    Shadow& s = s_shadows[++s_level];
    g_tap = true;
    struct Leave
    {
        ~Leave()
        {
            --s_level;
            g_tap = s_level >= 0;
        }
    } leave;
    const x86::CPU entry = cpu;
    portSpans(port, entry, s.spans);
    draw::snapshot(s.spans, s.before);

    s.capture.clear();
    s.taps.clear();
    s.thread = pf::Thread::currentId();
    s.mode = TapRecord;
    ++s_depth;
    bridge::begin(s.capture, bridge::Capture::Record);
    info.guest(app, cpu);
    bridge::end();
    --s_depth;
    s.mode = TapOff;
    if (cpu.terminate)
        return;
    const x86::CPU guest = cpu;
    draw::snapshot(s.spans, s.guestAfter);

    draw::restoreSpans(s.spans, s.before);
    cpu = entry;
    s_stores.clear();
    s_stackTop = entry.esp;
    s_logStores = true;
    s.mode = TapReplay;
    s.next = 0;
    s.differs = false;
    bridge::begin(s.capture, bridge::Capture::Replay);
    info.native(app, cpu);
    bridge::end();
    s.mode = TapOff;
    s_logStores = false;
    const x86::CPU native = cpu;
    draw::snapshot(s.spans, s.nativeAfter);
    draw::restoreSpans(s.spans, s.guestAfter);
    cpu = guest;

    ++s_calls;
    ++s_portCalls[port];
    char text[200] = {};
    bool same = true;
    if (s.differs)
    {
        same = false;
        snprintf(text, sizeof(text), "%s", s.text);
    }
    else if (s.next != s.taps.size())
    {
        same = false;
        snprintf(text, sizeof(text), "calls guest %u native %u", unsigned(s.taps.size()), unsigned(s.next));
    }
    same = same && draw::sameCalls(s.capture, text, sizeof(text))
           && draw::sameState(info.address, 0xff, guest, native, text, sizeof(text))
           && sameStack(guest, native, text, sizeof(text))
           && sameStores(s.spans, s.guestAfter, s.nativeAfter, text, sizeof(text));
    if (same)
        return;
    ++s_differing;
    ++s_portDiffering[port];
    if (s_portLogged[port] < ShadowLogLimit)
    {
        ++s_portLogged[port];
        pf::logInfo("shadow_cars: %s call %u (caller 0x%08x): %s", info.name, unsigned(s_calls),
                    unsigned(rc::ld32(entry.esp)), text);
    }
}

}

bool tapSlow(reg32 address, win32::WinApplication* app, x86::CPU& cpu, Function function)
{
    Shadow* level = current();
    if (!level || level->mode != TapRecord || level->inTap || pf::Thread::currentId() != level->thread)
        return false;
    Shadow& s = *level;
    TapCall t;
    t.address = address;
    t.esp = cpu.esp;
    t.depth = cpu.fpu.count;
    t.position = uint32_t(s.capture.calls.size());
    for (int r = 0; r < 8; ++r)
        t.registers[r] = registerOf(cpu, r);
    const uint32_t n = calleeArguments(address);
    for (uint32_t i = 0; i < n; ++i)
        t.arguments.push_back(rc::ld32(cpu.esp + 4 + 4 * i));
    calleeOutputs(address, cpu, t.outputs);
    calleeInputs(address, cpu, t.inputs);
    for (const Output& in : t.inputs)
        t.inputBytes.insert(t.inputBytes.end(), rc::host(in.address), rc::host(in.address) + in.size);
    s.inTap = true;
    bridge::end();
    const uint32_t depth = s_depth;
    s_depth = 0;
    function(app, cpu);
    s_depth = depth;
    bridge::begin(s.capture, bridge::Capture::Record);
    s.inTap = false;
    t.after = cpu;
    for (const Output& o : t.outputs)
        t.bytes.insert(t.bytes.end(), rc::host(o.address), rc::host(o.address) + o.size);
    s.taps.push_back(std::move(t));
    return true;
}

void run(Port port, win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    const PortInfo& info = s_ports[port];
    if (!s.native || s_depth != 0)
    {
        info.guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0) && !bridge::capturing())
    {
        shadow(port, app, cpu);
        return;
    }
    info.native(app, cpu);
}

void logShadowTotal()
{
    if (settings().native && settings().shadow)
    {
        pf::logInfo("shadow_cars: %u calls compared, %u differ", unsigned(s_calls), unsigned(s_differing));
        for (int p = 0; p < PortCount; ++p)
            pf::logInfo("shadow_cars: %s %u compared, %u differ", s_ports[p].name, unsigned(s_portCalls[p]),
                        unsigned(s_portDiffering[p]));
    }
}

}
}

namespace nfsu { namespace native
{

void sub_570950(win32::WinApplication* app, x86::CPU& cpu)
{
    cars::run(cars::RenderList, app, cpu);
}

}}
