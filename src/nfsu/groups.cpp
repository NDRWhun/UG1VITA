// Native effect group functions and model texture swaps (groups.h).

#include "groups.h"

#include "draw.h"

#include <winapi/d3d9_native.h>
#include <platform/config.h>
#include <platform/log.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace nfsu
{

// The translated guest bodies of the overridden functions (generated).
void sub_40e740_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40e7b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40e900_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40eb30_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_40eb60_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_53f540_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_53f500_guest(win32::WinApplication* app, x86::CPU& cpu);
// Texture lookup by name hash (generated), for the case the native code does not cover.
void sub_5461c0(win32::WinApplication* app, x86::CPU& cpu);

namespace groups
{

namespace
{

using win32::WinApplication;
using x86::reg8;
using x86::reg32;
using x86::sreg8;
using x86::sreg32;
namespace bridge = win32::d3d9::bridge;

// Guest data.
const reg32 CurrentGroup = 0x73640c;    // the effect group being drawn: effect +0x18, pass count +0xc
const reg32 DevicePointer = 0x73636c;
const reg32 CurrentView = 0x736458;     // view index at +4
const reg32 Views = 0x740460;           // 16 eViews of 0x60 bytes, the camera pointer at +0
const reg32 Packs = 0x740418;           // texture pack list: next +0, previous +4
const reg32 DefaultTexture = 0x73551c;
const reg32 HeadlightsName = 0x6cb25c;  // "HEADLIGHTS"
const reg32 BlackTexture = 0x71abb4;
const reg32 WhiteTexture = 0x71aa28;
const reg32 GlossValue = 0x6b7c68;
const reg32 OverlayTexture = 0x73646c;

// Group functions of the effect group table (0x702420).
const reg32 Empty = 0x40a880;

const uint32_t ShadowLogLimit = 20;

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("groups_native", true), pf::Config::getBool("shadow_groups", false) };
        pf::logInfo("groups: effect group functions and model texture swaps %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// Stores of the native code; the shadow compare logs their addresses while its native run is on.
bool s_logStores = false;

inline void store(reg32 address, reg32 value)
{
    if (__builtin_expect(s_logStores, 0))
        draw::g_stores.push_back(address);
    rc::st32(address, value);
}

inline void storeByte(reg32 address, reg8 value)
{
    if (__builtin_expect(s_logStores, 0))
        draw::g_stores.push_back(address & ~3u);
    rc::st8(address, value);
}

inline reg32 ld(reg32 address)
{
    return rc::ld32(address);
}

// ---- 0x5461c0: texture by name hash ----

// ScanHashTableKey32 0x53e400 with key offset 0 and stride 8 (eax = count): the first entry of the table sorted by
// key with the key, or 0; ecx and edx as it leaves them.
reg32 scan(reg32 key, reg32 table, sreg32 count, reg32& ecx, reg32& edx)
{
    if (table == 0 || count <= 0)
        return 0;
    sreg32 low = 0, high = count - 1;
    while (low <= high)
    {
        const sreg32 middle = (high + low) / 2;
        ecx = reg32(middle);
        reg32 at = reg32(middle) * 8 + table;
        edx = ld(at);
        if (key < edx)
            high = middle - 1;
        else if (key > edx)
            low = middle + 1;
        else
        {
            if (middle <= 0)
                return at;
            sreg32 n = middle;
            edx = at - 8;
            for (;;)
            {
                if (ld(edx) != key)
                    return at;
                at -= 8;
                edx -= 8;
                ecx = reg32(--n);
                if (n <= 0)
                    return at;
            }
        }
    }
    return 0;
}

// The pack and texture of a hash in list order (no stores); ecx and edx as the scans leave them.
bool find(reg32 hash, reg32& pack, reg32& texture, reg32& ecx, reg32& edx)
{
    if (hash == 0)
        return false;
    for (pack = ld(Packs); pack != Packs; pack = ld(pack))
    {
        const reg32 index = ld(pack + 0x70);
        if (index == 0)
            continue;
        const reg32 table = ld(index + 0x74);
        if (table == 0)
            continue;
        const reg32 entry = scan(hash, table, sreg32(ld(pack + 0x7c)), ecx, edx);
        if (entry == 0)
            continue;
        texture = ld(entry + 4);
        if (texture != 0)
            return true;
    }
    return false;
}

// (hash, 1, 0): the texture, its pack moved to the front of the list and the texture marked used; else the default
// texture. False when neither exists (the guest function stops at an int3).
bool lookupNative(reg32 hash, reg32& eax, reg32& ecx, reg32& edx)
{
    reg32 pack = 0, texture = 0;
    if (find(hash, pack, texture, ecx, edx))
    {
        ecx = ld(pack);
        edx = ld(pack + 4);
        store(edx, ecx);
        store(ecx + 4, edx);
        ecx = ld(Packs);
        store(Packs, pack);
        store(ecx + 4, pack);
        store(pack + 4, Packs);
        store(pack, ecx);
        storeByte(texture + 0x53, 1);
        eax = texture;
        return true;
    }
    eax = ld(DefaultTexture);
    return eax != 0;
}

// The texture of the name at address as the group functions hash it (bStringHash): edx ends at the terminator.
reg32 nameHash(reg32 address, reg32& edx)
{
    reg32 hash = 0xffffffffu;
    reg8 c = rc::ld8(address);
    edx = address;
    do
    {
        hash = hash * 33 + c;
        c = rc::ld8(edx + 1);
        ++edx;
    } while (c != 0);
    return hash;
}

// ---- the group functions ----

// The view's texture projection: the camera matrix times the screen bias, its fourth column copied to the third.
void projection(float* m)
{
    static const float Bias[16] = { 0.5f, 0, 0, 0, 0, -0.5f, 0, 0, 0, 0, 1, 0, 0.5f, 0.5f, 0, 1 };
    float r[16];
    memcpy(r, Bias, sizeof(r));
    const reg32 view = ld(ld(CurrentView) + 4) * 0x60 + Views;
    bridge::matrixMultiply(reinterpret_cast<const float*>(rc::host(ld(view) + 0x40)), r, r);
    r[2] = r[3];
    r[14] = r[15];
    r[6] = r[7];
    r[10] = r[11];
    memcpy(m, r, sizeof(r));
}

// The headlight texture into the group's handle +0x44 (lookup at the guest esp sp).
void headlights(WinApplication* app, x86::CPU& cpu, reg32 sp)
{
    reg32 edx = 0;
    const reg32 hash = nameHash(HeadlightsName, edx);
    reg32 eax = 0, ecx = hash;
    lookup(app, cpu, hash, sp, eax, ecx, edx);
    const reg32 group = ld(CurrentGroup);
    const reg32 effect = ld(group + 0x18);
    const reg32 texture = ld(ld(eax) + 0x18);
    bridge::effectSetTexture(app, cpu, effect, ld(group + 0x44), texture);
}

// Effect SetTexture of the current group's handle at offset to the texture at the guest address; eax, ecx, edx as
// the guest leaves them.
void setGroupTexture(WinApplication* app, x86::CPU& cpu, reg32 handleOffset, reg32 textureAddress)
{
    const reg32 group = ld(CurrentGroup);
    const reg32 effect = ld(group + 0x18);
    const reg32 handle = ld(group + handleOffset);
    cpu.eax = bridge::effectSetTexture(app, cpu, effect, handle, ld(textureAddress));
    cpu.ecx = handle;
    cpu.edx = ld(effect);
}

void setGloss(WinApplication* app, x86::CPU& cpu)
{
    const reg32 group = ld(CurrentGroup);
    const reg32 effect = ld(group + 0x18);
    const reg32 handle = ld(group + 0x8c);
    float value;
    memcpy(&value, rc::host(GlossValue), 4);
    cpu.eax = bridge::effectSetFloat(app, cpu, effect, handle, value);
    cpu.ecx = handle;
    cpu.edx = ld(effect);
}

// The texture projection into handle +0x88 when the group has passes; false (eax, ecx, edx set) when it has none.
bool setProjection(WinApplication* app, x86::CPU& cpu)
{
    const reg32 group = ld(CurrentGroup);
    if (ld(group + 0xc) == 0)
    {
        cpu.eax = 0;
        cpu.edx = group;
        return false;
    }
    float m[16];
    projection(m);
    const reg32 g = ld(CurrentGroup);
    const reg32 effect = ld(g + 0x18);
    const reg32 handle = ld(g + 0x88);
    cpu.eax = bridge::effectSetMatrix(app, cpu, effect, handle, m);
    cpu.ecx = handle;
    cpu.edx = ld(effect);
    return true;
}

// 0x40e740: headlight texture (+0x44) and black texture (+0x7c).
void startHeadlightsNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    headlights(app, cpu, entry - 0x14);
    setGroupTexture(app, cpu, 0x7c, BlackTexture);
    cpu.esp = entry + 4;
}

// 0x40e7b0: white texture (+0x84), gloss (+0x8c), and the texture projection when the group has passes.
void startProjectedNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    setGroupTexture(app, cpu, 0x84, WhiteTexture);
    setGloss(app, cpu);
    setProjection(app, cpu);
    cpu.esp = entry + 4;
}

// 0x40e900: headlight, black and white textures, then the projection when the group has passes, else the headlight
// texture again with the black and white ones and the gloss.
void startCarNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 frame = ((entry - 4) & ~15u) - 0x4c - 4;
    headlights(app, cpu, frame - 0x10);
    setGroupTexture(app, cpu, 0x7c, BlackTexture);
    setGroupTexture(app, cpu, 0x84, WhiteTexture);
    const reg32 group = ld(CurrentGroup);
    if (ld(group + 0xc) != 0)
        setProjection(app, cpu);
    else
    {
        headlights(app, cpu, frame - 0x10);
        setGroupTexture(app, cpu, 0x7c, BlackTexture);
        setGroupTexture(app, cpu, 0x84, WhiteTexture);
        setGloss(app, cpu);
    }
    cpu.esp = entry + 4;
}

