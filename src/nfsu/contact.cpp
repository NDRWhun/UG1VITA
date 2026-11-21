// Native contact test helpers, bit-exact with the translated guest code: every x87 chain in double in the guest's
// order, floats where the guest stores floats, the guest's stores outside its own stack frame and the registers the
// callers read.
//
// 0x468FE0 DynamicCollisionBody::Interpolate (ecx = the body; [esp+4] the position out, [esp+8] the matrix out,
// [esp+0xC] the time): the body's current position and matrix (its virtual getters +8 and +0x14) moved back towards
// the ones it started the step with (+0xC0, +0x80) by (step time +0x70 - time) / step time; with a step time below
// [0x6CCAE0] the current position and matrix are copied (0x401CD0). The deltas of the matrix, the z delta of the
// position and the scaled deltas of the last two columns (and of the last row from its second column) are stored as
// floats, as the guest does. Getters that return [body+0x6C] plus a constant run natively, others as guest code.
//
// 0x472830 (the matrices of a witness, esi; edi = position A, [esp+4] matrix A, [esp+8] matrix B, [esp+0xC] position
// B): when a position moved by more than [0x6CC8BC] on an axis from the one kept at +0x40 / +0x50, or the witness is
// already marked (+0x240), the world matrices are copied (0x401CD0) to +0x140 / +0x1C0 with the positions as their
// last rows, their transposes (0x565230) to +0x180 / +0x200, and the positions kept.
//
// contact_native=off runs the guest bodies; shadow_contact=on runs both and compares the outputs, the registers the
// callers read and the x87 depth (total at the end of a benchmark).

