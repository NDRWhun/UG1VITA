// EA SND 8 software mixer (docs: audio.md). The audio thread runs the render loop 0x654050 (SND ticks, the
// millisecond callbacks with the bank routines, the voice render 0x656D70 and the stream notifications) outside
// the execution lock, holding the SND mutex as the game does; each part checks what it reaches and takes the lock
// again when the check fails. The x87 kernels of the render run natively. Each kernel does the
// guest's loads, double operations and stores in the guest's order, so its output is bit-identical (only where two
// NaNs meet in one operation may the result carry the other NaN's payload: the compiler orders the operands, here as
// in the generated code).

#include "audio_mixer.h"

#include <lib/mutex.h>
#include <lib/teb.h>
#include <lib/winapp.h>
#include <platform/perf.h>
#include <recomp.h>
#include <x86.h>

#include <atomic>
#include <cstring>

namespace nfsu
{

// Generated guest code.
void sub_654050_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_64e330_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_64e2f0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_655880_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_64bbd0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_656d70_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_65f930(win32::WinApplication* app, x86::CPU& cpu);

namespace mixer
{

namespace
{

// Thread block (fs) of the guest thread in the render loop without the execution lock, 0 while there is none. Only
// that thread stores it; a thread compares it with its own block.
std::atomic<x86::reg32> s_thread{0};
// The execution lock that thread gave up, and whether SND state may have changed under the lock since the handle
// audit; only that thread uses them.
win32::Mutex* s_lock = nullptr;
bool s_changed = true;

void setLock(win32::WinApplication* app, const x86::CPU& cpu, win32::Mutex* lock)
{
    app->getMemory<win32::Mutex*>(cpu.efs + win32::teb::HostLock) = lock;
}

}

bool unlocked(const x86::CPU& cpu)
{
    return s_thread.load(std::memory_order_relaxed) == cpu.efs;
}

Locked::Locked(win32::WinApplication* app, const x86::CPU& cpu)
    :   m_app(app)
    ,   m_cpu(cpu)
    ,   m_taken(unlocked(cpu))
{
    if (!m_taken)
        return;
    s_thread.store(0, std::memory_order_relaxed);
    setLock(m_app, m_cpu, s_lock);
    m_app->lockContext(m_cpu);
}

Locked::~Locked()
{
    if (!m_taken)
        return;
    m_app->unlockContext(m_cpu);
    setLock(m_app, m_cpu, nullptr);
    s_changed = true;
    s_thread.store(m_cpu.efs, std::memory_order_relaxed);
}

}

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
// audited set, the render runs without the execution lock, else it takes the lock for the render.

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

// With the SND mutex held.
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

// ---- Handles and their drivers --------------------------------------------------------------------------------
//
// The SND tick and the sound calls of bank routines (0x64D860, 0x64C690) set the pitch, volume, filters and fades of
// handles (0x8C bytes at [0x713F48]) and stop them: the handle an id names (index id & 0xFF, any state but free),
// the handles of its group in use, and the handles it lists (+0x23 count, int16 indices from +4). That reaches SND
// state, each handle's driver (0x110 bytes at [0x71400C]: functions with their voice list argument), the voices it
// lists with their module chains, teardowns and position queries, and the SND pool. The game changes all of it only
// through SND calls that take the SND mutex.
//
// The audit checks every handle that is not free and the handles those list: software kind, and every driver
// function either null (a call of it fails as it does with the lock) or the software voice's. Only a sound start
// (driver set up, voices created) changes what it checks, and a start runs with the lock; the stops, parameter
// changes and module inserts the other calls make keep it true. So it is made once per run of the render loop and
// again after each call that took the lock (mixer::Locked).

const reg32 Handles         = 0x713f48;     // 0x8C bytes each: +0 id, +4 listed handles, +0x23 their count,
                                            // +0x24 kind flags, +0x69 state (0 = free)
const reg32 HandleCount     = 0x713ee8;     // int16
const reg32 HandleSize      = 0x8c;
const reg32 SoftwareHandle  = 4;            // kind flag of handles mixed by the software voices
const reg32 Drivers         = 0x71400c;     // 0x110 bytes per handle
const reg32 DriverSize      = 0x110;
const reg32 DriverVoices    = 0x2c;         // byte count, then int16 voice numbers from +4
const reg32 TickHookCount   = 0x713ee6;     // int8; hooks at 0x713EF0
const reg32 PoolFree        = 0x656500;
const int   MaxNodes        = 4096;         // bound of the list walks

// Driver function slot, the slot of its argument (0: the voice list itself), and the software voice's function.
struct DriverSlot
{
    reg32   function;
    reg32   argument;
    reg32   software;
};

const DriverSlot s_driverSlots[] = {
    {0xe8,  0,    0x6577a0},    // stop
    {0xec,  0xc8, 0x657540},    // volume
    {0xf0,  0xcc, 0x657590},    // filter of each voice
    {0xf4,  0xc4, 0x6574d0},    // pan of each listed handle
    {0xf8,  0xd0, 0x6575d0},    // low-pass module
    {0xfc,  0xd4, 0x657630},    // high-pass module
    {0x108, 0xd8, 0x657670},    // play position of the first voice
};

// Voice +0x44: play position of its source module (null: none).
const reg32 s_positionQueries[] = { 0, 0x6591c0, 0x6594d0 };

// Module teardowns (+4, along the +8 links) of a voice the stop tears down.
bool teardownsKnown(reg32 voice, int& budget, Audit& audit)
{
    for (reg32 module = rc::ld32(voice + 0x40); module; module = rc::ld32(module + 8))
    {
        if (--budget < 0)
            return audit.fail("module count", module);
        const reg32 teardown = rc::ld32(module + 4);
        if (!contains(s_teardowns, teardown))
            return audit.fail("teardown", teardown);
    }
    return true;
}

bool driverAudited(reg32 index, int& budget, Audit& audit)
{
    const reg32 handle = rc::ld32(Handles) + index * HandleSize;
    const reg32 kind = rc::ld16(handle + 0x24);
    if (!(kind & SoftwareHandle))
        return audit.fail("handle kind", kind);
    const reg32 driver = rc::ld32(Drivers) + index * DriverSize;
    for (const DriverSlot& slot : s_driverSlots)
    {
        const reg32 function = rc::ld32(driver + slot.function);
        if (!function)
            continue;
        if (function != slot.software)
            return audit.fail("driver function", function);
        if (slot.argument && rc::ld32(driver + slot.argument) != driver + DriverVoices)
            return audit.fail("driver voices", rc::ld32(driver + slot.argument));
    }
    // The stop of a handle of kind 2 without kind 1 walks the effect list at driver +0xE4.
    if ((kind & 3) == 2 && rc::ld32(driver + 0xe4))
        return audit.fail("handle effects", rc::ld32(driver + 0xe4));
    const reg32 voices = rc::ld32(VoiceArray);
    const reg32 voiceCount = rc::ld8(VoiceCount);
    const reg32 listed = rc::ld8(driver + DriverVoices);
    for (reg32 k = 0; k < listed; ++k)
    {
        const reg32 number = reg32(x86::sreg16(rc::ld16(driver + DriverVoices + 4 + 2 * k)));
        if (number >= voiceCount)
            return audit.fail("handle voice", number);
        if (!teardownsKnown(voices + number * VoiceSize, budget, audit))
            return false;
    }
    return true;
}

// With the SND mutex held.
bool driversChecked(Audit& audit)
{
    if (rc::ld32(FreeHook) != PoolFree)
        return audit.fail("free hook", rc::ld32(FreeHook));
    const reg32 handles = rc::ld32(Handles);
    const x86::sreg32 count = x86::sreg16(rc::ld16(HandleCount));
    int budget = MaxModules;
    for (x86::sreg32 i = 0; i < count; ++i)
    {
        const reg32 handle = handles + reg32(i) * HandleSize;
        if (!rc::ld8(handle + 0x69))
            continue;
        if (!driverAudited(reg32(i), budget, audit))
            return false;
        const reg32 listed = rc::ld8(handle + 0x23);
        for (reg32 k = 0; k < listed; ++k)
        {
            const x86::sreg32 other = x86::sreg16(rc::ld16(handle + 4 + 2 * k));
            if (other < 0 || other >= count)
                return audit.fail("listed handle", reg32(other));
            if (!driverAudited(reg32(other), budget, audit))
                return false;
        }
    }
    return true;
}

// The audit of the current run of the render loop (audio thread only).
enum DriverState
{
    DriversUnchecked,
    DriversPassed,
    DriversFailed,
};
DriverState s_driverState = DriversUnchecked;
Audit s_driverAudit;

bool driversAudited(Audit& audit)
{
    if (mixer::s_changed)
    {
        mixer::s_changed = false;
        s_driverState = DriversUnchecked;
    }
    if (s_driverState == DriversUnchecked)
        s_driverState = driversChecked(s_driverAudit) ? DriversPassed : DriversFailed;
    if (s_driverState == DriversFailed)
        audit = s_driverAudit;
    return s_driverState == DriversPassed;
}

// The tick 0x64E330 (each 10 ms of output) runs the tick hooks, then for every handle in use its envelopes and fade.
bool tickAudited(Audit& audit)
{
    const reg32 hooks = rc::ld8(TickHookCount);
    if (x86::sreg8(hooks) > 0)
        return audit.fail("tick hooks", hooks);
    return driversAudited(audit);
}

// ---- Sound calls of bank routines -----------------------------------------------------------------------------

// 0x64D860 (instance): moves the instance's sound to the target state [+0x18] (clamped to 0..2) from [+0xC] through
// the handlers of sound kind [+0x10] (tables of 4 kinds from 0x6E9A98 to 0x6E9AD8; -1 is none): 0 stops it, 2 pauses
// it, 1 starts it when there is none (0x6E9A98: voices and a driver, or a stream, are set up) or resumes it, and
// while it stays at 1 its parameters go to the kind's update. Kind 0 is a sample: its update passes the parameters
// whose value changed to the setters at 0x6E9A38 by their kind [+0x1C + 0xC n], and with [+0xF] set reads the
// handle's play position (driver +0x108: the position query of its first voice). Kinds 1 and 2 are streams (handlers
// set by 0x64F320): their pause 0x64EDF0, resume 0x64EE30 and update 0x64EEF0 (parameters to the setters at
// 0x6E9B98) reach SND state and the stream's handles only; their stop 0x64F290 reaches the game's stream reader, so
// a stop runs with the lock, as do starts of every kind.
const x86::sreg32 StreamKinds = 2;
const reg32 StreamSetters = 8;
bool positionAudited(reg32 instance, Audit& audit)
{
    const reg32 id = rc::ld32(instance + 8);
    const reg32 index = id & 0xff;
    const reg32 handle = rc::ld32(Handles) + index * HandleSize;
    if (x86::sreg32(id) < 0 || x86::sreg32(index) >= x86::sreg16(rc::ld16(HandleCount)) || !rc::ld8(handle + 0x69)
        || rc::ld32(handle) != id)
        return true;
    const reg32 driver = rc::ld32(Drivers) + index * DriverSize;
    if (!rc::ld32(driver + 0x108))
        return true;
    const reg32 number = reg32(x86::sreg16(rc::ld16(driver + DriverVoices + 4)));
    if (number >= rc::ld8(VoiceCount))
        return audit.fail("position voice", number);
    const reg32 query = rc::ld32(rc::ld32(VoiceArray) + number * VoiceSize + 0x44);
    if (!contains(s_positionQueries, query))
        return audit.fail("position query", query);
    return true;
}

bool soundAudited(reg32 instance, Audit& audit)
{
    const x86::sreg32 wanted = x86::sreg32(rc::ld32(instance + 0x18));
    const x86::sreg32 state = wanted < 0 ? 0 : wanted > 2 ? 2 : wanted;
    const x86::sreg32 current = x86::sreg8(rc::ld8(instance + 0xc));
    const x86::sreg32 sound = x86::sreg8(rc::ld8(instance + 0x10));
    if (sound > StreamKinds)
        return audit.fail("sound kind", reg32(sound));
    if (state != current && state == 1 && sound < 0 && !(current == 2 && rc::ld8(instance + 0xd) == 1))
        return audit.fail("sound start", instance);
    if (state != current && state == 0 && sound > 0)
        return audit.fail("stream stop", instance);
    if (state == 1 && sound >= 0)
    {
        // Sample setter kinds 9..11 are the empty function 0x40A880; past them the table holds other handlers.
        const reg32 setters = sound == 0 ? 12 : StreamSetters;
        const reg32 parameters = rc::ld8(instance + 0xe);
        for (reg32 k = 0; k < parameters; ++k)
        {
            const reg32 kind = rc::ld8(instance + 0x1c + 0xc * k);
            if (kind >= setters)
                return audit.fail("parameter kind", kind);
        }
        if (!driversAudited(audit))
            return false;
        return sound != 0 || !rc::ld8(instance + 0xf) || positionAudited(instance, audit);
    }
    return driversAudited(audit);
}

// The game's allocator object [0x713C54] (EAXSND8Wrapper, vtable 0x6BE4F0): its free 0x5329B0 puts the block back
// on the game's sound slot pool [0x72D558], which the game only touches through this allocator with the SND mutex.
const reg32 Allocator       = 0x713c54;
const reg32 AllocatorTable  = 0x6be4f0;
const reg32 AllocatorFree   = 0x5329b0;

bool allocatorAudited(Audit& audit)
{
    const reg32 allocator = rc::ld32(Allocator);
    if (!allocator || rc::ld32(allocator) != AllocatorTable)
        return audit.fail("allocator", allocator ? rc::ld32(allocator) : 0);
    if (rc::ld32(AllocatorTable + 4) != AllocatorFree)
        return audit.fail("allocator free", rc::ld32(AllocatorTable + 4));
    return true;
}

// 0x64C690 (instance): when it is linked, unlinks it from its bank entry and the bank tick, releases its objects
// (0x64B870, 0x64B8E0 and 0x64BBD0, which free them through the allocator; 0x64BBD0's listener check is its own),
// stops the sounds of the kinds [+0x10] of its sound entries through 0x6E9AA8 (kind 0 only), and frees it.
bool teardownAudited(reg32 instance, Audit& audit)
{
    if (!rc::ld32(instance + 0xc))
        return true;
    const reg32 entry = rc::ld32(instance);
    const reg32 data = rc::ld32(instance + 4);
    const reg32 sounds = rc::ld8(entry + 0x24);
    for (reg32 k = 0; k < sounds; ++k)
    {
        const x86::sreg32 kind = x86::sreg8(rc::ld8(rc::ld32(entry + 0x3c + 4 * k) + data + 0x10));
        if (kind > 0)
            return audit.fail("sound kind", reg32(kind));
    }
    return allocatorAudited(audit) && driversAudited(audit);
}

// 0x64BBD0 (this = object): calls the release listeners [+0xC] and frees the object at its last reference. The one
// listener the code links there is 0x64DBB0 (bank instances), which marks the instance's slot.
const reg32 ReleaseListener = 0x64dbb0;

bool releaseAudited(reg32 object, Audit& audit)
{
    int budget = MaxNodes;
    for (reg32 node = rc::ld32(object + 0xc); node; node = rc::ld32(node))
    {
        if (--budget < 0)
            return audit.fail("listener count", node);
        if (rc::ld32(node + 8) != ReleaseListener)
            return audit.fail("release listener", rc::ld32(node + 8));
    }
    return allocatorAudited(audit);
}

// ---- Millisecond callbacks ------------------------------------------------------------------------------------
//
// 0x64E2F0 runs the callback list 0x713CA8 once per new value of the millisecond count. The one callback the code
// links there is the bank tick 0x652960 (by the bank loader), which runs the routines linked at 0x713C70: code in
// the loaded banks, run by bankcode.cpp, whose library calls that fail their audit take the execution lock again.

const reg32 Callbacks       = 0x713ca8;     // nodes: +0 next, +8 function, +0xC argument
const reg32 BankTick        = 0x652960;
const reg32 BankRoutines    = 0x713c70;     // nodes: +0 next, +8 routine, +0xC argument

// With the SND mutex held.
bool callbacksAudited(win32::WinApplication* app, Audit& audit)
{
    int budget = MaxNodes;
    for (reg32 node = rc::ld32(Callbacks); node; node = rc::ld32(node))
    {
        if (--budget < 0)
            return audit.fail("callback count", node);
        if (rc::ld32(node + 8) != BankTick)
            return audit.fail("callback", rc::ld32(node + 8));
    }
    // A routine is bank code: no translated function has its address.
    for (reg32 node = rc::ld32(BankRoutines); node; node = rc::ld32(node))
    {
        if (--budget < 0)
            return audit.fail("bank callback count", node);
        if (app->getMethodName(rc::ld32(node + 8)))
            return audit.fail("bank callback", rc::ld32(node + 8));
    }
    return true;
}

// ---- Stream notifications -------------------------------------------------------------------------------------
//
// 0x655880 delivers the notifications the stream modules queued during the render to the callbacks the game gave
// each stream (+0x54, +0x58 of the stream at 0x7163DC): game code, so a delivery takes the execution lock.

const reg32 Notifications   = 0x7160d8;     // count; entries of 8 bytes from 0x7160DC

}