// 0x40eb30: overlay texture (+0x80).
void startOverlayNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    setGroupTexture(app, cpu, 0x80, OverlayTexture);
    cpu.esp = entry + 4;
}

// 0x40eb60: texture transforms of stages 0-3 off.
void startPlainNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 device = ld(DevicePointer);
    for (reg32 stage = 0; stage < 4; ++stage)
        cpu.eax = bridge::setTextureStageState(app, cpu, ld(DevicePointer), stage, 0x18, 0);
    cpu.ecx = ld(device);
    cpu.edx = ld(device);
    cpu.esp = entry + 4;
}

// ---- 0x53f540, 0x53f500: model texture swaps ----

// eax = the model: its replacement table (+0x10, entries of 16 bytes: name hash, texture or -1 until looked up,
// saved texture, solid slot) of +0x14 entries, its solid +0xc with +0x19 texture slots at +0x2c (hash, texture).
// Unresolved entries are looked up, then each slot whose hash has an entry gets the entry's texture, the slot's own
// saved in the entry.
void resolveNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 model = cpu.eax;
    const reg32 solid = ld(model + 0xc);
    const reg32 table = ld(model + 0x10);
    const sreg32 count = sreg32(ld(model + 0x14));
    const sreg32 slots = sreg32(sreg8(rc::ld8(solid + 0x19)));
    reg32 eax = reg32(slots), ecx = cpu.ecx, edx = cpu.edx;
    if (table != 0 && count != 0)
    {
        for (sreg32 i = 0; i < count; ++i)
        {
            const reg32 at = table + 8 + 16 * reg32(i);
            if (ld(at) != 0xffffffffu)
                continue;
            ecx = ld(at - 4);
            lookup(app, cpu, ecx, entry - 0x28, eax, ecx, edx);
            if (cpu.terminate)
                return;
            store(at, eax);
        }
        eax = reg32(slots);
        for (sreg32 i = 0; i < slots; ++i)
        {
            edx = ld(solid + 0x2c);
            const reg32 key = ld(edx + 8 * reg32(i));
            eax = table;
            edx += 8 * reg32(i);
            ecx = 0;
            if (count > 0)
            {
                for (;;)
                {
                    if (ld(eax + 4) != 0 && ld(eax) == key)
                    {
                        ecx = ld(edx + 4);
                        store(edx + 4, ld(eax + 8));
                        edx = ld(solid + 0x2c) + 8 * reg32(i) + 4;
                        store(eax + 0xc, edx);
                        store(eax + 8, ecx);
                        break;
                    }
                    eax += 0x10;
                    ++ecx;
                    if (sreg32(ecx) >= count)
                        break;
                }
            }
            eax = reg32(slots);
        }
    }
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.esp = entry + 4;
}

