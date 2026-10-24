// EA SND 8 software mixer (docs: audio.md). The audio thread renders the voices (0x656D70) outside the execution
// lock, and the x87 kernels of the render run natively. Each kernel does the guest's loads, double operations and
// stores in the guest's order, so its output is bit-identical (only where two NaNs meet in one operation may the
// result carry the other NaN's payload: the compiler orders the operands, here as in the generated code).

#include <lib/mutex.h>
#include <lib/teb.h>
#include <lib/winapp.h>
#include <platform/perf.h>
#include <recomp.h>
#include <x86.h>

#include <cstring>

namespace nfsu
{

// Generated guest code.
void sub_656d70_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_65f930(win32::WinApplication* app, x86::CPU& cpu);

namespace native
{

namespace
{

using x86::reg32;
using x86::sreg32;

// Stack argument n (0 = the first) of a cdecl function.
inline reg32 argument(const x86::CPU& cpu, int n)
{
    return rc::ld32(cpu.esp + 4 + 4 * n);
}

inline double loadFloat(reg32 address)
{
    return double(rc::ldf32(address));
}

// ---- Render graph ---------------------------------------------------------------------------------------------
//
// The render 0x656D70 runs on the audio thread inside the pump, which holds the SND mutex (0x6E9B8C) throughout.
// It and its callees touch only SND state (voices, modules, decoders, stream blocks, the SND pool, the mix buses),
// sample data and the DirectSound buffer, and the game changes that state only through SND calls that take the
// same mutex. Its indirect calls go through the pointers checked below; when every one leads to a function of the
// audited set, the render runs without the execution lock, else it keeps the lock.

const reg32 VoiceArray      = 0x71666c;     // 0x60-byte voices: +0 state (2 = playing), +0x40 module chain
const reg32 VoiceCount      = 0x716464;     // byte
const reg32 ChannelCount    = 0x716465;     // byte
const reg32 VoiceSize       = 0x60;
const reg32 PlayingVoice    = 2;
const reg32 MixKernel       = 0x716670;
const reg32 ConvertKernel   = 0x716674;
const reg32 VoiceDone       = 0x716468;
const reg32 ReverbHook      = 0x716470;
const reg32 ReverbModules   = 0x717294;     // table: +4 + 4 * channel
const reg32 PostFilters     = 0x7165ac;     // per channel, optional
const reg32 PostOutputs     = 0x716594;     // per channel
const reg32 FreeHook        = 0x7172f8;
const reg32 MaxChannels     = 8;
const int   MaxDepth        = 16;
const int   MaxModules      = 4096;

// Module: +0 render, +4 teardown, +8 first input (also the voice chain link), +0xC second input.
enum Extra
{
    Plain,
    Decoder,        // EA-XA decoder object at +0x1C, its function at +0
    Resampler,      // kernel at +0x24
};

struct ModuleKind
{
    reg32   render;
    bool    secondInput;
    Extra   extra;
};

const ModuleKind s_modules[] = {
    {0x6591d0, false, Plain},       // decoded bank sample
    {0x659310, false, Plain},       // decoded looping sample
    {0x659460, false, Decoder},     // EA-XA sample
    {0x659550, false, Decoder},     // looping EA-XA sample
    {0x6597f0, false, Plain},       // PCM sample
    {0x659ee0, false, Plain},       // filter
    {0x65b2d0, false, Plain},       // stream-synchronised gate
    {0x65b3f0, false, Plain},       // float -> 16-bit output
    {0x65bd60, false, Plain},       // filter
    {0x65fd80, false, Plain},       // buffer source
    {0x65fdf0, false, Plain},       // hold
    {0x65ff10, true,  Plain},       // sum of two inputs
    {0x660160, false, Plain},       // delay
    {0x6602e0, false, Plain},       // gain
    {0x6605c0, false, Plain},       // filter
    {0x660760, false, Plain},       // filter
    {0x660870, false, Plain},       // decoded stream
    {0x660b00, false, Decoder},     // EA-XA stream
    {0x660ce0, false, Plain},       // PCM stream
    {0x660e20, false, Plain},       // looping PCM sample
    {0x660f10, false, Resampler},   // resampler
};

const reg32 s_teardowns[] = { 0, 0x659230, 0x660c40, 0x660030, 0x6601f0, 0x660a60 };
const reg32 EaxaDecoder = 0x65fad0;                     // the x87 one; 0x65F9D0 is SSE
const reg32 s_resamplerKernels[] = { 0x661f60, 0x661780 };  // x87 mono and stereo

const ModuleKind* moduleKind(reg32 render)
{
    for (const ModuleKind& kind : s_modules)
    {
        if (kind.render == render)
            return &kind;
    }
    return nullptr;
}

template <size_t N>
bool contains(const reg32 (&set)[N], reg32 value)
{
    for (reg32 entry : set)
    {
        if (entry == value)
            return true;
    }
    return false;
}

// The first failed check of an audit: what failed (a string literal) and the guest value it failed on.
struct Audit
{
    const char* reason = nullptr;
    reg32       address = 0;