#include "contact.h"

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
void sub_468fe0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_472830_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace contact
{

namespace
{

using x86::reg32;

const reg32 InterpolateLimit = 0x6ccae0;
const reg32 MoveEpsilon = 0x6cc8bc;

inline double ld(reg32 address) { return double(rc::ldf32(address)); }
inline float pass(float value) { return float(double(value)); }

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = {pf::Config::getBool("contact_native", true), pf::Config::getBool("shadow_contact", false)};
        pf::logInfo("contact: interpolation and witness matrices %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// Virtual getters that return [body + 0x6C] + offset when [body + 0x60] is set.
struct Getter
{
    reg32 function, offset;
};

const Getter Getters[] = {
    {0x46a1c0, 0x20},   // CarCollisionBody position
    {0x46a220, 0x30},   // CarCollisionBody matrix
    {0x592880, 0xa0},   // WorldObjectCollisionBody position
    {0x5928e0, 0x30},   // WorldObjectCollisionBody matrix
};

// The body's getter at vtable offset slot, called with esp = frame when it runs as guest code; eax.
reg32 getter(win32::WinApplication* app, x86::CPU& cpu, reg32 body, reg32 slot, reg32 frame)
{
    const reg32 function = rc::ld32(rc::ld32(body) + slot);
    for (const Getter& g : Getters)
    {
        if (g.function != function)
            continue;
        if (rc::ld32(body + 0x60) != 0)
            return rc::ld32(body + 0x6c) + g.offset;
        break;
    }
    cpu.ecx = body;
    cpu.esp = frame;
    ::native::callGuest(app, cpu, function);
    return cpu.eax;
}

// 0x401CD0: the matrix at from copied to to, the middle two floats of each row through the x87.
void copyMatrix(reg32 to, reg32 from)
{
    for (reg32 row = 0; row < 0x40; row += 0x10)
    {
        const reg32 a = rc::ld32(from + row), d = rc::ld32(from + row + 0xc);
        const float b = pass(rc::ldf32(from + row + 4)), c = pass(rc::ldf32(from + row + 8));
        rc::stf32(to + row + 4, b);
        rc::st32(to + row, a);
        rc::stf32(to + row + 8, c);
        rc::st32(to + row + 0xc, d);
    }
}

// 0x565230: the transpose of the matrix at from written to to, elements 1 and 2 through the x87.
void transposeMatrix(reg32 to, reg32 from)
{
    reg32 m[16];
    for (reg32 i = 0; i < 16; ++i)
        m[i] = rc::ld32(from + 4 * i);
    const float m1 = pass(rc::ldf32(from + 4)), m2 = pass(rc::ldf32(from + 8));
    rc::stf32(to + 0x10, m1);
    rc::stf32(to + 0x20, m2);
    for (reg32 r = 0; r < 4; ++r)
        for (reg32 c = 0; c < 4; ++c)
        {
            if (r == 0 && (c == 1 || c == 2))
                continue;
            rc::st32(to + 4 * (c * 4 + r), m[r * 4 + c]);
        }
}

// The guest body of 0x468FE0 with esp = e at its entry; false when guest code it called terminated the thread.
bool interpolate(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 e = cpu.esp, body = cpu.ecx, esi = cpu.esi, edi = cpu.edi;
    const reg32 positionOut = rc::ld32(e + 4), matrixOut = rc::ld32(e + 8);
    const double time = ld(e + 0xc);
    const reg32 frame = ((e - 4) & ~0xfu) - 0x70;
    const reg32 position = getter(app, cpu, body, 8, frame);
    if (cpu.terminate)
        return false;
    const reg32 matrix = getter(app, cpu, body, 0x14, frame);
    if (cpu.terminate)
        return false;
    const double step = ld(body + 0x70);
    const double limit = ld(InterpolateLimit);
    cpu.fpu.compare(step, limit);
    if (step < limit)
    {
        const reg32 x = rc::ld32(position);
        const float y = pass(rc::ldf32(position + 4)), z = pass(rc::ldf32(position + 8));
        rc::st32(positionOut, x);
        rc::stf32(positionOut + 4, y);
        rc::stf32(positionOut + 8, z);
        copyMatrix(matrixOut, matrix);
        cpu.ecx = rc::ld32(matrix + 0x30);
        cpu.edx = rc::ld32(matrix + 0x3c);
    }
    else
    {
        const double s = (step - time) / step;
        const reg32 start = body + 0xc0, startMatrix = body + 0x80;
        const double dx = ld(position) - ld(start);
        const double dy = ld(position + 4) - ld(start + 4);
        const float dz = float(ld(position + 8) - ld(start + 8));
        float d[16];
        for (reg32 k = 0; k < 16; ++k)
            d[k] = float(ld(matrix + 4 * k) - ld(startMatrix + 4 * k));
        const float y = float(dy * s + ld(start + 4));
        const float z = float(double(dz) * s + ld(start + 8));
        const float x = float(dx * s + ld(start));
        rc::stf32(positionOut, x);
        rc::stf32(positionOut + 4, y);
        rc::stf32(positionOut + 8, z);
        float out[16];
        for (reg32 k = 0; k < 16; ++k)
        {
            const double base = ld(startMatrix + 4 * k);
            const bool rounded = (k & 3) >= 2 || k == 13;
            const double scaled = rounded ? double(float(double(d[k]) * s)) : double(d[k]) * s;
            out[k] = float(scaled + base);
        }
        for (reg32 k = 0; k < 16; ++k)
            rc::stf32(matrixOut + 4 * k, out[k]);
        cpu.ecx = rc::ld32(body + 0xb8);
        cpu.edx = rc::ld32(body + 0xbc);
    }
    cpu.eax = matrixOut;
    cpu.esi = esi;
    cpu.edi = edi;
    cpu.esp = e + 0x10;
    return true;
}

// |a - b| > [0x6CC8BC] for each axis as the guest compares them, eax's low half the status word of the last compare.
bool moved(x86::CPU& cpu, reg32 a, reg32 b, reg32& eax)
{
    const double epsilon = ld(MoveEpsilon);
    for (reg32 k = 0; k < 12; k += 4)
    {
        const double d = cpu.fpu.abs(ld(a + k) - ld(b + k));
        cpu.fpu.compare(d, epsilon);
        eax = (eax & 0xffff0000u) | x86::reg16(cpu.fpu.status.word);
        if (d >= epsilon && std::islessgreater(d, epsilon))
            return true;
    }
    return false;
}

// The guest body of 0x472830 with esp = e at its entry.
void witnessMatrices(x86::CPU& cpu)
{
    const reg32 e = cpu.esp, witness = cpu.esi, positionA = cpu.edi;
    const reg32 matrixA = rc::ld32(e + 4), matrixB = rc::ld32(e + 8), positionB = rc::ld32(e + 0xc);
    reg32 eax = cpu.eax, ecx = cpu.ecx, edx = cpu.edx;
    bool mark = moved(cpu, positionA, witness + 0x40, eax);
    if (!mark)
    {
        // 0x471D60 with ecx = the kept position B, edx = position B: eax 1 when it did not move.
        ecx = witness + 0x50;
        edx = positionB;
        mark = moved(cpu, positionB, witness + 0x50, eax);
        eax = mark ? 0 : 1;
    }
    if (mark)
        rc::st8(witness + 0x240, 1);
    eax = (eax & 0xffffff00u) | rc::ld8(witness + 0x240);
    if (rc::ld8(witness + 0x240) != 0)
    {
        copyMatrix(witness + 0x140, matrixA);
        transposeMatrix(witness + 0x180, matrixA);
        const reg32 ax = rc::ld32(positionA);
        const float ay = pass(rc::ldf32(positionA + 4)), az = pass(rc::ldf32(positionA + 8));
        rc::st32(witness + 0x170, ax);
        rc::stf32(witness + 0x174, ay);
        rc::stf32(witness + 0x178, az);
        rc::st32(witness + 0x17c, 0x3f800000u);
        copyMatrix(witness + 0x1c0, matrixB);
        transposeMatrix(witness + 0x200, matrixB);
        const reg32 bx = rc::ld32(positionB);
        const float by = pass(rc::ldf32(positionB + 4)), bz = pass(rc::ldf32(positionB + 8));
        rc::st32(witness + 0x1f0, bx);
        rc::stf32(witness + 0x1f4, by);
        rc::stf32(witness + 0x1f8, bz);
        rc::st32(witness + 0x1fc, 0x3f800000u);
        const reg32 ax2 = rc::ld32(positionA);
        const float ay2 = pass(rc::ldf32(positionA + 4)), az2 = pass(rc::ldf32(positionA + 8));
        rc::st32(witness + 0x40, ax2);
        rc::stf32(witness + 0x44, ay2);
        rc::stf32(witness + 0x48, az2);
        const reg32 bx2 = rc::ld32(positionB);
        const float by2 = pass(rc::ldf32(positionB + 4)), bz2 = pass(rc::ldf32(positionB + 8));
        rc::st32(witness + 0x50, bx2);
        rc::stf32(witness + 0x54, by2);
        rc::stf32(witness + 0x58, bz2);
        eax = bx2;
        ecx = bx;
        edx = ax2;
    }
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.esp = e + 0x10;
}

// ---- shadow compare ----

struct Span
{
    reg32 address, size;
};

struct ShadowCount
{
    const char* function;
    reg32 address;
    uint32_t compared, differing, logged;
};

ShadowCount s_interpolateShadow = {"Interpolate 0x468fe0", 0x468fe0, 0, 0, 0};
ShadowCount s_matricesShadow = {"witness matrices 0x472830", 0x472830, 0, 0, 0};
const uint32_t ShadowLogLimit = 20;

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

// The guest runs first; the spans are put back and native runs from the same entry state, then the guest's memory is
// put back again. The x87 depth, the registers and flags retstate lists, and the span bytes are compared.
template <typename Native>
void shadowRun(win32::WinApplication* app, x86::CPU& cpu, void (*guest)(win32::WinApplication*, x86::CPU&),
               const std::vector<Span>& spans, ShadowCount& count, Native native)
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
    const bool ran = native(n);
    save(spans, nativeAfter);
    restore(spans, guestAfter);
    if (!ran)
        return;
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
        for (reg32 k = 0; k < s.size; ++k)
        {
            if (guestAfter[at + k] != nativeAfter[at + k])
            {
                snprintf(text, sizeof(text), "byte at 0x%08x (+0x%x) guest 0x%02x native 0x%02x",
                         unsigned(s.address + k), unsigned(k), unsigned(guestAfter[at + k]),
                         unsigned(nativeAfter[at + k]));
                same = false;
                break;
            }
        }
        at += s.size;
    }
    if (same && (!retstate::sameFlags(retstate::flags(count.address) | retstate::Fc, cpu, n, text, sizeof(text)) ||
                 !retstate::sameRegisters(retstate::registers(count.address), cpu, n, text, sizeof(text))))
        same = false;
    if (same)
        return;
    ++count.differing;
    if (count.logged < ShadowLogLimit)
    {
        ++count.logged;
        pf::logInfo("shadow_contact: %s (caller 0x%08x): %s", count.function, unsigned(caller), text);
    }
}

}