// eax = the model: each entry with a solid slot puts the saved texture back into the slot and keeps the slot's.
void restoreNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    const reg32 model = cpu.eax;
    reg32 eax = model;
    reg32 ecx = ld(model + 0x14);
    reg32 edx = ld(model + 0x10);
    if (edx != 0 && ld(model + 0xc) != 0 && sreg32(ecx) > 0)
    {
        eax = edx + 0xc;
        for (sreg32 n = sreg32(ecx); n > 0; --n, eax += 0x10)
        {
            ecx = ld(eax);
            if (ecx == 0)
                continue;
            edx = ld(ecx);
            store(ecx, ld(eax - 4));
            store(eax, 0);
            store(eax - 4, edx);
        }
    }
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.esp = entry + 4;
}

// ---- dispatch and shadow compare ----

typedef void (*Function)(WinApplication*, x86::CPU&);

enum Index
{
    StartHeadlights, StartProjected, StartCar, StartOverlay, StartPlain, Resolve, Restore, Count
};

struct Info
{
    const char* name;
    Function guest;
    Function native;
    reg32 address;
};

const Info s_functions[Count] = {
    { "0x40e740", &sub_40e740_guest, &startHeadlightsNative, 0x40e740 },
    { "0x40e7b0", &sub_40e7b0_guest, &startProjectedNative, 0x40e7b0 },
    { "0x40e900", &sub_40e900_guest, &startCarNative, 0x40e900 },
    { "0x40eb30", &sub_40eb30_guest, &startOverlayNative, 0x40eb30 },
    { "0x40eb60", &sub_40eb60_guest, &startPlainNative, 0x40eb60 },
    { "texture swap 0x53f540", &sub_53f540_guest, &resolveNative, 0x53f540 },
    { "texture restore 0x53f500", &sub_53f500_guest, &restoreNative, 0x53f500 },
};

