#include <cpu.h>
#include <platform/log.h>
#include <platform/time.h>
#include <cstring>

namespace x86
{

void assertFail(const char* expression, const char* file, int line)
{
    pf::logError("assertion failed: %s (%s:%d)", expression, file, line);
#ifndef NFSU_ASSERT_SOFT
    __builtin_trap();
#endif
}

void fpuCallMismatch(CPU& cpu, reg32 expected, uint32_t site)
{
    // Guest code runs under the execution lock: no other thread is here.
    static uint32_t logged[64];
    static uint32_t loggedCount = 0;
    bool known = false;
    for (uint32_t i = 0; i < loggedCount && !known; ++i)
        known = logged[i] == site;
    if (!known && loggedCount < 64)
    {
        logged[loggedCount++] = site;
        pf::logError("x87 depth %u after the indirect call at 0x%08x, expected %d: corrected", cpu.fpu.count, site,
                     int(expected));
    }
    if (expected > 8)
        return;
    // A surplus is a float result nobody reads; a missing result reads as zero.
    while (cpu.fpu.count < expected)
        cpu.fpu.push(Float(0.0));
    cpu.fpu.count = expected;
}

void flagAuditFail(CPU& cpu, reg32 flags, uint32_t site)
{
    static uint32_t logged[256];
    static uint32_t loggedCount = 0;
    bool known = false;
    for (uint32_t i = 0; i < loggedCount && !known; ++i)
        known = logged[i] == site;
    if (!known && loggedCount < 256)
    {
        logged[loggedCount++] = site;
        static const char* const names[] = {"cf", "pf", "af", "zf", "sf", "of", "c0c2c3"};
        char text[40] = "";
        for (int i = 0; i < 7; ++i)
        {
            if (cpu.flagsUnknown & flags & (1u << i))
            {
                strcat(text, " ");
                strcat(text, names[i]);
            }
        }
        pf::logError("flag audit: 0x%08x reads flags that were not stored:%s", site, text);
    }
}

void guestTrap(uint32_t address, const char* reason)
{
    pf::logError("guest trap at 0x%08x: %s", address, reason);
    __builtin_trap();
}

extern const uint32_t TimestampMhz = 500;

// The time stamp counter of a TimestampMhz CPU.
void CPU::rdtsc()
{
    edx_eax = pf::ticksUs() * TimestampMhz;
}

// The CPU the guest sees (docs/re/cpu_threads.md section 1): a family 5 x86 of an unknown vendor, "VitaRecomp86",
// so the game's CPU class is the default one and it skips its REALTIME/affinity MHz measurement. Leaf 1 reports
// fpu vme de pse tsc msr mce cx8 and no MMX, SSE or SSE2; there are no extended leaves (0x80000000 answers
// 0x80000000, and 0x80000001, which the game reads without checking, is all zero), so 3DNow! is off too. Every
// SIMD path of Speed.exe is gated on these bits. A basic leaf above the highest one returns leaf 1, as Intel CPUs do.
void CPU::cpuid()
{
    const reg32 leaf = eax;
    if (leaf == 0)
    {
        eax = 1;
        ebx = 0x61746956;   // "Vita"
        edx = 0x6f636552;   // "Reco"
        ecx = 0x3638706d;   // "mp86"
        return;
    }
    if (leaf >= 0x80000000)
    {
        eax = leaf == 0x80000000 ? 0x80000000 : 0;
        ebx = 0;
        ecx = 0;
        edx = 0;
        return;
    }
    eax = 0x0543;
    ebx = 0x0000;
    ecx = 0x0000;
    edx = 0x000001bf;
}

// Environment layout (32-bit protected mode): control, status, tag, ip, cs/opcode, data offset, data selector; each
// field one dword.
static const reg32 EnvironmentSize = 28;

static void putWord(reg8* memory, reg32 offset, reg32 value)
{
    memcpy(memory + offset, &value, 4);
}

static reg32 getWord(const reg8* memory, reg32 offset)
{
    reg32 value;
    memcpy(&value, memory + offset, 4);
    return value;
}

// TOP: x87 pushes decrement it, so after count pushes from an empty stack it is -count mod 8.
void FPU::storeEnvironment(reg8* memory)
{
    const reg32 top = (8 - (count & 7)) & 7;
    reg32 tag = 0xffff;
    for (reg32 i = 0; i < count && i < 8; ++i)
        tag &= ~(3u << (2 * ((top + i) & 7)));
    putWord(memory, 0, 0xffff0000u | control.word);
    putWord(memory, 4, 0xffff0000u | (status.word & ~0x3800u) | (top << 11));
    putWord(memory, 8, 0xffff0000u | tag);
    for (reg32 offset = 12; offset < EnvironmentSize; offset += 4)
        putWord(memory, offset, 0);
    // fnstenv masks all exceptions after storing.
    control.word |= 0x3f;
}

// Loads the control word and the status bits; the stack depth stays as it is (TOP is not modelled).
void FPU::loadEnvironment(const reg8* memory)
{
    control.word = reg16(getWord(memory, 0));
    status.word = reg16(getWord(memory, 4) & ~0x3800u);
}

void FPU::save(reg8* memory)
{
    storeEnvironment(memory);
    for (reg32 i = 0; i < 8; ++i)
    {
        const IEEEf80 value{double(st(int(i)))};
        memcpy(memory + EnvironmentSize + 10 * i, &value.data, 10);
    }
    init();
}

void FPU::restore(const reg8* memory)
{
    loadEnvironment(memory);
    const reg32 tag = getWord(memory, 8) & 0xffff;
    const reg32 top = (getWord(memory, 4) >> 11) & 7;
    reg32 depth = 0;
    while (depth < 8 && ((tag >> (2 * ((top + depth) & 7))) & 3) != 3)
        ++depth;
    count = depth;
    for (reg32 i = 0; i < 8; ++i)
    {
        IEEEf80Data data;
        memcpy(&data, memory + EnvironmentSize + 10 * i, 10);
        st(int(i)) = Float(double(IEEEf80(data)));
    }
}

}