// Render loop (pointer to the output pointer, frames): SND ticks of 10 ms, the millisecond callbacks, the voice
// render and the stream notifications, until the frames are written. The audio thread runs it with the SND mutex
// and gives the execution lock up meanwhile; a part that fails its audit, and every notification delivery, takes it
// for that call only (mixer::Locked).
void sub_654050(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 slot = cpu.efs + win32::teb::HostLock;
    win32::Mutex* const lock = app->getMemory<win32::Mutex*>(slot);
    if (!lock || mixer::unlocked(cpu))
    {
        sub_654050_guest(app, cpu);
        return;
    }
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    app->unlockContext(cpu);
    app->getMemory<win32::Mutex*>(slot) = nullptr;
    mixer::s_lock = lock;
    mixer::s_changed = true;
    mixer::s_thread.store(cpu.efs, std::memory_order_relaxed);
    sub_654050_guest(app, cpu);
    mixer::s_thread.store(0, std::memory_order_relaxed);
    app->getMemory<win32::Mutex*>(slot) = lock;
    if (timed)
    {
        pf::perf::audio(pf::perf::AudioLoopsUnlocked, 1);
        pf::perf::audio(pf::perf::AudioLoopUnlockedUs, pf::perf::now() - start);
    }
    app->lockContext(cpu);
}