// Memory a lookup of hash may store to: the pack list links and the found texture's used flag.
void lookupSpans(reg32 hash, std::vector<draw::Span>& spans)
{
    if (spans.empty() || spans.front().address != Packs)
    {
        spans.push_back(draw::Span{Packs, 8});
        reg32 pack = ld(Packs);
        for (uint32_t n = 0; pack != Packs && n < 10000; ++n, pack = ld(pack))
            spans.push_back(draw::Span{pack, 8});
    }
    reg32 pack = 0, texture = 0, ecx = 0, edx = 0;
    if (find(hash, pack, texture, ecx, edx))
        spans.push_back(draw::Span{texture + 0x50, 4});
}

void spansOf(Index index, const x86::CPU& cpu, std::vector<draw::Span>& spans)
{
    spans.clear();
    switch (index)
    {
    case StartHeadlights:
    case StartCar:
    {
        reg32 edx = 0;
        lookupSpans(nameHash(HeadlightsName, edx), spans);
        break;
    }
    case Resolve:
    {
        const reg32 model = cpu.eax;
        const reg32 table = ld(model + 0x10);
        const sreg32 count = sreg32(ld(model + 0x14));
        if (table == 0 || count <= 0)
            break;
        spans.push_back(draw::Span{table, 16 * reg32(count)});
        const reg32 solid = ld(model + 0xc);
        const sreg32 slots = sreg32(sreg8(rc::ld8(solid + 0x19)));
        if (slots > 0)
            spans.push_back(draw::Span{ld(solid + 0x2c), 8 * reg32(slots)});
        for (sreg32 i = 0; i < count; ++i)
        {
            if (ld(table + 8 + 16 * reg32(i)) == 0xffffffffu)
                lookupSpans(ld(table + 4 + 16 * reg32(i)), spans);
        }
        break;
    }
    case Restore:
    {
        const reg32 model = cpu.eax;
        const reg32 table = ld(model + 0x10);
        const sreg32 count = sreg32(ld(model + 0x14));
        if (table == 0 || count <= 0)
            break;
        spans.push_back(draw::Span{table, 16 * reg32(count)});
        for (sreg32 i = 0; i < count; ++i)
        {
            const reg32 slot = ld(table + 0xc + 16 * reg32(i));
            if (slot != 0)
                spans.push_back(draw::Span{slot, 4});
        }
        break;
    }
    default:
        break;
    }
}

struct Shadow
{
    bridge::Capture capture;
    std::vector<draw::Span> spans;
    std::vector<reg8> before, guestAfter, nativeAfter;
    uint32_t calls = 0, differing = 0, logged = 0;
};

Shadow s_shadow;
uint32_t s_depth = 0;

