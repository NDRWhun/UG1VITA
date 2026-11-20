// Native kernels of the movie decoder (the MAD codec of the RCMP library): the 8x8 inverse DCT in 16.16 fixed
// point (0x64ABC0 with its row and column passes 0x64AD18 / 0x64AF67), the intra coefficient decoder of a block
// (0x64B110), and the block moves between bytes and the 16.16 work buffers (0x64A694, 0x64A71F, 0x64A7F0, 0x64A8C1,
// 0x64AAA5). Each override writes the guest memory and leaves eax, ecx and edx as the translated code does.
// movie_native=off runs the guest code; shadow_movie=on runs both and logs where they differ.

#include "retstate.h"

#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>

#include <cstdio>
#include <cstring>
#include <vector>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_64abc0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_64b110_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_64a694_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_64a71f_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_64a7f0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_64a8c1_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_64aaa5_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace movie
{

namespace
{

using x86::reg32;
using x86::sreg32;

const reg32 Coefficients = 0x713b50;    // 8x8 int32 block the coefficient decoder fills and the IDCT reads
const reg32 Transposed = 0x713a50;      // 8x8 int32: the row pass results, one column per input row
const reg32 Clamp = 0x713848;           // 512 bytes: pixel of a 9-bit value
const reg32 BitBuffer = 0x712c38;       // the next bits of the stream, most significant first
const reg32 BitCount = 0x712030;        // valid bits in the buffer
const reg32 BitPointer = 0x713a48;      // next 16-bit word of the stream
const reg32 Quantiser = 0x712438;       // int32 per coefficient position
const reg32 ZigZag = 0x6aa8b8;          // int32: coefficient position of the n-th coded coefficient
const reg32 FirstCodes = 0x712c40;      // 512 entries by the top 9 bits
const reg32 LongCodes = 0x712738;       // 256 entries by the top 8 bits after 9 bits
const reg32 MiddleCodes = 0x713448;     // 256 entries by the top 8 bits after 6 bits

struct Settings
{
    bool native, shadow;
};

const Settings& settings()
{
    static const Settings s = [] {
        const Settings r = { pf::Config::getBool("movie_native", true), pf::Config::getBool("shadow_movie", false) };
        pf::logInfo("movie: decoder kernels %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", compared with the guest code (shadow_movie)" : "");
        return r;
    }();
    return s;
}

inline reg32 argument(const x86::CPU& cpu, int n)
{
    return rc::ld32(cpu.esp + 4 + 4 * n);
}

inline sreg32* words(reg32 address)
{
    return reinterpret_cast<sreg32*>(rc::host(address));
}

// The rounding 16.16 product of 0x64AF54: bits 16..47 of the 64-bit product plus bit 15.
inline sreg32 fixedMultiply(sreg32 a, sreg32 b)
{
    const int64_t product = int64_t(a) * int64_t(b);
    return sreg32(uint32_t(uint64_t(product) >> 16) + uint32_t((uint64_t(product) >> 15) & 1));
}

inline sreg32 add(sreg32 a, sreg32 b)
{
    return sreg32(uint32_t(a) + uint32_t(b));
}

inline sreg32 sub(sreg32 a, sreg32 b)
{
    return sreg32(uint32_t(a) - uint32_t(b));
}

// One pass of 0x64AD18 / 0x64AF67: eight inputs p, eight outputs o[0], o[step], ... The row pass copies p[0]
// to every output when p[1..7] are 0 (zeroShortcut).
inline void idctPass(const sreg32* p, sreg32* o, int step, bool zeroShortcut)
{
    if (zeroShortcut && (p[1] | p[2] | p[3] | p[4] | p[5] | p[6] | p[7]) == 0)
    {
        for (int k = 0; k < 8; ++k)
            o[k * step] = p[0];
        return;
    }
    sreg32 a20 = sub(p[5], p[3]);
    sreg32 aC = add(p[1], p[7]);
    sreg32 a10 = sub(p[1], p[7]);
    sreg32 a24 = add(p[5], p[3]);
    sreg32 a1C = sub(aC, a24);
    a24 = add(a24, aC);
    aC = fixedMultiply(add(a20, a10), 0x61f8);
    a20 = add(fixedMultiply(a20, 0x8a8c), aC);
    a1C = fixedMultiply(a1C, 0xb505);
    a10 = sub(fixedMultiply(a10, 0x14e7b), aC);
    a24 = add(a24, a10);
    a10 = add(a10, a1C);
    a1C = add(a1C, a20);
    sreg32 a18 = add(p[0], p[4]);
    sreg32 a8 = sub(p[0], p[4]);
    const sreg32 a14 = fixedMultiply(sub(p[2], p[6]), 0xb505);
    sreg32 a4 = add(add(p[2], p[6]), a14);
    aC = sub(a18, a4);
    a18 = add(a18, a4);
    a4 = sub(a8, a14);
    a8 = add(a8, a14);
    o[0 * step] = add(a18, a24);
    o[1 * step] = add(a8, a10);
    o[2 * step] = add(a4, a1C);
    o[3 * step] = add(aC, a20);
    o[4 * step] = sub(aC, a20);
    o[5 * step] = sub(a4, a1C);
    o[6 * step] = sub(a8, a10);
    o[7 * step] = sub(a18, a24);
}

// 0x64ABC0 (dst, stride in int32s): rows of the coefficient block into the columns of the transposed block, then
// its rows into dst.
void idctNative(x86::CPU& cpu)
{
    const reg32 dst = argument(cpu, 0), stride = argument(cpu, 1);
    const sreg32* in = words(Coefficients);
    sreg32* temp = words(Transposed);
    for (int row = 0; row < 8; ++row)
        idctPass(in + row * 8, temp + row, 8, true);
    reg32 out = dst;
    for (int row = 0; row < 8; ++row)
    {
        out = dst + reg32(row) * stride * 4;
        idctPass(temp + row * 8, words(out), 1, false);
    }
    cpu.eax = rc::ld32(out + 0x1c);
    cpu.ecx = out;
    cpu.edx = out;
    cpu.esp += 4;
}

// 0x64B2C7: drops n bits and refills 16 bits from the stream when fewer than 16 are left. Leaves ecx and edx as
// the guest does; edx keeps its value when there is no refill.
void skipBits(reg32 n, reg32& ecx, reg32& edx)
{
    reg32 buffer = rc::ld32(BitBuffer) << (n & 31);
    sreg32 count = sreg32(rc::ld32(BitCount)) - sreg32(n);
    rc::st32(BitBuffer, buffer);
    rc::st32(BitCount, reg32(count));
    ecx = reg32(count);
    if (count >= 16)
        return;
    const reg32 pointer = rc::ld32(BitPointer);
    const reg32 word = rc::ld16(pointer);
    buffer |= word << ((16 - reg32(count)) & 31);
    count += 16;
    rc::st32(BitBuffer, buffer);
    rc::st32(BitCount, reg32(count));
    rc::st32(BitPointer, pointer + 2);
    ecx = buffer;
    edx = reg32(count);
}

// 0x64B110: the DC value from 8 bits, then run/level codes until the end-of-block code; returns the position
// after the last coefficient (1 when there is only the DC value).
void intraCoefficientsNative(x86::CPU& cpu)
{
    reg32 ecx = cpu.ecx, edx = cpu.edx;
    rc::st32(Coefficients, reg32(sreg32(rc::ld32(BitBuffer)) >> 24) * rc::ld32(Quantiser));
    skipBits(8, ecx, edx);
    memset(rc::host(Coefficients + 4), 0, 63 * 4);
    reg32 position = 1;
    for (;;)
    {
        reg32 entry = rc::ld32(FirstCodes + (rc::ld32(BitBuffer) >> 23) * 4);
        reg32 length = entry & 0xff;
        edx = entry;
        if (length > 9)
        {
            if (!(length & 0x20))
            {
                if (!(length & 0x10))
                {
                    skipBits(9, ecx, edx);
                    entry = rc::ld32(LongCodes + (rc::ld32(BitBuffer) >> 24) * 4);
                }
                else
                {
                    skipBits(6, ecx, edx);
                    entry = rc::ld32(MiddleCodes + (rc::ld32(BitBuffer) >> 24) * 4);
                }
                length = entry & 0xff;
            }
            else if (!(length & 0x10))
            {
                skipBits(6, ecx, edx);
                entry = rc::ld32(BitBuffer);
                length = 0x10;
            }
            else
            {
                skipBits(2, ecx, edx);
                cpu.eax = position;
                cpu.ecx = ecx;
                cpu.edx = edx;
                cpu.esp += 4;
                return;
            }
        }
        skipBits(length, ecx, edx);
        position += reg32(sreg32(entry) >> 16) & 0x3f;
        const reg32 at = rc::ld32(ZigZag + position * 4);
        const reg32 level = reg32(sreg32(entry) >> 22);
        rc::st32(Coefficients + at * 4, level * rc::ld32(Quantiser + at * 4));
        ++position;
    }
}

// 0x64A694 (dst, stride in int32s): 8 rows of 8 int32s set to the DC value of the coefficient block.
void fillNative(x86::CPU& cpu)
{
    const reg32 dst = argument(cpu, 0), stride = argument(cpu, 1);
    const reg32 value = rc::ld32(Coefficients);
    for (reg32 row = 0; row < 8; ++row)
    {
        sreg32* out = words(dst + row * stride * 4);
        for (int i = 0; i < 8; ++i)
            out[i] = sreg32(value);
    }
    cpu.eax = 8;
    cpu.ecx = dst + 8 * stride * 4;
    cpu.edx = stride;
    cpu.esp += 4;
}

// 0x64A71F / 0x64A7F0 (src, src stride, dst, bias): 8 rows of 8 bytes plus bias as 16.16 values; dst rows are
// dstRow int32s apart.
void loadNative(x86::CPU& cpu, reg32 dstRow)
{
    const reg32 src = argument(cpu, 0), srcStride = argument(cpu, 1), dst = argument(cpu, 2), bias = argument(cpu, 3);
#if defined(__ARM_NEON)
    const uint32x4_t addend = vdupq_n_u32(bias);
    for (reg32 row = 0; row < 8; ++row)
    {
        const uint16x8_t in = vmovl_u8(vld1_u8(rc::host(src + row * srcStride)));
        uint32_t* out = reinterpret_cast<uint32_t*>(rc::host(dst + row * dstRow * 4));
        vst1q_u32(out, vshlq_n_u32(vaddq_u32(vmovl_u16(vget_low_u16(in)), addend), 16));
        vst1q_u32(out + 4, vshlq_n_u32(vaddq_u32(vmovl_u16(vget_high_u16(in)), addend), 16));
    }
#else
    for (reg32 row = 0; row < 8; ++row)
    {
        const uint8_t* in = rc::host(src + row * srcStride);
        reg32* out = reinterpret_cast<reg32*>(rc::host(dst + row * dstRow * 4));
        for (int i = 0; i < 8; ++i)
            out[i] = (reg32(in[i]) + bias) << 16;
    }
#endif
    cpu.eax = 8;
    cpu.ecx = src + 8 * srcStride;
    cpu.edx = dst + 8 * dstRow * 4;
    cpu.esp += 4;
}

// 0x64A8C1 (16 rows of 16) / 0x64AAA5 (8 rows of 8) (src, dst, dst stride): each 16.16 value to the pixel of its
// bits 16..24 through the clamp table.
// True when the clamp table holds what 0x649EAE writes: for s in -256..254 the entry at s & 0x1ff is
// clamp(s, -128, 127) + 128. Entry 0xff is not written there. Only that function writes the table, always with these
// values, so a table found so once stays so.
bool standardClamp(const uint8_t* clamp)
{
    static bool standard = false;
    if (standard)
        return true;
    for (int s = -256; s < 255; ++s)
    {
        const int v = s < -128 ? -128 : s > 127 ? 127 : s;
        if (clamp[s & 0x1ff] != uint8_t(v + 128))
            return false;
    }
    standard = true;
    return true;
}

// Eight 16.16 values to pixels with the standard clamp table; false (nothing written) when one of them reads entry
// 0xff.
inline bool storeEight(const sreg32* in, uint8_t* out)
{
#if defined(__ARM_NEON)
    const int32x4_t a = vshrq_n_s32(vshlq_n_s32(vld1q_s32(in), 7), 23);
    const int32x4_t b = vshrq_n_s32(vshlq_n_s32(vld1q_s32(in + 4), 7), 23);
    const int32x4_t unwritten = vdupq_n_s32(255);
    const uint32x4_t hits = vorrq_u32(vceqq_s32(a, unwritten), vceqq_s32(b, unwritten));
    const uint32x2_t hit = vorr_u32(vget_low_u32(hits), vget_high_u32(hits));
    if (vget_lane_u32(hit, 0) | vget_lane_u32(hit, 1))
        return false;
    const int32x4_t offset = vdupq_n_s32(128);
    const uint16x8_t wide = vcombine_u16(vqmovun_s32(vaddq_s32(a, offset)), vqmovun_s32(vaddq_s32(b, offset)));
    vst1_u8(out, vqmovn_u16(wide));
    return true;
#else
    (void)in;
    (void)out;
    return false;
#endif
}

void storeNative(x86::CPU& cpu, reg32 size)
{
    const reg32 src = argument(cpu, 0), dst = argument(cpu, 1), stride = argument(cpu, 2);
    const uint8_t* clamp = rc::host(Clamp);
    const bool standard = standardClamp(clamp);
    for (reg32 row = 0; row < size; ++row)
    {
        const sreg32* in = words(src + row * size * 4);
        uint8_t* out = rc::host(dst + row * stride);
        if (standard && storeEight(in, out) && (size == 8 || storeEight(in + 8, out + 8)))
            continue;
        for (reg32 i = 0; i < size; ++i)
            out[i] = clamp[(in[i] >> 16) & 0x1ff];
    }
    const reg32 lastRow = src + (size - 1) * size * 4;
    const reg32 lastPixel = (rc::ld32(lastRow + (size - 1) * 4) >> 16) & 0x1ff;
    const reg32 carrier = (lastRow & 0xffffff00) | clamp[lastPixel];
    cpu.eax = size;
    if (size == 16)
    {
        cpu.ecx = carrier;
        cpu.edx = src + size * size * 4;
    }
    else
    {
        cpu.ecx = dst + size * stride;
        cpu.edx = carrier;
    }
    cpu.esp += 4;
}

// ---- Shadow compares -----------------------------------------------------------------------------------------------

struct Span
{
    reg32 address, size;
};

struct ShadowCount
{
    const char* name;
    uint32_t calls, differing, logged;
};

// Runs the native code on a copy of the CPU, then the guest code on the restored memory, and compares the spans
// and the registers the callers read (retstate).
void shadow(ShadowCount& count, reg32 function, win32::WinApplication* app, x86::CPU& cpu,
            void (*native)(x86::CPU&), void (*guest)(win32::WinApplication*, x86::CPU&),
            const std::vector<Span>& spans)
{
    static std::vector<uint8_t> before, after;
    before.clear();
    for (const Span& span : spans)
        before.insert(before.end(), rc::host(span.address), rc::host(span.address) + span.size);
    x86::CPU n = cpu;
    native(n);
    after.clear();
    for (const Span& span : spans)
        after.insert(after.end(), rc::host(span.address), rc::host(span.address) + span.size);
    size_t at = 0;
    for (const Span& span : spans)
    {
        memcpy(rc::host(span.address), before.data() + at, span.size);
        at += span.size;
    }
    guest(app, cpu);
    char text[160] = {};
    bool same = retstate::sameRegisters(retstate::registers(function), cpu, n, text, sizeof(text));
    at = 0;
    for (const Span& span : spans)
    {
        for (reg32 k = 0; same && k < span.size; ++k)
        {
            if (rc::ld8(span.address + k) != after[at + k])
            {
                snprintf(text, sizeof(text), "byte at 0x%08x guest 0x%02x native 0x%02x", unsigned(span.address + k),
                         unsigned(rc::ld8(span.address + k)), unsigned(after[at + k]));
                same = false;
            }
        }
        at += span.size;
    }
    ++count.calls;
    if (!same)
    {
        ++count.differing;
        if (count.logged < 8)
        {
            ++count.logged;
            pf::logInfo("shadow_movie: %s: %s", count.name, text);
        }
    }
    if ((count.calls & 4095) == 0)
        pf::logInfo("shadow_movie: %s: %u calls compared, %u differ", count.name, unsigned(count.calls),
                    unsigned(count.differing));
}

// Rows of a block: rows of width bytes, stride apart.
void addRows(std::vector<Span>& spans, reg32 address, reg32 rows, reg32 width, reg32 stride)
{
    for (reg32 row = 0; row < rows; ++row)
        spans.push_back({address + row * stride, width});
}

ShadowCount s_idctShadow = {"IDCT 0x64abc0", 0, 0, 0};
ShadowCount s_coefficientShadow = {"intra coefficients 0x64b110", 0, 0, 0};
ShadowCount s_fillShadow = {"DC fill 0x64a694", 0, 0, 0};
ShadowCount s_load16Shadow = {"load 0x64a71f", 0, 0, 0};
ShadowCount s_load8Shadow = {"load 0x64a7f0", 0, 0, 0};
ShadowCount s_store16Shadow = {"store 0x64a8c1", 0, 0, 0};
ShadowCount s_store8Shadow = {"store 0x64aaa5", 0, 0, 0};

void load16(x86::CPU& cpu) { loadNative(cpu, 16); }
void load8(x86::CPU& cpu) { loadNative(cpu, 8); }
void store16(x86::CPU& cpu) { storeNative(cpu, 16); }
void store8(x86::CPU& cpu) { storeNative(cpu, 8); }

}

}

namespace native
{

using x86::reg32;

void sub_64abc0(win32::WinApplication* app, x86::CPU& cpu)
{
    const movie::Settings& s = movie::settings();
    if (!s.native)
        return sub_64abc0_guest(app, cpu);
    if (__builtin_expect(s.shadow, 0))
    {
        std::vector<movie::Span> spans = {{movie::Transposed, 256}};
        movie::addRows(spans, rc::ld32(cpu.esp + 4), 8, 32, rc::ld32(cpu.esp + 8) * 4);
        return movie::shadow(movie::s_idctShadow, 0x64abc0, app, cpu, &movie::idctNative, &sub_64abc0_guest, spans);
    }
    movie::idctNative(cpu);
}

void sub_64b110(win32::WinApplication* app, x86::CPU& cpu)
{
    const movie::Settings& s = movie::settings();
    if (!s.native)
        return sub_64b110_guest(app, cpu);
    if (__builtin_expect(s.shadow, 0))
        return movie::shadow(movie::s_coefficientShadow, 0x64b110, app, cpu, &movie::intraCoefficientsNative, &sub_64b110_guest,
                             {{movie::Coefficients, 256}, {movie::BitBuffer, 4}, {movie::BitCount, 4},
                              {movie::BitPointer, 4}});
    movie::intraCoefficientsNative(cpu);
}

void sub_64a694(win32::WinApplication* app, x86::CPU& cpu)
{
    const movie::Settings& s = movie::settings();
    if (!s.native)
        return sub_64a694_guest(app, cpu);
    if (__builtin_expect(s.shadow, 0))
    {
        std::vector<movie::Span> spans;
        movie::addRows(spans, rc::ld32(cpu.esp + 4), 8, 32, rc::ld32(cpu.esp + 8) * 4);
        return movie::shadow(movie::s_fillShadow, 0x64a694, app, cpu, &movie::fillNative, &sub_64a694_guest, spans);
    }
    movie::fillNative(cpu);
}

void sub_64a71f(win32::WinApplication* app, x86::CPU& cpu)
{
    const movie::Settings& s = movie::settings();
    if (!s.native)
        return sub_64a71f_guest(app, cpu);
    if (__builtin_expect(s.shadow, 0))
    {
        std::vector<movie::Span> spans;
        movie::addRows(spans, rc::ld32(cpu.esp + 0xc), 8, 32, 64);
        return movie::shadow(movie::s_load16Shadow, 0x64a71f, app, cpu, &movie::load16, &sub_64a71f_guest, spans);
    }
    movie::loadNative(cpu, 16);
}

void sub_64a7f0(win32::WinApplication* app, x86::CPU& cpu)
{
    const movie::Settings& s = movie::settings();
    if (!s.native)
        return sub_64a7f0_guest(app, cpu);
    if (__builtin_expect(s.shadow, 0))
    {
        std::vector<movie::Span> spans;
        movie::addRows(spans, rc::ld32(cpu.esp + 0xc), 8, 32, 32);
        return movie::shadow(movie::s_load8Shadow, 0x64a7f0, app, cpu, &movie::load8, &sub_64a7f0_guest, spans);
    }
    movie::loadNative(cpu, 8);
}

void sub_64a8c1(win32::WinApplication* app, x86::CPU& cpu)
{
    const movie::Settings& s = movie::settings();
    if (!s.native)
        return sub_64a8c1_guest(app, cpu);
    if (__builtin_expect(s.shadow, 0))
    {
        std::vector<movie::Span> spans;
        movie::addRows(spans, rc::ld32(cpu.esp + 8), 16, 16, rc::ld32(cpu.esp + 0xc));
        return movie::shadow(movie::s_store16Shadow, 0x64a8c1, app, cpu, &movie::store16, &sub_64a8c1_guest, spans);
    }
    movie::storeNative(cpu, 16);
}

void sub_64aaa5(win32::WinApplication* app, x86::CPU& cpu)
{
    const movie::Settings& s = movie::settings();
    if (!s.native)
        return sub_64aaa5_guest(app, cpu);
    if (__builtin_expect(s.shadow, 0))
    {
        std::vector<movie::Span> spans;
        movie::addRows(spans, rc::ld32(cpu.esp + 8), 8, 8, rc::ld32(cpu.esp + 0xc));
        return movie::shadow(movie::s_store8Shadow, 0x64aaa5, app, cpu, &movie::store8, &sub_64aaa5_guest, spans);
    }
    movie::storeNative(cpu, 8);
}

}

}