void logShadowTotal()
{
    if (settings().native && settings().shadow)
        pf::logInfo("shadow_contact: %u calls compared (interpolate %u, witness matrices %u), %u differ",
                    unsigned(s_interpolateShadow.compared + s_matricesShadow.compared),
                    unsigned(s_interpolateShadow.compared), unsigned(s_matricesShadow.compared),
                    unsigned(s_interpolateShadow.differing + s_matricesShadow.differing));
}

}

namespace native
{

// DynamicCollisionBody::Interpolate (guest 0x468FE0).
void sub_468fe0(win32::WinApplication* app, x86::CPU& cpu)
{
    const contact::Settings& s = contact::settings();
    if (!s.native)
    {
        sub_468fe0_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0))
    {
        const x86::reg32 e = cpu.esp;
        contact::shadowRun(app, cpu, sub_468fe0_guest,
                           {contact::Span{rc::ld32(e + 4), 12}, contact::Span{rc::ld32(e + 8), 0x40}},
                           contact::s_interpolateShadow,
                           [app](x86::CPU& n) { return contact::interpolate(app, n); });
        return;
    }
    contact::interpolate(app, cpu);
}

// The matrices of a witness (guest 0x472830).
void sub_472830(win32::WinApplication* app, x86::CPU& cpu)
{
    const contact::Settings& s = contact::settings();
    if (!s.native)
    {
        sub_472830_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0))
    {
        contact::shadowRun(app, cpu, sub_472830_guest, {contact::Span{cpu.esi + 0x40, 0x204}},
                           contact::s_matricesShadow,
                           [](x86::CPU& n)
                           {
                               contact::witnessMatrices(n);
                               return true;
                           });
        return;
    }
    contact::witnessMatrices(cpu);
}

}
}