// SND tick.
void sub_64e330(win32::WinApplication* app, x86::CPU& cpu)
{
    Audit audit;
    if (!mixer::unlocked(cpu) || tickAudited(audit))
    {
        sub_64e330_guest(app, cpu);
        return;
    }
    {
        mixer::Locked locked(app, cpu);
        sub_64e330_guest(app, cpu);
    }
    if (pf::perf::enabled())
    {
        pf::perf::audio(pf::perf::AudioTicksLocked, 1);
        pf::perf::audioLocked(audit.reason, audit.address);
    }
}

// Millisecond callbacks.
void sub_64e2f0(win32::WinApplication* app, x86::CPU& cpu)
{
    Audit audit;
    if (!mixer::unlocked(cpu) || callbacksAudited(app, audit))
    {
        sub_64e2f0_guest(app, cpu);
        return;
    }
    {
        mixer::Locked locked(app, cpu);
        sub_64e2f0_guest(app, cpu);
    }
    if (pf::perf::enabled())
    {
        pf::perf::audio(pf::perf::AudioCallbacksLocked, 1);
        pf::perf::audioLocked(audit.reason, audit.address);
    }
}

// Releases an object (this = ecx), called by the game and by instance teardowns of bank routines.
void sub_64bbd0(win32::WinApplication* app, x86::CPU& cpu)
{
    Audit audit;
    if (!mixer::unlocked(cpu) || releaseAudited(cpu.ecx, audit))
    {
        sub_64bbd0_guest(app, cpu);
        return;
    }
    {
        mixer::Locked locked(app, cpu);
        sub_64bbd0_guest(app, cpu);
    }
    if (pf::perf::enabled())
    {
        pf::perf::audio(pf::perf::AudioLibraryLocked, 1);
        pf::perf::audioLocked(audit.reason, audit.address);
    }
}