    bool fail(const char* what, reg32 value)
    {
        reason = what;
        address = value;
        return false;
    }
};

bool knownModule(reg32 module, int depth, int& budget, Audit& audit)
{
    if (!module)
        return true;
    if (depth > MaxDepth)
        return audit.fail("depth", module);
    if (--budget < 0)
        return audit.fail("module count", module);
    const reg32 render = rc::ld32(module);
    const ModuleKind* kind = moduleKind(render);
    if (!kind)
        return audit.fail("module", render);
    const reg32 teardown = rc::ld32(module + 4);
    if (!contains(s_teardowns, teardown))
        return audit.fail("teardown", teardown);
    if (kind->extra == Decoder)
    {
        const reg32 decoder = rc::ld32(module + 0x1c);
        if (!decoder)
            return audit.fail("decoder", 0);
        if (rc::ld32(decoder) != EaxaDecoder)
            return audit.fail("decoder", rc::ld32(decoder));
    }
    else if (kind->extra == Resampler && !contains(s_resamplerKernels, rc::ld32(module + 0x24)))
    {
        return audit.fail("resampler", rc::ld32(module + 0x24));
    }
    return knownModule(rc::ld32(module + 8), depth + 1, budget, audit)
        && (!kind->secondInput || knownModule(rc::ld32(module + 0xc), depth + 1, budget, audit));
}

bool knownRoot(reg32 module, int& budget, Audit& audit)
{
    if (!module)
        return audit.fail("no output", 0);
    return knownModule(module, 0, budget, audit);
}

// With the execution lock and the SND mutex held.
bool renderAudited(Audit& audit)
{
    if (rc::ld32(MixKernel) != 0x65a880)
        return audit.fail("mix kernel", rc::ld32(MixKernel));
    if (rc::ld32(ConvertKernel) != 0x65a9a0)
        return audit.fail("convert kernel", rc::ld32(ConvertKernel));
    if (rc::ld32(VoiceDone) != 0x6525f0)
        return audit.fail("voice done", rc::ld32(VoiceDone));
    if (rc::ld32(FreeHook) != 0x656500)
        return audit.fail("free hook", rc::ld32(FreeHook));
    const reg32 channels = rc::ld8(ChannelCount);
    if (channels > MaxChannels)
        return audit.fail("channels", channels);
    int budget = MaxModules;
    const reg32 voices = rc::ld32(VoiceArray);
    const reg32 voiceCount = rc::ld8(VoiceCount);
    for (reg32 i = 0; i < voiceCount; ++i)
    {
        const reg32 voice = voices + i * VoiceSize;
        if (rc::ld8(voice) == PlayingVoice && !knownRoot(rc::ld32(voice + 0x40), budget, audit))
            return false;
    }
    for (reg32 channel = 0; channel < channels; ++channel)
    {
        const reg32 filter = rc::ld32(PostFilters + 4 * channel);
        if ((filter && !knownRoot(filter, budget, audit))
            || !knownRoot(rc::ld32(PostOutputs + 4 * channel), budget, audit))
            return false;
    }
    const reg32 hook = rc::ld32(ReverbHook);
    if (hook == 0x659fa0)
    {
        const reg32 table = rc::ld32(ReverbModules);
        if (!table)
            return audit.fail("reverb table", 0);
        for (reg32 channel = 0; channel < channels; ++channel)
        {
            if (!knownModule(rc::ld32(table + 4 + 4 * channel), 0, budget, audit))
                return false;
        }
    }
    else if (hook != 0)
    {
        return audit.fail("reverb hook", hook);
    }
    return true;
}

}

// Renders frames into the DirectSound buffer: (pointer to the output pointer, frames). The audio thread gives the
// execution lock up meanwhile, and no preemption point inside takes it back.
void sub_656d70(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 slot = cpu.efs + win32::teb::HostLock;
    win32::Mutex* const lock = app->getMemory<win32::Mutex*>(slot);
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    Audit audit;
    if (!lock || !renderAudited(audit))
    {
        sub_656d70_guest(app, cpu);
        if (timed)
        {
            pf::perf::audio(pf::perf::AudioRendersLocked, 1);
            pf::perf::audio(pf::perf::AudioRenderLockedUs, pf::perf::now() - start);
            pf::perf::renderLocked(lock ? audit.reason : "no lock", audit.address);
        }
        return;
    }
    app->unlockContext(cpu);
    app->getMemory<win32::Mutex*>(slot) = nullptr;
    sub_656d70_guest(app, cpu);
    app->getMemory<win32::Mutex*>(slot) = lock;
    if (timed)
    {
        pf::perf::audio(pf::perf::AudioRendersUnlocked, 1);
        pf::perf::audio(pf::perf::AudioRenderUnlockedUs, pf::perf::now() - start);
    }
    app->lockContext(cpu);
}

// ---- x87 kernels ----------------------------------------------------------------------------------------------

// dst[i] += src[i] * gain over n floats (n, gain, src, dst), from the top: single samples until n is a multiple of
// 4, then blocks of 4 that load everything before they store.
void sub_65a880(win32::WinApplication*, x86::CPU& cpu)
{
    reg32 n = argument(cpu, 0);
    float gainValue;
    const reg32 gainBits = argument(cpu, 1);
    memcpy(&gainValue, &gainBits, sizeof(gainValue));
    const double gain = double(gainValue);
    const reg32 src = argument(cpu, 2);
    const reg32 dst = argument(cpu, 3);
    cpu.esp += 4;
    while (n & 3)
    {
        double value = loadFloat(src + n * 4 - 4);
        value *= gain;
        value += loadFloat(dst + n * 4 - 4);
        rc::stf32(dst + n * 4 - 4, float(value));
        if (n == 1)
            return;
        --n;
    }
    for (;;)
    {
        const reg32 s = src + n * 4;
        const reg32 d = dst + n * 4;
        double v0 = loadFloat(s - 16);
        v0 *= gain;
        double v1 = loadFloat(s - 12);
        v1 *= gain;
        double v2 = loadFloat(s - 8);
        v2 *= gain;
        double v3 = loadFloat(s - 4);
        v3 *= gain;
        v0 += loadFloat(d - 16);
        v1 += loadFloat(d - 12);
        v2 += loadFloat(d - 8);
        v3 += loadFloat(d - 4);
        rc::stf32(d - 16, float(v0));
        rc::stf32(d - 12, float(v1));
        rc::stf32(d - 4, float(v3));
        rc::stf32(d - 8, float(v2));
        const reg32 previous = n;
        n -= 4;
        if (!(sreg32(previous) > 4))
            return;
    }
}

// Signed 16-bit to float (n, src, dst), from the top: single samples until n is a multiple of 8, then blocks of 8.
void sub_65a9a0(win32::WinApplication*, x86::CPU& cpu)
{
    reg32 n = argument(cpu, 0);
    const reg32 src = argument(cpu, 1);
    const reg32 dst = argument(cpu, 2);
    cpu.esp += 4;
    while (n & 7)
    {
        const double value = double(x86::sreg16(rc::ld16(src + n * 2 - 2)));
        rc::stf32(dst + n * 4 - 4, float(value));
        if (n == 1)
            return;
        --n;
    }
    for (;;)
    {
        const reg32 s = src + n * 2;
        const reg32 d = dst + n * 4;
        double v[8];
        for (int i = 0; i < 8; ++i)
            v[i] = double(x86::sreg16(rc::ld16(s - 16 + 2 * i)));
        rc::stf32(d - 32, float(v[0]));
        rc::stf32(d - 8, float(v[6]));
        rc::stf32(d - 12, float(v[5]));
        rc::stf32(d - 16, float(v[4]));
        rc::stf32(d - 20, float(v[3]));
        rc::stf32(d - 24, float(v[2]));
        rc::stf32(d - 28, float(v[1]));
        rc::stf32(d - 4, float(v[7]));
        const reg32 previous = n;
        n -= 8;
        if (!(sreg32(previous) > 8))
            return;
    }
}

// Linear interpolation resampler (n, src, dst, &position, &fraction, step, step fraction): the 32-bit fraction is
// the low half of the qword at 0x6EB120 that the guest loads with fild; scaled by the float at 0x6EB128 (2^-32).
// The qword is converted without the software int64 path while its high half is 0, which is exact.
void sub_661f60(win32::WinApplication*, x86::CPU& cpu)
{
    const reg32 FractionWord = 0x6eb120;
    const reg32 FractionScale = 0x6eb128;
    reg32 n = argument(cpu, 0);
    const reg32 src = argument(cpu, 1);
    reg32 dst = argument(cpu, 2);
    const reg32 positionAddress = argument(cpu, 3);
    const reg32 fractionAddress = argument(cpu, 4);
    const reg32 step = argument(cpu, 5);
    const reg32 stepFraction = argument(cpu, 6);
    cpu.esp += 4;
    reg32 fraction = rc::ld32(fractionAddress);
    reg32 position = rc::ld32(positionAddress);
    rc::st32(FractionWord, fraction);
    const reg32 high = rc::ld32(FractionWord + 4);
    const double scale = loadFloat(FractionScale);
    auto sample = [&]()
    {
        const reg32 at = src + position * 4;
        double value = loadFloat(at + 4);
        value -= loadFloat(at);
        const double weight = high == 0 ? double(fraction)
                                        : double(x86::sreg64((x86::reg64(high) << 32) | fraction));
        value *= weight;
        value *= scale;
        const reg32 next = fraction + stepFraction;
        const reg32 carry = next < fraction;
        fraction = next;
        value += loadFloat(at);
        position += step + carry;
        dst += 4;
        rc::st32(FractionWord, fraction);
        rc::stf32(dst - 4, float(value));
    };
    if (sreg32(n) >= 2)
    {
        n -= 2;
        for (;;)
        {
            sample();
            sample();
            const reg32 previous = n;
            n -= 2;
            if (!(sreg32(previous) >= 2))
                break;
        }
        n += 2;
    }
    while (n & 1)
    {
        sample();
        --n;
    }
    rc::st32(fractionAddress, fraction);
    rc::st32(positionAddress, position);
}

// EA-XA decoder (state): state +0 bytes left (28 per frame), +4/+8 the last two samples, +0xC input, +0x10 output
// (floats; the two before it hold the history). A frame is a header byte (coefficient pair, shift) and 14 bytes of
// nibbles; 0xEE starts a raw frame, decoded by 0x65F930.
void sub_65fad0(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 CoefficientA = 0x6eac28;
    const reg32 CoefficientB = 0x6eac18;
    const reg32 NibbleTable = 0x6eac38;     // 16 rows of 16 floats, by shift and nibble
    const reg32 RawFrame = 0xee;
    const reg32 state = argument(cpu, 0);
    // The guest's own pushes (10 registers, pushad, pushfd) lie between its return slot and the call's argument.
    const reg32 callSlot = cpu.esp - 40 - 32 - 4 - 8;
    reg32 out = rc::ld32(state + 0x10);
    rc::st32(out - 4, rc::ld32(state + 4));
    rc::st32(out - 8, rc::ld32(state + 8));
    reg32 in = rc::ld32(state + 0xc);
    while (sreg32(rc::ld32(state)) > 0)
    {
        rc::st32(state, rc::ld32(state) - 0x1c);
        if (rc::ld8(in) == RawFrame)
        {
            // pushad / pushfd around a cdecl call: every register comes back.
            const x86::CPU saved = cpu;
            rc::st32(callSlot + 4, state);
            cpu.esp = callSlot;
            sub_65f930(app, cpu);
            if (cpu.terminate)
                return;
            const x86::FPU fpu = cpu.fpu;
            cpu = saved;
            cpu.fpu = fpu;
            out = rc::ld32(state + 0x10);
            rc::st32(out - 4, rc::ld32(state + 4));
            rc::st32(out - 8, rc::ld32(state + 8));
            in = rc::ld32(state + 0xc);
            continue;
        }
        const reg32 header = rc::ld8(in);
        const double a = loadFloat((header >> 4) * 4 + CoefficientA);
        const double b = loadFloat((header >> 4) * 4 + CoefficientB);
        const reg32 row = (header & 0xf) << 6;
        reg32 nibble = rc::ld8(in + 1) >> 4;
        out += 0x70;
        in += 0xf;
        for (reg32 i = reg32(-14); i != 0; ++i)
        {
            const reg32 at = out + i * 8;
            double previous = loadFloat(at - 4);
            previous *= b;
            double older = loadFloat(at - 8);
            older *= a;
            previous += loadFloat(row + nibble * 4 + NibbleTable);
            older += previous;
            nibble = rc::ld8(in + i) & 0xf;
            rc::stf32(at, float(older));
            previous = loadFloat(at);
            previous *= b;
            older = loadFloat(at - 4);
            older *= a;
            previous += loadFloat(row + nibble * 4 + NibbleTable);
            older += previous;
            nibble = rc::ld8(in + i + 1) >> 4;
            rc::stf32(at + 4, float(older));
        }
        rc::st32(state + 4, rc::ld32(out - 4));
        rc::st32(state + 8, rc::ld32(out - 8));
        rc::st32(state + 0x10, out);
        rc::st32(state + 0xc, in);
    }
    cpu.esp += 4;
}

// Output module: float -> signed 16-bit (module, frames, float buffer, output, flags). Pulls its input first when
// it has one. Each sample goes through float(sample + 0x6AAB68's 1.5 * 2^23), stored back into the buffer, whose
// low 20 bits hold the integer; out of range saturates.
void sub_65b3f0(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 RoundingBias = 0x6aab68;
    const reg32 module = argument(cpu, 0);
    const reg32 frames = argument(cpu, 1);
    reg32 samples = argument(cpu, 2);
    reg32 out = argument(cpu, 3);
    const reg32 input = rc::ld32(module + 8);
    cpu.edx = module;
    if (input)
    {
        // The guest saves ebx, esi and edi, then pushes the five arguments and calls.
        const reg32 base = cpu.esp - 12 - 20;
        rc::st32(base, input);
        rc::st32(base + 4, frames);
        rc::st32(base + 8, out);
        rc::st32(base + 12, samples);
        rc::st32(base + 16, rc::ld8(module + 0x1a));
        const reg32 returnSlot = cpu.esp;
        const reg32 ebx = cpu.ebx, esi = cpu.esi, edi = cpu.edi;
        const reg32 depth = cpu.fpu.count;
        cpu.edx = rc::ld8(module + 0x1a);
        cpu.ecx = out;
        cpu.ebx = frames;
        cpu.esi = samples;
        cpu.edi = out;
        cpu.eax = input;
        cpu.esp = base - 4;
        cpu.ip = rc::ld32(input);
        app->dynamic_call(cpu.ip, cpu);
        if (cpu.terminate)
            return;
        RC_FPU_CALL(cpu, depth, 0x65b415u);
        // The guest's pops restore its saved registers.
        cpu.ebx = ebx;
        cpu.esi = esi;
        cpu.edi = edi;
        cpu.esp = returnSlot + 4;
        if (sreg32(cpu.eax) <= 0)
            return;
    }
    else
    {
        cpu.esp += 4;
    }
    const double bias = loadFloat(RoundingBias);
    const reg32 end = samples + frames * 4;
    cpu.ecx = end;
    reg32 high = 0;
    reg32 low = 0;
    for (; samples < end; samples += 4, out += 2)
    {
        double value = loadFloat(samples);
        value += bias;
        rc::stf32(samples, float(value));
        reg32 bits = rc::ld32(samples) & 0xfffff;
        if (bits > 0x7fff && bits < 0xf8000)
        {
            const bool positive = bits < 0x80000;
            high += positive;
            low += !positive;
            bits = positive ? 0x7fff : 0xffff8000;
        }
        rc::st16(out, x86::reg16(bits));
    }
    cpu.eax = frames;
    if (pf::perf::enabled())
    {
        pf::perf::audio(pf::perf::AudioOutputSamples, frames);
        pf::perf::audio(pf::perf::AudioSaturatedHigh, high);
        pf::perf::audio(pf::perf::AudioSaturatedLow, low);
    }
}

}
}