void shadow(Index index, WinApplication* app, x86::CPU& cpu)
{
    Shadow& s = s_shadow;
    const Info& info = s_functions[index];
    const x86::CPU entry = cpu;
    spansOf(index, entry, s.spans);
    draw::snapshot(s.spans, s.before);
    s.capture.clear();
    bridge::begin(s.capture, bridge::Capture::Record);
    info.guest(app, cpu);
    bridge::end();
    if (cpu.terminate)
        return;
    const x86::CPU guest = cpu;
    draw::snapshot(s.spans, s.guestAfter);

    draw::restoreSpans(s.spans, s.before);
    cpu = entry;
    draw::g_stores.clear();
    s_logStores = true;
    bridge::begin(s.capture, bridge::Capture::Replay);
    info.native(app, cpu);
    bridge::end();
    s_logStores = false;
    const x86::CPU native = cpu;
    draw::snapshot(s.spans, s.nativeAfter);
    draw::restoreSpans(s.spans, s.guestAfter);
    cpu = guest;

    ++s.calls;
    char text[200] = {};
    const bool same = draw::sameCalls(s.capture, text, sizeof(text))
                      && draw::sameState(info.address, 0xff, guest, native, text, sizeof(text))
                      && draw::sameMemory(s.spans, s.guestAfter, s.nativeAfter, text, sizeof(text));
    draw::g_stores.clear();
    if (same)
        return;
    ++s.differing;
    if (s.logged < ShadowLogLimit)
    {
        ++s.logged;
        pf::logInfo("shadow_groups: %s call %u (caller 0x%08x): %s", info.name, unsigned(s.calls),
                    unsigned(rc::ld32(entry.esp)), text);
    }
}

void run(Index index, WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    const Info& info = s_functions[index];
    if (!s.native || bridge::capturing())
    {
        info.guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0) && s_depth == 0 && !draw::g_logStores)
    {
        ++s_depth;
        shadow(index, app, cpu);
        --s_depth;
        return;
    }
    info.native(app, cpu);
}

}

void lookup(WinApplication* app, x86::CPU& cpu, reg32 hash, reg32 sp, reg32& eax, reg32& ecx, reg32& edx)
{
    if (lookupNative(hash, eax, ecx, edx))
        return;
    rc::st32(sp + 4, hash);
    rc::st32(sp + 8, 1);
    rc::st32(sp + 0xc, 0);
    const x86::CPU saved = cpu;
    cpu.esp = sp;
    cpu.ecx = ecx;
    cpu.edx = edx;
    sub_5461c0(app, cpu);
    eax = cpu.eax;
    ecx = cpu.ecx;
    edx = cpu.edx;
    const bool terminate = cpu.terminate;
    cpu = saved;
    cpu.terminate = terminate;
}

void textureSpans(reg32 hash, std::vector<draw::Span>& spans)
{
    lookupSpans(hash, spans);
}

void call(WinApplication* app, x86::CPU& cpu, reg32 target)
{
    if (bridge::capturing() || !settings().native)
    {
        bridge::callGuest(app, cpu, target);
        return;
    }
    switch (target)
    {
    case Empty:
        cpu.esp += 4;
        return;
    case 0x40e740:
        run(StartHeadlights, app, cpu);
        return;
    case 0x40e7b0:
        run(StartProjected, app, cpu);
        return;
    case 0x40e900:
        run(StartCar, app, cpu);
        return;
    case 0x40eb30:
        run(StartOverlay, app, cpu);
        return;
    case 0x40eb60:
        run(StartPlain, app, cpu);
        return;
    default:
        bridge::callGuest(app, cpu, target);
        return;
    }
}

void logShadowTotal()
{
    const Settings& s = settings();
    if (s.native && s.shadow)
        pf::logInfo("shadow_groups: %u calls compared, %u differ", unsigned(s_shadow.calls),
                    unsigned(s_shadow.differing));
}

}

namespace native
{

void sub_40e740(win32::WinApplication* app, x86::CPU& cpu)
{
    groups::run(groups::StartHeadlights, app, cpu);
}

void sub_40e7b0(win32::WinApplication* app, x86::CPU& cpu)
{
    groups::run(groups::StartProjected, app, cpu);
}

void sub_40e900(win32::WinApplication* app, x86::CPU& cpu)
{
    groups::run(groups::StartCar, app, cpu);
}

void sub_40eb30(win32::WinApplication* app, x86::CPU& cpu)
{
    groups::run(groups::StartOverlay, app, cpu);
}

void sub_40eb60(win32::WinApplication* app, x86::CPU& cpu)
{
    groups::run(groups::StartPlain, app, cpu);
}

void sub_53f540(win32::WinApplication* app, x86::CPU& cpu)
{
    groups::run(groups::Resolve, app, cpu);
}

void sub_53f500(win32::WinApplication* app, x86::CPU& cpu)
{
    groups::run(groups::Restore, app, cpu);
}

}
}