// Stream notifications: without any queued, the guest only clears the count.
void sub_655880(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!mixer::unlocked(cpu) || x86::sreg32(rc::ld32(Notifications)) <= 0)
    {
        sub_655880_guest(app, cpu);
        return;
    }
    {
        mixer::Locked locked(app, cpu);
        sub_655880_guest(app, cpu);
    }
    if (pf::perf::enabled())
        pf::perf::audio(pf::perf::AudioNotificationsLocked, 1);
}

// Renders frames into the DirectSound buffer: (pointer to the output pointer, frames). In the render loop without
// the execution lock it runs as it is when the audit passes, else with the lock; no preemption point inside takes
// the lock back.
void sub_656d70(win32::WinApplication* app, x86::CPU& cpu)
{
    const bool timed = pf::perf::enabled();
    const uint32_t start = timed ? pf::perf::now() : 0;
    Audit audit;
    if (mixer::unlocked(cpu) && renderAudited(audit))
    {
        sub_656d70_guest(app, cpu);
        if (timed)
        {
            pf::perf::audio(pf::perf::AudioRendersUnlocked, 1);
            pf::perf::audio(pf::perf::AudioRenderUnlockedUs, pf::perf::now() - start);
        }
        return;
    }
    const char* const reason = mixer::unlocked(cpu) ? audit.reason : "outside the render loop";
    {
        mixer::Locked locked(app, cpu);
        sub_656d70_guest(app, cpu);
    }
    if (timed)
    {
        pf::perf::audio(pf::perf::AudioRendersLocked, 1);
        pf::perf::audio(pf::perf::AudioRenderLockedUs, pf::perf::now() - start);
        pf::perf::audioLocked(reason, audit.address);
    }
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

namespace mixer
{

bool libraryAudited(x86::reg32 target, const x86::CPU& cpu, const char*& reason, x86::reg32& value)
{
    const x86::reg32 SoundState = 0x64d860;
    const x86::reg32 InstanceTeardown = 0x64c690;
    native::Audit audit;
    const x86::reg32 instance = rc::ld32(cpu.esp + 4);
    bool passed;
    if (target == SoundState)
        passed = native::soundAudited(instance, audit);
    else if (target == InstanceTeardown)
        passed = native::teardownAudited(instance, audit);
    else
        passed = audit.fail("library call", target);
    reason = audit.reason;
    value = audit.address;
    return passed;
}

}
}