extern "C" void convert80x64(const x86::IEEEf80Data *fp80, double *fp64)
{
    /* Read the 10 raw bytes as a little-endian 80-bit extended-precision
     * value.  Layout (bit numbering from LSB):
     *   bits  0-15  : data1  (mantissa low)
     *   bits 16-31  : data2
     *   bits 32-47  : data3
     *   bits 48-63  : data4  (mantissa high, bit 63 = explicit integer bit)
     *   bits 64-79  : data5  (bit 79 = sign, bits 78-64 = 15-bit exponent)
     */
    uint64_t mantissa = uint64_t(uint16_t(fp80->data1))
                      | (uint64_t(uint16_t(fp80->data2)) << 16)
                      | (uint64_t(uint16_t(fp80->data3)) << 32)
                      | (uint64_t(uint16_t(fp80->data4)) << 48);
    uint16_t expSign  = uint16_t(fp80->data5);
    uint32_t sign     = (expSign >> 15) & 1;
    uint32_t exp80    = expSign & 0x7FFF;

    uint64_t result;

    if (exp80 == 0)
    {
        /* Zero or denormal.  80-bit denormals are too tiny for double, so
         * flush to signed zero. */
        result = uint64_t(sign) << 63;
    }
    else if (exp80 == 0x7FFF)
    {
        /* Infinity or NaN.  Preserve the sign and NaN payload (top 51 bits
         * of the 63-bit fractional mantissa, skipping the integer bit). */
        uint64_t frac64 = mantissa & 0x7FFFFFFFFFFFFFFFULL; /* strip integer bit */
        uint64_t frac52 = frac64 >> 11;
        if (frac52 == 0 && (mantissa & 0x7FFFFFFFFFFFFFFFULL) != 0)
            frac52 = 1; /* keep NaN-ness: ensure at least one bit is set */
        result = (uint64_t(sign) << 63)
               | (uint64_t(0x7FF) << 52)
               | frac52;
    }
    else
    {
        /* Normal number.  Re-bias exponent: 80-bit bias = 16383, 64-bit = 1023 */
        int32_t unbiased = int32_t(exp80) - 16383;

        if (unbiased > 1023)
        {
            /* Overflow → infinity */
            result = (uint64_t(sign) << 63) | (uint64_t(0x7FF) << 52);
        }
        else if (unbiased < -1022)
        {
            /* Underflow → flush to signed zero (could denormalise, but for
             * game code this is perfectly fine). */
            result = uint64_t(sign) << 63;
        }
        else
        {
            uint32_t exp64 = uint32_t(unbiased + 1023);
            /* The 80-bit mantissa has an explicit integer bit (bit 63).
             * IEEE-754 double has an implicit one-bit, so we take the top
             * 52 fraction bits (bits 62..11) of the 80-bit mantissa. */
            uint64_t frac52 = (mantissa >> 11) & 0x000FFFFFFFFFFFFFULL;
            result = (uint64_t(sign) << 63)
                   | (uint64_t(exp64) << 52)
                   | frac52;
        }
    }

    memcpy(fp64, &result, sizeof(result));
}

