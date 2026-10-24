// Native resource loading: the JDLZ decompressor (guest 0x58d190), the texture creation of a texture record
// (0x407db0), the car skin compositor CompositeSkin32 (0x5a8a50) and its weighted pixel sum (0x5a8640).
//
// Bit-exact with the translated guest code: the same guest memory stores (stack frames included, with their final
// values), the registers and flags the translated code leaves in cpu, the x87 status of its last compare and the
// x87 slot above the stack it spills at its calls. x87 chains are evaluated in double in the guest's order and
// rounded to float where the guest stores a dword.

#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <lib/memmap.h>
#include <lib/winapp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fpu.h>
#include <recomp.h>
#include <vector>
#include <x86.h>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_58d190_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5a8640_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5a8a50_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_407db0_guest(win32::WinApplication* app, x86::CPU& cpu);
// Guest functions the native code calls (generated).
void sub_408010(win32::WinApplication* app, x86::CPU& cpu);
void sub_67121a(win32::WinApplication* app, x86::CPU& cpu);
void sub_671102(win32::WinApplication* app, x86::CPU& cpu);

namespace load
{

namespace
{

using x86::reg8;
using x86::reg16;
using x86::reg32;
using x86::sreg16;
using x86::sreg32;
using x86::sreg64;

// Float constants in .rdata.
const reg32 ByteScale = 0x6cc970;       // 1/255
const reg32 MaskScale = 0x6cc7cc;       // 1/128
const reg32 One = 0x6cc7bc;
const reg32 Zero = 0x6cc7a4;

// CompositeSkin32 data: four tables of 16 pixel indices with their counts, built once from the first skin's key
// colours.
const reg32 KeyPixels = 0x733e28;
const reg32 KeyTablesBuilt = 0x733f4c;
const reg32 KeyCounts = 0x733f50;
const reg32 KeyCountsEnd = 0x733f60;

const reg32 JdlzMagic = 0x5a4c444a;

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("load_native", true), pf::Config::getBool("shadow_load", false) };
        pf::logInfo("load: jdlz, texture creation, skin compositor %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// Guest stores. Shadow mode records the old values to undo them, except in the locked texture level, whose contents
// before the unlock are kept in texture.
struct Writer
{
    struct Undo
    {
        reg32 address;
        reg32 value;
        reg32 size;
    };

    bool record = false;
    std::vector<Undo> undo;
    reg32 lockedBegin = 0, lockedEnd = 0;
    reg32 lockSlot = 0;
    std::vector<reg8> texture;

    [[gnu::noinline, gnu::cold]] void log(reg32 address, reg32 size)
    {
        if (address - lockedBegin < lockedEnd - lockedBegin)
            return;
        undo.push_back(Undo{address, size == 4 ? rc::ld32(address) : size == 2 ? rc::ld16(address)
                                                                                : rc::ld8(address), size});
    }
    void remember(reg32 address, reg32 size)
    {
        if (__builtin_expect(record, 0))
            log(address, size);
    }
    // The level at bits, its address stored at slot by the lock.
    void locked(reg32 slot, reg32 bits, reg32 size)
    {
        if (record && size <= 0x1000000)
        {
            lockSlot = slot;
            lockedBegin = bits;
            lockedEnd = bits + size;
        }
    }
    void unlocking()
    {
        if (record && lockedEnd != lockedBegin)
            texture.assign(rc::host(lockedBegin), rc::host(lockedEnd));
    }
    void st32(reg32 address, reg32 value) { remember(address, 4); rc::st32(address, value); }
    void st8(reg32 address, reg8 value) { remember(address, 1); rc::st8(address, value); }
    void stf32(reg32 address, float value)
    {
        reg32 bits;
        memcpy(&bits, &value, 4);
        st32(address, bits);
    }
};

inline double ld(reg32 address) { return double(rc::ldf32(address)); }

// ---- JDLZ decompressor (0x58d190) ----

// out[i] = out[i - distance] for i in [0, length), in order.
inline void copyMatch(reg32 out, reg32 distance, reg32 length)
{
    if (__builtin_expect(distance <= out && reg32(out + length) > out, 1))
    {
        reg8* d = rc::host(out);
        const reg8* s = d - distance;
        if (distance >= length)
        {
            memcpy(d, s, length);
            return;
        }
        if (distance == 1)
        {
            memset(d, *s, length);
            return;
        }
        memcpy(d, s, distance);
        for (reg32 n = distance; n < length;)
        {
            const reg32 c = std::min(n, length - n);
            memcpy(d + n, d, c);
            n += c;
        }
        return;
    }
    for (reg32 i = 0; i < length; ++i)
        rc::st8(out + i, rc::ld8(out - distance + i));
}

// edx: the packed file ("JDLZ", version byte 2 at +4, unpacked size at +8, packed size at +0xc, the first flag
// bytes at +0x10 and +0x11, the stream at +0x12), eax: the output. Flag bits of the first stream pick a literal
// (0) or a match (1), those of the second the match form: 1 = 12-bit length and 4-bit distance, 0 = 5-bit length
// and 11-bit distance. Returns the end of the output.
reg32 jdlzNative(x86::CPU& cpu)
{
    const reg32 ret = cpu.esp;
    const reg32 header = cpu.edx;
    const reg32 magic = rc::ld32(header);
    cpu.ecx = magic;
    rc::st32(ret - 0x14, cpu.edi);
    // add esp, 0x10 before the ret.
    const reg32 frame = ret - 0x10;
    cpu.flags.cf = reg32(frame + 0x10) < frame;
    cpu.flags.zf = reg32(frame + 0x10) == 0;
    cpu.esp = ret + 4;
    if (magic != JdlzMagic || rc::ld8(header + 4) != 2)
    {
        cpu.flags.pf = 1;
        return cpu.eax;
    }
    const reg32 dst = cpu.eax;
    const reg32 ebxHigh = cpu.ebx & 0xffff0000u;
    reg32 flags1 = ebxHigh | rc::ld8(header + 0x10) | 0x100;
    reg32 flags2 = ebxHigh | rc::ld8(header + 0x11) | 0x100;
    const reg32 end = rc::ld32(header + 8) + dst;
    rc::st32(ret - 0x18, cpu.ebx);
    rc::st32(ret - 0x1c, cpu.esi);
    rc::st32(ret - 4, header + 8);
    rc::st32(ret - 8, end);
    cpu.ecx = header + 8;
    bool pf = !x86::parity8(reg8(flags2));
    reg32 left = rc::ld32(header + 0xc) - 0x12;
    reg32 src = header + 0x12;
    reg32 out = dst;
    if (left != 0)
    {
        rc::st32(ret - 0x20, cpu.ebp);
        // The upper halves of eax and ecx, stored with the flag bytes they reload.
        reg32 eaxHigh = header & 0xffff0000u;
        reg32 ecxHigh = (header + 8) & 0xffff0000u;
        do
        {
            if (flags1 & 1)
            {
                const reg32 b0 = rc::ld8(src);
                const reg32 b1 = rc::ld8(src + 1);
                reg32 length, distance;
                if (flags2 & 1)
                {
                    length = (((b0 & 0xf0) << 4) | b1) + 3;
                    distance = (b0 & 0xf) + 1;
                }
                else
                {
                    distance = (((b0 & 0xe0) << 3) | b1) + 0x11;
                    length = (b0 & 0x1f) + 3;
                }
                copyMatch(out, distance, length);
                eaxHigh = 0;
                ecxHigh = 0;
                flags2 = (flags2 & 0xffff0000u) | reg16(sreg16(reg16(flags2)) >> 1);
                src += 2;
                left -= 2;
                out += length;
            }
            else
            {
                if (out < end)
                {
                    rc::st8(out, rc::ld8(src));
                    ++out;
                    ++src;
                }
                --left;
            }
            const reg16 shifted = reg16(sreg16(reg16(flags1)) >> 1);
            flags1 = (flags1 & 0xffff0000u) | shifted;
            if (shifted == 1)
            {
                flags1 = ecxHigh | rc::ld8(src) | 0x100;
                ++src;
                --left;
            }
            if (reg16(flags2) == 1)
            {
                flags2 = eaxHigh | rc::ld8(src) | 0x100;
                ++src;
                --left;
            }
        }
        while (left != 0);
        pf = true;
    }
    rc::st32(ret - 0xc, flags1);
    rc::st32(ret - 0x10, flags2);
    cpu.edx = src;
    cpu.flags.pf = pf;
    return out;
}

// ---- weighted pixel sum (0x5a8640) ----

// _ftol2 of x: the 64-bit integer toward zero, the integer indefinite for NaN and out of range.
inline sreg64 truncate(double x)
{
    if (__builtin_expect(std::fabs(x) < 2147483648.0, 1))
        return sreg32(x);
    return x86::fistInteger<sreg64>(std::trunc(x));
}

// (pixels, weights, n, alpha max): each pixel's B, G, R bytes times its float weight, truncated, summed when the
// weight is above 1/255; alpha the same, or the largest low byte of its products when alpha max is set. The sums
// saturate at 255 (a negative sum keeps its low byte) into the returned B, G, R, A dword.
void pixelSumNative(x86::CPU& cpu, Writer& w)
{
    const reg32 ret = cpu.esp;
    const reg32 frame = ret - 0x28;
    const reg32 pixels = rc::ld32(ret + 4);
    const reg32 weights = rc::ld32(ret + 8);
    const sreg32 count = sreg32(rc::ld32(ret + 0xc));
    const bool alphaMax = rc::ld8(ret + 0x10) != 0;
    const double threshold = ld(ByteScale);
    w.st32(ret - 0x1c, cpu.ebx);
    w.st32(ret - 0x20, cpu.ebp);
    w.st32(ret - 0x24, cpu.esi);
    w.st32(ret - 0x28, cpu.edi);
    reg32 sumR = 0, sumG = 0, sumB = 0, alpha = 0;
    reg32 edx = cpu.edx;
    bool compared = false, product = false;
    double lastWeight = 0, lastProduct = 0;
    // The int temporaries of the fild at frame + 0x24 (groups of four) and frame + 0x2c (the rest).
    bool groupTemp = false, tailTemp = false;
    reg32 groupValue = 0, tailValue = 0;
    const sreg32 groups = count >= 4 ? count / 4 : 0;
    for (sreg32 i = 0; i < count; ++i)
    {
        if (i == groups * 4)
            edx = pixels;
        const double weight = ld(weights + reg32(i) * 4);
        compared = true;
        lastWeight = weight;
        if (!(weight > threshold))
            continue;
        const reg32 p = pixels + reg32(i) * 4;
        sumR += reg32(truncate(double(sreg32(rc::ld8(p + 2))) * weight));
        sumG += reg32(truncate(double(sreg32(rc::ld8(p + 1))) * weight));
        sumB += reg32(truncate(double(sreg32(rc::ld8(p))) * weight));
        const reg32 a = rc::ld8(p + 3);
        lastProduct = double(sreg32(a)) * weight;
        product = true;
        const sreg64 t = truncate(lastProduct);
        edx = reg32(x86::reg64(t) >> 32);
        if (alphaMax)
        {
            const reg32 low = reg32(t) & 0xff;
            if (sreg32(low) > sreg32(alpha))
                alpha = low;
        }
        else
        {
            alpha += reg32(t);
        }
        if (i < groups * 4)
        {
            groupTemp = true;
            groupValue = a;
        }
        else
        {
            tailTemp = true;
            tailValue = a;
        }
    }
    const auto clamp = [](reg32 v) { return sreg32(v) > 0xff ? reg32(0xff) : v & 0xff; };
    const reg32 packed = clamp(sumB) | clamp(sumG) << 8 | clamp(sumR) << 16 | clamp(alpha) << 24;
    w.st32(frame + 0x10, sumR);
    w.st32(frame + 0x14, sumG);
    w.st32(frame + 0x18, sumB);
    w.st32(frame + 0x1c, packed);
    if (groups)
        w.st32(frame + 0x20, reg32(3 + 4 * groups));
    if (groupTemp)
        w.st32(frame + 0x24, groupValue);
    if (tailTemp)
        w.st32(frame + 0x2c, tailValue);
    cpu.eax = packed;
    cpu.ecx = sumB;
    cpu.edx = edx;
    cpu.esp = ret + 4;
    if (compared)
        cpu.fpu.compare(lastWeight, threshold);
    if (product)
        cpu.fpu.regs[(cpu.fpu.count + 1) & 7] = lastProduct;
}

// ---- CompositeSkin32 (0x5a8a50) ----

// The x86 registers of the translated code.
struct Registers
{
    reg32 eax, ecx, edx, ebx, esp, ebp, esi, edi;

    void load(const x86::CPU& cpu)
    {
        eax = cpu.eax;
        ecx = cpu.ecx;
        edx = cpu.edx;
        ebx = cpu.ebx;
        esp = cpu.esp;
        ebp = cpu.ebp;
        esi = cpu.esi;
        edi = cpu.edi;
    }
    void store(x86::CPU& cpu) const
    {
        cpu.eax = eax;
        cpu.ecx = ecx;
        cpu.edx = edx;
        cpu.ebx = ebx;
        cpu.esp = esp;
        cpu.ebp = ebp;
        cpu.esi = esi;
        cpu.edi = edi;
    }
};

// The dword at q with bytes 0 and 2 swapped; edx, eax and ecx as the guest leaves them (new value, byte 1,
// byte 2).
inline void swapRedBlue(reg32 q, Registers& r, Writer& w)
{
    const reg32 b0 = rc::ld8(q), b1 = rc::ld8(q + 1), b2 = rc::ld8(q + 2), b3 = rc::ld8(q + 3);
    r.edx = b3 << 24 | b0 << 16 | b1 << 8 | b2;
    r.eax = b1;
    r.ecx = b2;
    w.st32(q, r.edx);
}

// Calls the pixel sum with the four arguments the guest pushes below esp; eax and ecx come back.
inline void callPixelSum(x86::CPU& cpu, Registers& r, Writer& w, reg32 pixels, reg32 weights, reg32 count,
                         reg32 alphaMax)
{
    w.st32(r.esp - 4, alphaMax);
    w.st32(r.esp - 8, count);
    w.st32(r.esp - 0xc, weights);
    w.st32(r.esp - 0x10, pixels);
    r.store(cpu);
    cpu.esp = r.esp - 0x14;
    pixelSumNative(cpu, w);
    r.eax = cpu.eax;
    r.ecx = cpu.ecx;
}

// Skin parameters P (the argument): +0 the texture, +4 the base colour, +8 four colours written into the key
// colour pixels, +0x18 layers of 0x34 bytes (+0 enabled, +0xc layer pixels, +0x14 mask bytes (dword stride),
// +0x20 tinted, +0x24 three tint colours), +0x1b8 the layer count. Returns 0 when the texture is not 32-bit.
// Returns false when the guest asked to terminate.
bool compositeNative(win32::WinApplication* app, x86::CPU& cpu, Writer& w)
{
    const reg32 depth = cpu.fpu.count;
    Registers r;
    r.load(cpu);
    const reg32 ret = r.esp;
    w.st32(ret - 0x48, r.ebx);
    w.st32(ret - 0x4c, r.ebp);
    r.ebp = rc::ld32(ret + 4);
    r.ebx = rc::ld32(r.ebp + 4);
    r.eax = rc::ld32(r.ebp + 0x1b8);
    w.st32(ret - 0x50, r.esi);
    w.st32(ret - 0x54, r.edi);
    r.esp = ret - 0x54;
    r.edi = rc::ld32(r.ebp);
    r.esi = r.ebp + 8;
    reg32 s = r.esp;
    w.st32(s + 0x10, r.edi);
    w.st32(s + 0x1c, r.ebx);
    w.st32(s + 0x38, r.esi);
    w.st32(s + 0x2c, r.eax);
    if (r.edi == 0 || rc::ld8(r.edi + 0x4a) != 0x20)
    {
        cpu.eax = 0;
        cpu.esp = ret + 4;
        return true;
    }

    // LockRect(texture, 0, &rect at s + 0x3c, NULL, 0).
    r.eax = rc::ld32(rc::ld32(r.edi) + 0x18);
    r.ecx = rc::ld32(r.eax);
    r.edx = s + 0x3c;
    w.st32(s - 4, 0);
    w.st32(s - 8, 0);
    w.st32(s - 0xc, r.edx);
    w.st32(s - 0x10, 0);
    w.st32(s - 0x14, r.eax);
    r.store(cpu);
    cpu.esp = s - 0x18;
    cpu.flags.cf = false;
    cpu.flags.pf = !x86::parity8(reg8(r.edi));
    cpu.flags.zf = true;
    cpu.ip = rc::ld32(r.ecx + 0x4c);
    app->dynamic_call(cpu.ip, cpu);
    if (cpu.terminate)
        return false;
    r.ebx = cpu.ebx;
    r.esp = cpu.esp;
    r.ebp = cpu.ebp;
    r.esi = cpu.esi;
    r.edi = cpu.edi;
    RC_FPU_CALL(cpu, depth, 0x5a8aa1u);
    s = r.esp;

    const reg32 width = reg32(sreg32(sreg16(rc::ld16(r.edi + 0x44))));
    const reg32 height = reg32(sreg32(sreg16(rc::ld16(r.edi + 0x46))));
    const reg32 bits = rc::ld32(s + 0x40);
    w.locked(s + 0x40, bits, height * width * 4);
    r.edx = bits;
    w.st32(s + 0x28, width);
    w.st32(s + 0x30, bits);
    w.st32(s + 0x24, height);
    if (rc::ld32(KeyTablesBuilt) == 0)
    {
        for (reg32 i = 0; i < 0x40; ++i)
            w.st32(KeyPixels + i * 4, 0);
        const reg32 end = bits + height * width * 4;
        static const reg32 keys[4] = {0xbfff0000, 0xbf00ff00, 0xbf0000ff, 0xbfff00ff};
        for (reg32 i = 0; i < 4; ++i)
            w.st32(s + 0x44 + i * 4, keys[i]);
        w.st32(s + 0x34, end);
        r.eax = height * width;
        r.ebx = end;
        r.ecx = bits;
        if (bits < end)
        {
            reg32 offset = 0;
            do
            {
                r.edi = rc::ld32(r.ecx);
                r.eax = 0;
                while (r.eax < 4 && r.edi != keys[r.eax])
                    ++r.eax;
                if (r.eax < 4)
                {
                    const reg32 n = rc::ld32(KeyCounts + r.eax * 4);
                    w.st32(KeyPixels + ((r.eax << 4) + n) * 4, reg32(sreg32(offset) >> 2));
                    r.ebp = rc::ld32(s + 0x58);
                    r.edi = n + 1;
                    w.st32(KeyCounts + r.eax * 4, r.edi);
                }
                r.ecx += 4;
                offset += 4;
                r.edi = offset;
            }
            while (r.ecx < end);
            w.st32(s + 0x14, offset);
        }
        r.ebx = rc::ld32(s + 0x1c);
        r.edi = rc::ld32(s + 0x10);
        w.st32(KeyTablesBuilt, 1);
    }

    // The base colour and the four key colours with red and blue swapped; the texture filled with the base colour.
    {
        const reg32 base = r.ebx;
        const reg32 swapped = (base & 0xff00ff00u) | (base >> 16 & 0xff) | (base & 0xff) << 16;
        w.st32(s + 0x58, swapped);
        for (reg32 q = r.esi; q != r.esi + 0x10; q += 4)
        {
            const reg32 b0 = rc::ld8(q), b1 = rc::ld8(q + 1), b2 = rc::ld8(q + 2), b3 = rc::ld8(q + 3);
            r.ecx = b3 << 24 | b0 << 16 | b1 << 8 | b2;
            w.st32(q, r.ecx);
        }
        r.eax = rc::ld32(s + 0x24) * rc::ld32(s + 0x28);
        const reg32 count = r.eax;
        const reg32 end = r.edx + count * 4;
        w.st32(s + 0x34, count);
        w.st32(s + 0x28, end);
        r.ecx = end;
        if (bits < end)
        {
            r.eax = swapped;
            const reg32 n = ((end - bits - 1) >> 2) + 1;
            if (!w.record && bits + n * 4 > bits)
            {
                reg8* d = rc::host(bits);
                for (reg32 i = 0; i < n; ++i)
                    memcpy(d + i * 4, &swapped, 4);
            }
            else
            {
                for (reg32 i = 0; i < n; ++i)
                    w.st32(bits + i * 4, swapped);
            }
            r.ecx = 0;
            r.edi = rc::ld32(s + 0x10);
        }
    }

    reg32 layers = rc::ld32(s + 0x2c);
    if (sreg32(layers) > 0)
    {
        const double byteScale = ld(ByteScale);
        const double maskScale = ld(MaskScale);
        const double one = ld(One);
        const double zero = ld(Zero);
        r.eax = layers;
        r.ecx = layers;
        r.edi = r.ebp + 0x3d;
        do
        {
            if (rc::ld32(r.edi - 0x25) != 0)
            {
                r.eax = rc::ld32(r.edi - 5);
                if (r.eax != 0)
                {
                    for (reg32 q = r.edi - 1; q != r.edi + 0xf; q += 4)
                        swapRedBlue(q, r, w);
                }
                r.ebx = rc::ld32(r.edi - 0x19);
                r.edx = rc::ld32(s + 0x34);
                r.ecx = rc::ld32(r.edi - 0x11);
                r.ebp = rc::ld32(s + 0x30);
                r.eax = r.ebx + r.edx * 4;
                const reg32 end = r.eax;
                w.st32(s + 0x24, end);
                reg32 mask = r.ecx;
                if (r.ebx < end)
                {
                    reg32 temp = 0;
                    do
                    {
                        r.esi = rc::ld32(mask) & 0xff;
                        r.eax = rc::ld32(r.ebx);
                        temp = r.eax;
                        r.ecx = rc::ld32(r.edi - 5);
                        r.edx = mask;
                        if (r.ecx != 0 && r.esi != 0)
                        {
                            const reg32 c = r.eax;
                            const reg32 red = c >> 16 & 0xff, green = c >> 8 & 0xff, blue = c & 0xff;
                            w.stf32(s + 0x44, float(double(sreg32(red)) * byteScale));
                            w.stf32(s + 0x48, float(double(sreg32(green)) * byteScale));
                            w.st32(s + 0x50, 0);
                            w.stf32(s + 0x4c, float(double(sreg32(blue)) * byteScale));
                            temp = blue;
                            r.eax = r.edi - 1;
                            r.ecx = blue;
                            r.edx = s + 0x44;
                            callPixelSum(cpu, r, w, r.edi - 1, s + 0x44, 3, 1);
                        }
                        if (r.esi > 0x7f)
                        {
                            w.st32(r.ebp, r.eax);
                        }
                        else if (r.esi != 0)
                        {
                            w.st32(s + 0x14, r.eax);
                            r.eax = rc::ld32(r.ebp);
                            temp = r.esi;
                            w.st32(s + 0x18, r.eax);
                            const double a = double(sreg32(r.esi)) * maskScale;
                            w.stf32(s + 0x1c, float(a));
                            const float rest = float(one - a);
                            w.stf32(s + 0x20, rest);
                            cpu.fpu.compare(a, one);
                            if (a > one)
                                w.st32(s + 0x1c, 0x3f800000);
                            cpu.fpu.compare(double(rest), zero);
                            if (double(rest) < zero)
                                w.st32(s + 0x20, 0);
                            r.eax = (r.eax & 0xffff0000u) | cpu.fpu.status.word;
                            r.ecx = s + 0x1c;
                            r.edx = s + 0x14;
                            callPixelSum(cpu, r, w, s + 0x14, s + 0x1c, 2, 0);
                            w.st32(r.ebp, r.eax);
                        }
                        r.ebx += 4;
                        mask += 4;
                        r.edx = mask;
                        r.ebp += 4;
                        r.eax = end;
                    }
                    while (r.ebx < end);
                    w.st32(s + 0x28, temp);
                    r.esi = rc::ld32(s + 0x38);
                }
                w.st32(s + 0x58, mask);
            }
            r.eax = rc::ld32(s + 0x2c) - 1;
            r.edi += 0x34;
            w.st32(s + 0x2c, r.eax);
        }
        while (r.eax != 0);
        r.edi = rc::ld32(s + 0x10);
    }

    // The four colours into the key colour pixels.
    r.edx = KeyPixels;
    r.esi -= KeyCounts;
    reg32 tested = 0;
    for (r.eax = KeyCounts; sreg32(r.eax) < sreg32(KeyCountsEnd); r.eax += 4)
    {
        r.ebx = rc::ld32(r.eax);
        tested = r.ebx;
        r.ecx = 0;
        if (sreg32(r.ebx) > 0)
        {
            do
            {
                const reg32 index = rc::ld32(r.edx);
                r.ebx = rc::ld32(r.esi + r.eax);
                r.ebp = rc::ld32(s + 0x30);
                w.st32(r.ebp + index * 4, r.ebx);
                r.edi = rc::ld32(r.eax);
                ++r.ecx;
                r.edx += 4;
            }
            while (sreg32(r.ecx) < sreg32(r.edi));
            r.edi = rc::ld32(s + 0x10);
        }
        r.edx = KeyPixels + (r.eax - KeyCounts + 4) * 0x10;
    }
    w.st32(s + 0x58, r.edx);

    // UnlockRect(texture, 0).
    w.unlocking();
    r.eax = rc::ld32(rc::ld32(r.edi) + 0x18);
    r.ecx = rc::ld32(r.eax);
    w.st32(s - 4, 0);
    w.st32(s - 8, r.eax);
    r.store(cpu);
    cpu.esp = s - 0xc;
    cpu.flags.cf = false;
    cpu.flags.pf = !x86::parity8(reg8(tested));
    cpu.flags.zf = true;
    cpu.ip = rc::ld32(r.ecx + 0x50);
    app->dynamic_call(cpu.ip, cpu);
    if (cpu.terminate)
        return false;
    RC_FPU_CALL(cpu, depth, 0x5a8e7au);
    const reg32 esp = cpu.esp;
    cpu.eax = 1;
    cpu.ebx = rc::ld32(esp + 0xc);
    cpu.esp = esp + 0x10 + 0x44 + 4;
    return true;
}

// ---- texture creation (0x407db0) ----

const reg32 MipSkip = 0x7364f4;         // levels left out of every texture
const reg32 Device = 0x73636c;          // the D3D device
const reg32 TextureListTail = 0x78daa4; // the list of created textures, head node at 0x78daa0
const reg32 TextureListHead = 0x78daa0;
const reg32 FormatP8 = 0x29;
const reg32 FormatA8R8G8B8 = 0x15;

inline reg32 signed16(reg32 address) { return reg32(sreg32(sreg16(rc::ld16(address)))); }
inline reg32 signed8(reg32 address) { return reg32(sreg32(x86::sreg8(rc::ld8(address)))); }
inline reg32 sar(reg32 value, reg32 count)
{
    const reg32 n = count & 31;
    return n ? reg32(sreg32(value) >> n) : value;
}
inline void push(reg32& esp, reg32 value)
{
    esp -= 4;
    rc::st32(esp, value);
}

// n dwords from palette[byte at src + i] to dst + 4i.
void expandPalette(reg32 dst, reg32 src, reg32 palette, reg32 n)
{
    if (reg32(dst + n * 4) > dst && n < 0x40000000u && reg32(src + n) > src && reg32(palette + 0x400) > palette)
    {
        reg8* d = rc::host(dst);
        const reg8* s = rc::host(src);
        const reg8* p = rc::host(palette);
        for (reg32 i = 0; i < n; ++i)
            memcpy(d + i * 4, p + s[i] * 4, 4);
        return;
    }
    for (reg32 i = 0; i < n; ++i)
        rc::st32(dst + i * 4, rc::ld32(palette + rc::ld8(src + i) * 4));
}

// rep movs of n units of size bytes from si to es:di in the direction of DF.
void repMove(const x86::CPU& cpu, reg32& si, reg32& di, reg32 n, reg32 size)
{
    const reg32 bytes = n * size;
    if (!cpu.flags.df && !cpu.ees && n < 0x40000000u && reg32(si + bytes) >= si && reg32(di + bytes) >= di
        && (si + bytes <= di || di + bytes <= si))
    {
        memcpy(rc::host(di), rc::host(si), bytes);
        si += bytes;
        di += bytes;
        return;
    }
    const reg32 step = cpu.flags.df ? 0u - size : size;
    for (; n != 0; --n)
    {
        if (size == 4)
            rc::st32(cpu.ees + di, rc::ld32(si));
        else
            rc::st8(cpu.ees + di, rc::ld8(si));
        si += step;
        di += step;
    }
}

// The texture of a texture record T (eax): its D3D texture [[T] + 0x18] is released (0x408010) and created again
// with T's size (+0x44, +0x46, powers of two), level count (+0x4e) less the skipped levels, and the format of
// [T] + 0x14; an 8-bit palette texture (format 0x29: indices at +0x70, palette at +0x74, +0x38 pixels in all) is
// expanded into an A8R8G8B8 buffer first and marked 32-bit (+0x4a). Each level, +0x40 bytes for level 0, is copied
// into the locked texture; the record goes on the texture list.
bool textureNative(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 depth = cpu.fpu.count;
    reg32 eax = cpu.eax, ecx = cpu.ecx, edx = cpu.edx, ebx = cpu.ebx, esp = cpu.esp, ebp = cpu.ebp;
    reg32 esi = cpu.esi, edi = cpu.edi;
    bool fcf = false, fpf = false, fzf = false;
    reg32 heightTest = 0;
    esp -= 0x24;
    push(esp, ebx);
    push(esp, ebp);
    push(esp, esi);
    esi = eax;
    ebx = rc::ld32(esi);
    eax = rc::ld32(ebx + 0x18);
    ebp = ebx + 0x18;
    rc::st32(esp + 0x18, ebx);
    rc::st32(esp + 0x14, ebp);
    if (eax != 0)
    {
        ecx = esi;
        esp -= 4;
        cpu.ecx = ecx;
        cpu.ebx = ebx;
        cpu.esp = esp;
        cpu.ebp = ebp;
        cpu.esi = esi;
        sub_408010(app, cpu);
        if (cpu.terminate)
            return false;
        ecx = cpu.ecx;
        edx = cpu.edx;
        ebx = cpu.ebx;
        esp = cpu.esp;
        ebp = cpu.ebp;
        esi = cpu.esi;
        edi = cpu.edi;
    }
    eax = rc::ld32(esi + 0x70);
    if (eax == 0)
        goto popEsi;
    ecx = signed16(esi + 0x44);
    edx = rc::ld32(ebx + 0x14);
    push(esp, edi);
    edi = ecx - 1;
    if (ecx & edi)
        goto popEdi;
    ecx = signed16(esi + 0x46);
    edi = ecx - 1;
    heightTest = ecx & edi;
    if (heightTest)
        goto popEdi;
    {
        const bool palette = edx == FormatP8;
        ecx = rc::ld32(esi + 0x40);
        rc::st32(esp + 0x28, 0);
        rc::st32(esp + 0x10, ecx);
        rc::st32(esp + 0x20, eax);
        if (palette)
        {
            edx = rc::ld32(esi + 0x74);
            ebp = eax;
            eax = rc::ld32(esi + 0x38);
            fcf = (eax >> 30) & 1;
            eax <<= 2;
            fzf = eax == 0;
            push(esp, eax);
            rc::st32(esp + 0x28, edx);
            esp -= 4;
            cpu.eax = eax;
            cpu.ecx = ecx;
            cpu.edx = edx;
            cpu.ebx = ebx;
            cpu.esp = esp;
            cpu.ebp = ebp;
            cpu.esi = esi;
            cpu.edi = edi;
            cpu.flags.cf = fcf;
            cpu.flags.pf = !x86::parity8(reg8(heightTest));
            cpu.flags.zf = fzf;
            sub_67121a(app, cpu);
            if (cpu.terminate)
                return false;
            eax = cpu.eax;
            ecx = cpu.ecx;
            ebx = cpu.ebx;
            esp = cpu.esp;
            ebp = cpu.ebp;
            esi = cpu.esi;
            rc::st32(esp + 0x24, eax);
            edi = eax;
            eax = (eax & 0xffffff00u) | rc::ld8(esi + 0x4e);
            esp += 4;
            rc::st32(esp + 0x28, 1);
            rc::st32(esp + 0x14, 0);
            if (x86::sreg8(reg8(eax)) > 0)
            {
                reg32 level = 0;
                do
                {
                    ebx = reg32(sreg32(sreg16(reg16(level))));
                    edx = signed16(esi + 0x44);
                    eax = signed16(esi + 0x46);
                    ecx = (ecx & 0xffffff00u) | (ebx & 0xff);
                    edx = sar(edx, ecx);
                    eax = sar(eax, ecx);
                    eax = reg32(sreg32(eax) * sreg32(edx));
                    if (sreg32(eax) > 0)
                    {
                        const reg32 colours = rc::ld32(esp + 0x24);
                        expandPalette(edi, ebp, colours, eax);
                        ecx = rc::ld32(colours + rc::ld8(ebp + eax - 1) * 4);
                        edx = colours;
                        edi += eax * 4;
                        ebp += eax;
                        eax = 0;
                    }
                    eax = level + 1;
                    edx = signed8(esi + 0x4e);
                    level = eax;
                    rc::st32(esp + 0x14, eax);
                }
                while (sreg32(eax) < sreg32(edx));
                ebx = rc::ld32(esp + 0x1c);
            }
            eax = rc::ld32(esp + 0x10);
            ebp = rc::ld32(esp + 0x18);
            ecx = eax * 4;
            edx = FormatA8R8G8B8;
            rc::st32(esp + 0x10, ecx);
            rc::st8(esi + 0x4a, 0x20);
        }
    }
    {
        // CreateTexture(device, width, height, levels, 0, format, D3DPOOL_MANAGED, [T] + 0x18, NULL).
        edi = rc::ld32(MipSkip);
        const reg32 skipTest = edi;
        fcf = false;
        fzf = edi == 0;
        bool skipped = false;
        if (edi != 0)
        {
            eax = signed8(esi + 0x4e);
            fcf = eax < edi;
            fzf = eax == edi;
            skipped = sreg32(eax) > sreg32(edi);
        }
        if (skipped)
        {
            push(esp, 0);
            push(esp, ebp);
            push(esp, 1);
            push(esp, edx);
            const reg32 levels = eax;
            eax -= edi;
            push(esp, 0);
            push(esp, eax);
            eax = signed16(esi + 0x46);
            edx = reg32(sreg32(eax) >> 31);
            ecx = edi + edi;
            {
                const rc::Division v = rc::divideSigned(edx, eax, ecx);
                eax = v.quotient;
                edx = v.remainder;
            }
            ebx = rc::ld32(rc::ld32(Device));
            push(esp, eax);
            eax = signed16(esi + 0x44);
            edx = reg32(sreg32(eax) >> 31);
            {
                const rc::Division v = rc::divideSigned(edx, eax, ecx);
                eax = v.quotient;
                edx = v.remainder;
            }
            edx = rc::ld32(Device);
            push(esp, eax);
            push(esp, edx);
            cpu.ip = rc::ld32(ebx + 0x5c);
            esp -= 4;
            cpu.eax = eax;
            cpu.ecx = ecx;
            cpu.edx = edx;
            cpu.ebx = ebx;
            cpu.esp = esp;
            cpu.ebp = ebp;
            cpu.esi = esi;
            cpu.edi = edi;
            cpu.flags.cf = levels < edi;
            cpu.flags.pf = !x86::parity8(reg8(skipTest));
            cpu.flags.zf = levels == edi;
            app->dynamic_call(cpu.ip, cpu);
            if (cpu.terminate)
                return false;
            ecx = cpu.ecx;
            edx = cpu.edx;
            esp = cpu.esp;
            ebp = cpu.ebp;
            esi = cpu.esi;
            RC_FPU_CALL(cpu, depth, 0x407ef4u);
            ebx = rc::ld32(esp + 0x1c);
        }
        else
        {
            eax = rc::ld32(Device);
            ecx = rc::ld32(eax);
            push(esp, 0);
            push(esp, ebp);
            push(esp, 1);
            push(esp, edx);
            edx = signed8(esi + 0x4e);
            push(esp, 0);
            push(esp, edx);
            edx = signed16(esi + 0x46);
            push(esp, edx);
            edx = signed16(esi + 0x44);
            push(esp, edx);
            push(esp, eax);
            cpu.ip = rc::ld32(ecx + 0x5c);
            esp -= 4;
            cpu.eax = eax;
            cpu.ecx = ecx;
            cpu.edx = edx;
            cpu.ebx = ebx;
            cpu.esp = esp;
            cpu.ebp = ebp;
            cpu.esi = esi;
            cpu.edi = edi;
            cpu.flags.cf = fcf;
            cpu.flags.pf = !x86::parity8(reg8(skipTest));
            cpu.flags.zf = fzf;
            app->dynamic_call(cpu.ip, cpu);
            if (cpu.terminate)
                return false;
            ecx = cpu.ecx;
            edx = cpu.edx;
            ebx = cpu.ebx;
            esp = cpu.esp;
            ebp = cpu.ebp;
            esi = cpu.esi;
            RC_FPU_CALL(cpu, depth, 0x407f1cu);
        }
    }
    if (rc::ld32(ebp) == 0)
        goto popEdi;
    eax = rc::ld32(TextureListTail);
    edi = rc::ld32(esp + 0x20);
    rc::st32(eax, ebx);
    rc::st32(TextureListTail, ebx);
    rc::st32(ebx + 4, eax);
    eax = rc::ld32(MipSkip);
    rc::st32(ebx, TextureListHead);
    esi = signed8(esi + 0x4e);
    rc::st32(esp + 0x14, edi);
    rc::st32(esp + 0x24, esi);
    if (eax != 0 && sreg32(esi) > sreg32(eax) && sreg32(eax) > 0)
    {
        // The skipped levels' data is passed over.
        ecx = eax;
        esi -= ecx;
        rc::st32(esp + 0x24, esi);
        do
        {
            eax = rc::ld32(esp + 0x10);
            edi += eax;
            edx = reg32(sreg32(eax) >> 31) & 3;
            eax = reg32(sreg32(eax + edx) >> 2);
            --ecx;
            rc::st32(esp + 0x10, eax);
        }
        while (ecx != 0);
        rc::st32(esp + 0x14, edi);
    }
    ebx = 0;
    fcf = false;
    fpf = !x86::parity8(reg8(esi));
    fzf = esi == 0;
    if (sreg32(esi) > 0)
    {
        rc::st32(esp + 0x1c, ebx);
        bool first = true;
        do
        {
            if (!first)
                edi = rc::ld32(esp + 0x14);
            first = false;
            // LockRect(texture, level, &rect at esp + 0x2c, NULL, 0).
            eax = rc::ld32(rc::ld32(esp + 0x18));
            ecx = rc::ld32(eax);
            push(esp, 0);
            push(esp, 0);
            edx = esp + 0x34;
            push(esp, edx);
            push(esp, ebx);
            push(esp, eax);
            cpu.ip = rc::ld32(ecx + 0x4c);
            esp -= 4;
            cpu.eax = eax;
            cpu.ecx = ecx;
            cpu.edx = edx;
            cpu.ebx = ebx;
            cpu.esp = esp;
            cpu.esi = esi;
            cpu.edi = edi;
            cpu.flags.cf = fcf;
            cpu.flags.pf = fpf;
            cpu.flags.zf = fzf;
            app->dynamic_call(cpu.ip, cpu);
            if (cpu.terminate)
                return false;
            ebx = cpu.ebx;
            esp = cpu.esp;
            edi = cpu.edi;
            RC_FPU_CALL(cpu, depth, 0x407fa5u);
            ecx = rc::ld32(esp + 0x1c);
            ebp = sar(rc::ld32(esp + 0x10), ecx);
            esi = edi;
            edi = rc::ld32(esp + 0x30);
            push(esp, ebx);
            ecx = ebp;
            eax = ecx;
            repMove(cpu, esi, edi, ecx >> 2, 4);
            ecx = eax & 3;
            const reg32 tail = ecx;
            repMove(cpu, esi, edi, ecx, 1);
            ecx = rc::ld32(esp + 0x1c);
            // UnlockRect(texture, level).
            eax = rc::ld32(ecx);
            edx = rc::ld32(eax);
            push(esp, eax);
            cpu.ip = rc::ld32(edx + 0x50);
            esp -= 4;
            cpu.eax = eax;
            cpu.ecx = ecx;
            cpu.edx = edx;
            cpu.esp = esp;
            cpu.ebp = ebp;
            cpu.esi = esi;
            cpu.edi = edi;
            cpu.flags.cf = false;
            cpu.flags.pf = !x86::parity8(reg8(tail));
            cpu.flags.zf = tail == 0;
            app->dynamic_call(cpu.ip, cpu);
            if (cpu.terminate)
                return false;
            edx = cpu.edx;
            ebx = cpu.ebx;
            esp = cpu.esp;
            ebp = cpu.ebp;
            edi = cpu.edi;
            fpf = cpu.flags.pf;
            RC_FPU_CALL(cpu, depth, 0x407fd2u);
            esi = rc::ld32(esp + 0x14) + ebp;
            ecx = rc::ld32(esp + 0x1c) + 2;
            eax = rc::ld32(esp + 0x24);
            ++ebx;
            fcf = ebx < eax;
            fzf = ebx == eax;
            rc::st32(esp + 0x14, esi);
            rc::st32(esp + 0x1c, ecx);
        }
        while (sreg32(ebx) < sreg32(eax));
    }
    eax = rc::ld32(esp + 0x28);
    if (eax != 0)
    {
        const reg32 allocated = eax;
        eax = rc::ld32(esp + 0x20);
        push(esp, eax);
        esp -= 4;
        cpu.eax = eax;
        cpu.ecx = ecx;
        cpu.edx = edx;
        cpu.ebx = ebx;
        cpu.esp = esp;
        cpu.esi = esi;
        cpu.edi = edi;
        cpu.flags.pf = !x86::parity8(reg8(allocated));
        sub_671102(app, cpu);
        if (cpu.terminate)
            return false;
        ecx = cpu.ecx;
        edx = cpu.edx;
        esp = cpu.esp + 4;
    }
popEdi:
    edi = rc::ld32(esp);
    esp += 4;
popEsi:
    esi = rc::ld32(esp);
    ebp = rc::ld32(esp + 4);
    ebx = rc::ld32(esp + 8);
    esp += 12 + 0x24 + 4;
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.ebx = ebx;
    cpu.esp = esp;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.edi = edi;
    return true;
}

// ---- shadow mode ----

const uint32_t ShadowLogLimit = 20;

enum Entry
{
    EntryJdlz,
    EntryComposite,
    EntryPixelSum,
    EntryTexture,
    EntryCount
};

const char* const EntryNames[EntryCount] = {"jdlz", "composite", "pixel sum", "texture"};

struct Shadow
{
    uint32_t calls[EntryCount] = {}, differing[EntryCount] = {};
    uint32_t relocated[EntryCount] = {};
    uint32_t uncompared = 0;
    uint32_t logged = 0;
    uint32_t reported = 0;
    bool started = false;
};

Shadow s_shadow;

// Values are equal, or the guest's object g stands where the native code's object n does.
struct Alias
{
    reg32 g = 0, n = 0;

    bool same(reg32 guest, reg32 native) const { return guest == native || (g && guest == g && native == n); }
};

bool compareRegisters(const x86::CPU& guest, const x86::CPU& native, char* text, size_t size,
                      const Alias& alias = Alias())
{
    const struct { const char* name; reg32 g, n; } regs[] = {
        {"eax", guest.eax, native.eax}, {"ecx", guest.ecx, native.ecx}, {"edx", guest.edx, native.edx},
        {"ebx", guest.ebx, native.ebx}, {"esp", guest.esp, native.esp}, {"ebp", guest.ebp, native.ebp},
        {"esi", guest.esi, native.esi}, {"edi", guest.edi, native.edi},
        {"eflags", guest.flags.eflags, native.flags.eflags}, {"x87 depth", guest.fpu.count, native.fpu.count},
        {"x87 status", guest.fpu.status.word, native.fpu.status.word},
    };
    for (const auto& r : regs)
    {
        if (!alias.same(r.g, r.n))
        {
            snprintf(text, size, "%s guest 0x%08x native 0x%08x", r.name, unsigned(r.g), unsigned(r.n));
            return false;
        }
    }
    for (int i = 0; i < 8; ++i)
    {
        const double g = guest.fpu.regs[i], n = native.fpu.regs[i];
        if (memcmp(&g, &n, sizeof(g)) != 0)
        {
            snprintf(text, size, "x87 register slot %d guest %.17g native %.17g", i, g, n);
            return false;
        }
    }
    return true;
}

void report()
{
    Shadow& s = s_shadow;
    const uint32_t now = pf::perf::now();
    if (s.started && now - s.reported < 2000000)
        return;
    s.started = true;
    s.reported = now;
    pf::logInfo("shadow_load: compared/differing jdlz %u/%u, composite %u/%u, pixel sum %u/%u, texture %u/%u; "
                "texture contents only (lock or buffer at another address): composite %u, texture %u; texture "
                "replacing a texture (not compared) %u",
                unsigned(s.calls[EntryJdlz]), unsigned(s.differing[EntryJdlz]),
                unsigned(s.calls[EntryComposite]), unsigned(s.differing[EntryComposite]),
                unsigned(s.calls[EntryPixelSum]), unsigned(s.differing[EntryPixelSum]),
                unsigned(s.calls[EntryTexture]), unsigned(s.differing[EntryTexture]),
                unsigned(s.relocated[EntryComposite]), unsigned(s.relocated[EntryTexture]), unsigned(s.uncompared));
}

void result(Entry entry, bool same, const char* text, reg32 caller)
{
    Shadow& s = s_shadow;
    ++s.calls[entry];
    if (!same)
    {
        ++s.differing[entry];
        if (s.logged < ShadowLogLimit)
        {
            ++s.logged;
            pf::logInfo("shadow_load: %s call %u (caller 0x%08x): %s", EntryNames[entry], unsigned(s.calls[entry]),
                        unsigned(caller), text);
        }
    }
    report();
}

struct Region
{
    reg32 address;
    reg32 size;
    std::vector<reg8> before;
    std::vector<bool> stored;
};

// Level 0 of a texture interface read through a read-only lock below the guest's stack; false when the lock fails.
bool readTexture(win32::WinApplication* app, x86::CPU& cpu, reg32 texture, reg32 level, reg32 size,
                 std::vector<reg8>& out)
{
    const reg32 ReadOnly = 0x10;
    const x86::CPU saved = cpu;
    const reg32 base = (cpu.esp - 0x200) & ~0xfu;
    const reg32 rect = base + 0x40;
    rc::st32(base + 4, texture);
    rc::st32(base + 8, level);
    rc::st32(base + 0xc, rect);
    rc::st32(base + 0x10, 0);
    rc::st32(base + 0x14, ReadOnly);
    cpu.esp = base;
    cpu.ip = rc::ld32(rc::ld32(texture) + 0x4c);
    app->dynamic_call(cpu.ip, cpu);
    bool ok = !cpu.terminate && cpu.eax == 0;
    if (ok)
    {
        const reg32 bits = rc::ld32(rect + 4);
        ok = win32::MemMap::isGuestRange(bits, size);
        if (ok)
            out.assign(rc::host(bits), rc::host(bits) + size);
        rc::st32(base + 4, texture);
        rc::st32(base + 8, level);
        cpu.esp = base;
        cpu.ip = rc::ld32(rc::ld32(texture) + 0x50);
        app->dynamic_call(cpu.ip, cpu);
    }
    const bool terminate = cpu.terminate;
    cpu = saved;
    cpu.terminate = terminate;
    return ok;
}

// Runs the native entry with its stores recorded and undone, then the guest code (whose result stays), and logs
// the first difference: registers, x87 state, every native store, guest stores the native code did not make in
// the given regions, then check.
template< typename Native, typename Regions, typename Check >
void shadowRun(Entry entry, win32::WinApplication* app, x86::CPU& cpu, Native native,
               void (*guest)(win32::WinApplication*, x86::CPU&), Regions regionsOf, Check check)
{
    const x86::CPU start = cpu;
    Writer w;
    w.record = true;
    if (!native(app, cpu, w))
        return;
    struct Store { reg32 address, size, value; };
    std::vector<Store> stores;
    stores.reserve(w.undo.size());
    for (const Writer::Undo& u : w.undo)
        stores.push_back(Store{u.address, u.size, u.size == 4 ? rc::ld32(u.address) : rc::ld8(u.address)});
    for (auto u = w.undo.rbegin(); u != w.undo.rend(); ++u)
    {
        if (u->size == 4)
            rc::st32(u->address, u->value);
        else
            rc::st8(u->address, reg8(u->value));
    }
    const x86::CPU after = cpu;

    std::vector<Region> regions;
    for (const auto& range : regionsOf(start, after))
    {
        if (range.second && win32::MemMap::isGuestRange(range.first, range.second))
            regions.push_back(Region{range.first, range.second, {}, {}});
    }
    for (Region& r : regions)
    {
        r.before.assign(rc::host(r.address), rc::host(r.address) + r.size);
        r.stored.assign(r.size, false);
    }

    cpu = start;
    guest(app, cpu);
    if (cpu.terminate)
        return;
    char text[200] = {};
    // A lock that gave the guest another address than the native code: only the texture is compared.
    const bool relocated = w.lockSlot && rc::ld32(w.lockSlot) != w.lockedBegin;
    if (relocated)
    {
        ++s_shadow.relocated[entry];
        stores.clear();
        regions.clear();
    }
    bool same = relocated || compareRegisters(cpu, after, text, sizeof(text));
    for (const Store& st : stores)
    {
        for (Region& r : regions)
        {
            for (reg32 i = 0; i < st.size; ++i)
            {
                if (st.address + i - r.address < r.size)
                    r.stored[st.address + i - r.address] = true;
            }
        }
        if (!same)
            continue;
        const reg32 g = st.size == 4 ? rc::ld32(st.address) : rc::ld8(st.address);
        if (g != st.value)
        {
            same = false;
            snprintf(text, sizeof(text), "guest 0x%08x native 0x%08x at 0x%08x", unsigned(g), unsigned(st.value),
                     unsigned(st.address));
        }
    }
    for (const Region& r : regions)
    {
        for (reg32 i = 0; same && i < r.size; ++i)
        {
            const reg8 now = rc::ld8(r.address + i);
            if (!r.stored[i] && now != r.before[i])
            {
                same = false;
                snprintf(text, sizeof(text), "guest stored 0x%02x at 0x%08x, native nothing", unsigned(now),
                         unsigned(r.address + i));
            }
        }
    }
    if (same)
        same = check(app, cpu, start, w, text, sizeof(text));
    if (cpu.terminate)
        return;
    result(entry, same, text, rc::ld32(start.esp));
}

bool noCheck(win32::WinApplication*, x86::CPU&, const x86::CPU&, const Writer&, char*, size_t)
{
    return true;
}

// CompositeSkin32 with parameters params: the texture level the guest wrote against the one the native code
// wrote.
bool checkSkinTexture(win32::WinApplication* app, x86::CPU& cpu, reg32 params, const Writer& w, char* text,
                      size_t size)
{
    const reg32 texture = rc::ld32(params);
    if (!texture || w.texture.empty())
        return true;
    std::vector<reg8> guest;
    if (!readTexture(app, cpu, rc::ld32(rc::ld32(texture) + 0x18), 0, reg32(w.texture.size()), guest))
    {
        snprintf(text, size, "texture not readable");
        return false;
    }
    for (size_t i = 0; i < guest.size(); i += 4)
    {
        reg32 g, n;
        memcpy(&g, &guest[i], 4);
        memcpy(&n, &w.texture[i], 4);
        if (g != n)
        {
            snprintf(text, size, "texture pixel %u guest 0x%08x native 0x%08x", unsigned(i / 4), unsigned(g),
                     unsigned(n));
            return false;
        }
    }
    return true;
}

// JDLZ: the native output and stack frame are kept, the memory restored, the guest run and the output range of
// either compared byte for byte.
void shadowJdlz(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::CPU start = cpu;
    const reg32 ret = cpu.esp;
    const reg32 dst = cpu.eax;
    const reg32 frame = ret - 0x20;
    const bool valid = rc::ld32(cpu.edx) == JdlzMagic && rc::ld8(cpu.edx + 4) == 2;
    const reg32 size = valid ? rc::ld32(cpu.edx + 8) : 0;
    // The guest's output may run past the unpacked size by the last match.
    reg32 span = size + 0x1004;
    if (!win32::MemMap::isGuestRange(dst, span))
        span = size;
    std::vector<reg8> before(rc::host(dst), rc::host(dst) + span);
    std::vector<reg8> frameBefore(rc::host(frame), rc::host(frame) + 0x20);
    const reg32 end = jdlzNative(cpu);
    const x86::CPU after = cpu;
    std::vector<reg8> nativeOut(rc::host(dst), rc::host(dst) + span);
    std::vector<reg8> nativeFrame(rc::host(frame), rc::host(frame) + 0x20);
    memcpy(rc::host(dst), before.data(), span);
    memcpy(rc::host(frame), frameBefore.data(), 0x20);

    cpu = start;
    sub_58d190_guest(app, cpu);
    if (cpu.terminate)
        return;
    char text[200] = {};
    bool same = compareRegisters(cpu, after, text, sizeof(text));
    if (same && end - dst > span)
    {
        same = false;
        snprintf(text, sizeof(text), "output 0x%08x..0x%08x beyond the compared 0x%x bytes", unsigned(dst),
                 unsigned(end), unsigned(span));
    }
    for (reg32 i = 0; same && i < span; ++i)
    {
        if (rc::ld8(dst + i) != nativeOut[i])
        {
            same = false;
            snprintf(text, sizeof(text), "guest 0x%02x native 0x%02x at 0x%08x (output +0x%x)",
                     unsigned(rc::ld8(dst + i)), unsigned(nativeOut[i]), unsigned(dst + i), unsigned(i));
        }
    }
    for (reg32 i = 0; same && i < 0x20; ++i)
    {
        if (rc::ld8(frame + i) != nativeFrame[i])
        {
            same = false;
            snprintf(text, sizeof(text), "guest 0x%02x native 0x%02x at 0x%08x (stack)",
                     unsigned(rc::ld8(frame + i)), unsigned(nativeFrame[i]), unsigned(frame + i));
        }
    }
    result(EntryJdlz, same, text, rc::ld32(start.esp));
}

// The bytes the texture creation of record t copies into each level, as the guest computes them.
std::vector<reg32> levelSizes(reg32 t)
{
    std::vector<reg32> sizes;
    reg32 size = rc::ld32(t + 0x40);
    if (rc::ld32(rc::ld32(t) + 0x14) == FormatP8)
        size *= 4;
    sreg32 levels = sreg32(signed8(t + 0x4e));
    const reg32 skip = rc::ld32(MipSkip);
    if (skip && levels > sreg32(skip) && sreg32(skip) > 0)
    {
        for (reg32 i = 0; i < skip; ++i)
            size = reg32(sreg32(size + (reg32(sreg32(size) >> 31) & 3)) >> 2);
        levels -= sreg32(skip);
    }
    for (sreg32 level = 0; level < levels && level < 16; ++level)
        sizes.push_back(sar(size, reg32(level) * 2));
    return sizes;
}

// Texture creation: the guest code runs first, its texture levels are read back and the memory it changed (stack
// frame, the record, the list tail, the 32-bit mark) is restored; then the native code runs and both are compared,
// the native texture standing for the guest's, which is released. A record that already has a texture (released
// on the way) runs the native code alone.
void shadowTexture(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 ret = cpu.esp;
    const reg32 t = cpu.eax;
    const reg32 record = rc::ld32(t);
    if (rc::ld32(record + 0x18) != 0)
    {
        ++s_shadow.uncompared;
        textureNative(app, cpu);
        report();
        return;
    }
    const std::vector<reg32> sizes = levelSizes(t);
    const x86::CPU start = cpu;
    const reg32 tail = rc::ld32(TextureListTail);
    struct Saved
    {
        reg32 address;
        reg32 size;
        std::vector<reg8> before, guest;
    };
    std::vector<Saved> saved;
    const std::pair<reg32, reg32> ranges[] = {{ret - 0x80, 0x84}, {record, 0x1c}, {TextureListTail, 4}, {tail, 4},
                                              {t + 0x48, 4}};
    for (const auto& r : ranges)
    {
        if (win32::MemMap::isGuestRange(r.first, r.second))
            saved.push_back(Saved{r.first, r.second, std::vector<reg8>(rc::host(r.first), rc::host(r.first) + r.second),
                                  {}});
    }

    sub_407db0_guest(app, cpu);
    if (cpu.terminate)
        return;
    const x86::CPU guest = cpu;
    for (Saved& s : saved)
        s.guest.assign(rc::host(s.address), rc::host(s.address) + s.size);
    const reg32 guestTexture = rc::ld32(record + 0x18);
    const reg32 guestBuffer = rc::ld32(ret - 0x14);
    const reg32 guestBits = rc::ld32(ret - 4);
    std::vector<std::vector<reg8>> guestLevels(sizes.size());
    bool readable = true;
    for (size_t level = 0; guestTexture && readable && level < sizes.size(); ++level)
        readable = readTexture(app, cpu, guestTexture, reg32(level), sizes[level], guestLevels[level]);
    if (cpu.terminate)
        return;
    for (const Saved& s : saved)
        memcpy(rc::host(s.address), s.before.data(), s.size);

    cpu = start;
    if (!textureNative(app, cpu))
        return;
    const reg32 nativeTexture = rc::ld32(record + 0x18);
    char text[200] = {};
    bool same = true;
    Alias alias;
    if (guestTexture && nativeTexture && guestTexture != nativeTexture)
        alias = Alias{guestTexture, nativeTexture};
    if (rc::ld32(ret - 0x14) != guestBuffer || rc::ld32(ret - 4) != guestBits)
    {
        ++s_shadow.relocated[EntryTexture];
    }
    else
    {
        same = compareRegisters(guest, cpu, text, sizeof(text), alias);
        for (const Saved& s : saved)
        {
            for (reg32 i = 0; same && i + 4 <= s.size; i += 4)
            {
                reg32 g, n;
                memcpy(&g, &s.guest[i], 4);
                n = rc::ld32(s.address + i);
                if (!alias.same(g, n))
                {
                    same = false;
                    snprintf(text, sizeof(text), "guest 0x%08x native 0x%08x at 0x%08x", unsigned(g), unsigned(n),
                             unsigned(s.address + i));
                }
            }
        }
    }
    if (same && !readable)
    {
        same = false;
        snprintf(text, sizeof(text), "guest texture not readable");
    }
    for (size_t level = 0; same && guestTexture && level < sizes.size(); ++level)
    {
        std::vector<reg8> native;
        if (!nativeTexture || !readTexture(app, cpu, nativeTexture, reg32(level), sizes[level], native))
        {
            same = false;
            snprintf(text, sizeof(text), "native texture level %u not readable", unsigned(level));
            break;
        }
        for (size_t i = 0; i < native.size(); ++i)
        {
            if (native[i] != guestLevels[level][i])
            {
                same = false;
                snprintf(text, sizeof(text), "level %u byte 0x%x guest 0x%02x native 0x%02x", unsigned(level),
                         unsigned(i), unsigned(guestLevels[level][i]), unsigned(native[i]));
                break;
            }
        }
    }
    if (cpu.terminate)
        return;
    if (alias.g)
    {
        // Release(guest texture).
        const x86::CPU kept = cpu;
        const reg32 base = (cpu.esp - 0x200) & ~0xfu;
        rc::st32(base + 4, guestTexture);
        cpu.esp = base;
        cpu.ip = rc::ld32(rc::ld32(guestTexture) + 8);
        app->dynamic_call(cpu.ip, cpu);
        const bool terminate = cpu.terminate;
        cpu = kept;
        cpu.terminate = terminate;
        if (terminate)
            return;
    }
    result(EntryTexture, same, text, rc::ld32(start.esp));
}

// ---- perf builds: time per second of game time ----

struct PerfSums
{
    uint32_t calls[EntryCount] = {};
    uint64_t us[EntryCount] = {};
    uint32_t start = 0;
    bool started = false;
};

PerfSums s_perf;
const uint32_t ReportUs = 5000000;

void perfReport(uint32_t now)
{
    PerfSums& p = s_perf;
    if (now - p.start < ReportUs)
        return;
    const Settings& s = settings();
    pf::logInfo("load: %s, jdlz %u calls %.2f ms, texture %u calls %.2f ms, composite %u calls %.2f ms in the last "
                "%.1f s", !s.native ? "guest" : s.shadow ? "shadow" : "native", unsigned(p.calls[EntryJdlz]),
                p.us[EntryJdlz] / 1000.0, unsigned(p.calls[EntryTexture]), p.us[EntryTexture] / 1000.0,
                unsigned(p.calls[EntryComposite]), p.us[EntryComposite] / 1000.0, (now - p.start) / 1000000.0);
    p = PerfSums();
    p.start = now;
    p.started = true;
}

template< typename Body >
void timed(Entry entry, Body body)
{
    if (!pf::perf::enabled())
    {
        body();
        return;
    }
    PerfSums& p = s_perf;
    const uint32_t start = pf::perf::now();
    body();
    const uint32_t end = pf::perf::now();
    ++p.calls[entry];
    p.us[entry] += end - start;
    if (!p.started)
    {
        p.start = start;
        p.started = true;
    }
    perfReport(end);
}

}

}

namespace native
{

using load::settings;

// JDLZ decompressor (guest 0x58d190).
void sub_58d190(win32::WinApplication* app, x86::CPU& cpu)
{
    load::timed(load::EntryJdlz, [&]
    {
        const load::Settings& s = settings();
        if (!s.native)
            sub_58d190_guest(app, cpu);
        else if (!s.shadow)
            load::jdlzNative(cpu);
        else
            load::shadowJdlz(app, cpu);
    });
}

// Weighted pixel sum (guest 0x5a8640).
void sub_5a8640(win32::WinApplication* app, x86::CPU& cpu)
{
    const load::Settings& s = settings();
    if (!s.native)
    {
        sub_5a8640_guest(app, cpu);
    }
    else if (!s.shadow)
    {
        load::Writer w;
        load::pixelSumNative(cpu, w);
    }
    else
    {
        const auto native = [](win32::WinApplication*, x86::CPU& c, load::Writer& w)
        {
            load::pixelSumNative(c, w);
            return true;
        };
        const auto regions = [](const x86::CPU& start, const x86::CPU&)
        {
            return std::vector<std::pair<x86::reg32, x86::reg32>>{{start.esp - 0x28, 0x3c}};
        };
        load::shadowRun(load::EntryPixelSum, app, cpu, native, sub_5a8640_guest, regions, load::noCheck);
    }
}

// CompositeSkin32 (guest 0x5a8a50).
void sub_5a8a50(win32::WinApplication* app, x86::CPU& cpu)
{
    load::timed(load::EntryComposite, [&]
    {
        const load::Settings& s = settings();
        if (!s.native || cpu.flags.df || cpu.ees)
        {
            sub_5a8a50_guest(app, cpu);
        }
        else if (!s.shadow)
        {
            load::Writer w;
            load::compositeNative(app, cpu, w);
        }
        else
        {
            const auto regions = [](const x86::CPU& start, const x86::CPU&)
            {
                using Range = std::pair<x86::reg32, x86::reg32>;
                std::vector<Range> r;
                const x86::reg32 ret = start.esp;
                r.push_back(Range{ret - 0xa0, 0xa8});
                const x86::reg32 p = rc::ld32(ret + 4);
                const x86::reg32 layers = rc::ld32(p + 0x1b8);
                r.push_back(Range{p, 0x1bc});
                if (x86::sreg32(layers) > 0 && layers < 0x100)
                    r.push_back(Range{p + 0x18, layers * 0x34});
                r.push_back(Range{load::KeyPixels, load::KeyCountsEnd - load::KeyPixels});
                return r;
            };
            const x86::reg32 params = rc::ld32(cpu.esp + 4);
            const auto check = [params](win32::WinApplication* a, x86::CPU& c, const x86::CPU&,
                                        const load::Writer& w, char* text, size_t size)
            {
                return load::checkSkinTexture(a, c, params, w, text, size);
            };
            load::shadowRun(load::EntryComposite, app, cpu, load::compositeNative, sub_5a8a50_guest, regions, check);
        }
    });
}

// Texture creation (guest 0x407db0).
void sub_407db0(win32::WinApplication* app, x86::CPU& cpu)
{
    load::timed(load::EntryTexture, [&]
    {
        const load::Settings& s = settings();
        if (!s.native)
            sub_407db0_guest(app, cpu);
        else if (!s.shadow)
            load::textureNative(app, cpu);
        else
            load::shadowTexture(app, cpu);
    });
}

}
}