extern "C" void convert64x80(const double *fp64, x86::IEEEf80Data *fp80)
{
    uint64_t bits;
    memcpy(&bits, fp64, sizeof(bits));

    uint32_t sign   = uint32_t(bits >> 63);
    uint32_t exp64  = uint32_t((bits >> 52) & 0x7FF);
    uint64_t frac52 = bits & 0x000FFFFFFFFFFFFFULL;

    uint16_t expSign;
    uint64_t mantissa;

    if (exp64 == 0)
    {
        if (frac52 == 0)
        {
            /* Signed zero */
            expSign  = uint16_t(sign << 15);
            mantissa = 0;
        }
        else
        {
            /* Denormal double → normalise into 80-bit.  Shift fraction left
             * until the implicit bit is in position 63. */
            int shift = 0;
            uint64_t m = frac52;
            while ((m & (uint64_t(1) << 52)) == 0)
            {
                m <<= 1;
                shift++;
            }
            /* Now bit 52 of m is 1.  The unbiased exponent is
             * (1 - 1023 - shift) = -1022 - shift.  Re-bias to 80-bit. */
            int32_t unbiased = -1022 - shift;
            uint32_t exp80 = uint32_t(unbiased + 16383);
            expSign  = uint16_t((sign << 15) | exp80);
            /* Place the integer bit explicitly and shift the rest up. */
            mantissa = m << 11; /* bit 63 = integer bit (1) */
        }
    }
    else if (exp64 == 0x7FF)
    {
        /* Infinity or NaN */
        expSign = uint16_t((sign << 15) | 0x7FFF);
        if (frac52 == 0)
        {
            /* Infinity: integer bit set, fraction zero */
            mantissa = uint64_t(1) << 63;
        }
        else
        {
            /* NaN: integer bit set, preserve payload */
            mantissa = (uint64_t(1) << 63) | (frac52 << 11);
            if ((mantissa & 0x7FFFFFFFFFFFFFFFULL) == 0)
                mantissa |= 1; /* keep NaN-ness */
        }
    }
    else
    {
        /* Normal number */
        int32_t unbiased = int32_t(exp64) - 1023;
        uint32_t exp80   = uint32_t(unbiased + 16383);
        expSign  = uint16_t((sign << 15) | exp80);
        /* Reconstruct 64-bit mantissa: explicit integer bit (1) in bit 63,
         * then the 52 fraction bits in bits 62..11. */
        mantissa = (uint64_t(1) << 63) | (frac52 << 11);
    }

    fp80->data1 = x86::reg16(uint16_t(mantissa));
    fp80->data2 = x86::reg16(uint16_t(mantissa >> 16));
    fp80->data3 = x86::reg16(uint16_t(mantissa >> 32));
    fp80->data4 = x86::reg16(uint16_t(mantissa >> 48));
    fp80->data5 = x86::reg16(expSign);
}
