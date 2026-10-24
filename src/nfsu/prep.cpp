// Native render preparation (prep.h): the per-view StuffScenery loop. The bump allocation of the light contexts,
// the light-context fill (0x5437f0) of each instance into its own 0x1c0-byte context and the submission (0x40c910)
// run in the guest's order on the calling thread. Shadow mode compares the result against the guest body.

#include "prep.h"
#include "flares.h"
#include "draw.h"
#include "mathfn.h"

#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>

#include <cstdio>
#include <cstring>
#include <recomp.h>
#include <vector>

namespace nfsu
{

// Generated functions: the StuffScenery guest body (overridden), the light fill, the submission, the frame render.
void sub_446730_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5437f0(win32::WinApplication* app, x86::CPU& cpu);
void sub_40c910(win32::WinApplication* app, x86::CPU& cpu);
void sub_40a580_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace prep
{

namespace
{

using x86::reg8;
using x86::reg16;
using x86::reg32;
using x86::sreg32;
using x86::sreg16;

// Guest data.
const reg32 FrameCursor = 0x7355b4;     // per-frame allocator: cursor, end, overflow flag, bytes refused
const reg32 FrameEnd = 0x7355b8;
const reg32 FrameOverflow = 0x7355c4;
const reg32 FrameRefused = 0x7355c8;
const reg32 Model = 0x736140;           // the model slot the submission reads
const reg32 LightSize = 0x1c0;          // one instance light context
const reg32 ViewCount = 0x820;          // views in the culler object
const reg32 InfoSize = 0xd0;
const reg32 DefaultTexture = 0x78ea40;  // fill argument (guest 0x446833)
const uint32_t ReportFrames = 300;

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("prep_native", true), pf::Config::getBool("shadow_prep", false) };
        pf::logInfo("prep: render preparation %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// A visible entry that needs a light fill and the arguments the fill takes. The entry index pairs it with phase C.
struct Item
{
    size_t entry;       // index in the view's visible list (phase C pairs it back)
    reg32 light;        // the instance light record
    reg32 context;      // its 0x1c0 light context, from the bump allocator (fill argument 0)
    reg32 matrix;       // the instance matrix ([entry+4]); fill argument 1
    reg32 target;       // [view+0x40] + 0x40 (fill argument 3)
    reg32 view;         // the view (fill argument 4)
};

std::vector<Item> s_items;
std::vector<size_t> s_overflow;

// Runs the light fills of the items in order, each on a copy of the caller's state with the arguments below its stack.
void fillAll(win32::WinApplication* app, const x86::CPU& caller)
{
    for (const Item& item : s_items)
    {
        x86::CPU cpu = caller;
        cpu.esp = (caller.esp - 0x40) & ~15u;
        // The guest pushes, right to left (0x446828-0x446839): context, matrix, 0x78ea40, [view+0x40]+0x40, view.
        rc::st32(cpu.esp, 0x0044683f);              // return address slot
        rc::st32(cpu.esp + 4, item.context);
        rc::st32(cpu.esp + 8, item.matrix);
        rc::st32(cpu.esp + 12, DefaultTexture);
        rc::st32(cpu.esp + 16, item.target);
        rc::st32(cpu.esp + 20, item.view);
        sub_5437f0(app, cpu);
    }
}

// One light context from the per-frame bump allocator (guest 0x4467ee-0x44681d), 0 when the frame buffer is full.
reg32 allocate()
{
    const reg32 cursor = rc::ld32(FrameCursor);
    const reg32 next = cursor + LightSize;
    if (next < rc::ld32(FrameEnd))
    {
        rc::st32(FrameCursor, next);
        return cursor;
    }
    rc::st32(FrameOverflow, 1);
    rc::st32(FrameRefused, rc::ld32(FrameRefused) + LightSize);
    return 0;
}

// The call eViewPlatInterface::Render (guest 0x40c910 at 0x44685e). The guest pushes, right to left,
// light-or-extra, arg3, model, view, with eax = flags; the function reads eax, [esp+4] = view, [esp+8] = model.
// arg3 and the fourth slot are the matrix and the light in the light path, or [view+0x54] and the matrix (0) in the
// no-matrix path.
void submit(win32::WinApplication* app, x86::CPU& cpu, reg32 flags, reg32 view, reg32 model, reg32 slotC, reg32 slotD)
{
    x86::CPU c = cpu;
    const reg32 esp = (cpu.esp - 0x40) & ~15u;
    rc::st32(esp, 0x00446863);
    rc::st32(esp + 4, view);
    rc::st32(esp + 8, model);
    rc::st32(esp + 12, slotC);
    rc::st32(esp + 16, slotD);
    c.esp = esp;
    c.eax = flags;
    sub_40c910(app, c);
}

// StuffScenery for one view (cpu.ecx = the detail flag, cpu.edx = the culler object, [esp+4] = the view). Returns
// the culler info whose view matched, as the guest does.
void stuffNative(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 view = rc::ld32(cpu.esp + 4);
    const reg32 detail = cpu.ecx != 0 ? 0x1000u : 0;
    const reg32 object = cpu.edx;
    const sreg32 views = sreg32(rc::ld32(object + ViewCount));

    const x86::CPU entry = cpu;
    reg32 info = object;
    sreg32 v = 0;
    for (; v < views; ++v, info += InfoSize)
        if (rc::ld32(info + 0x80) == view)
            break;
    // No matching view: the guest returns eax = ecx = views, edx = info past the last view.
    if (v >= views)
    {
        cpu.eax = reg32(views);
        cpu.ecx = reg32(views);
        cpu.edx = info;
        cpu.ebx = entry.ebx;
        cpu.esi = entry.esi;
        cpu.ebp = entry.ebp;
        cpu.esp = entry.esp + 8;
        cpu.flags.pf = true;
        return;
    }

    const reg32 listStart = rc::ld32(info + 0x88), listEnd = rc::ld32(info + 0x8c);
    if (listStart >= listEnd)
    {
        cpu.eax = reg32(v);
        cpu.ecx = listStart;
        cpu.edx = info;
        cpu.ebx = entry.ebx;
        cpu.esi = entry.esi;
        cpu.ebp = entry.ebp;
        cpu.esp = entry.esp + 8;
        cpu.flags.pf = true;
        return;
    }
    s_items.clear();
    s_overflow.clear();
    // Phase A: allocate the light contexts in the guest's order.
    size_t index = 0;
    for (reg32 entry = listStart; entry < listEnd; entry += 12, ++index)
    {
        const reg32 model = rc::ld32(entry);        // model | state
        const reg32 matrix = rc::ld32(entry + 4);
        const reg32 modelBase = model & ~3u;
        if (matrix == 0)
            continue;
        const reg32 light = rc::ld32(modelBase + 0xc);
        if (light == 0 || (rc::ld8(light + 0xe) & 0x40) == 0)
            continue;
        const reg32 context = allocate();
        if (context == 0)
        {
            s_overflow.push_back(index);
            continue;
        }
        const reg32 target = rc::ld32(view + 0x40) + 0x40;
        s_items.push_back(Item{index, light, context, matrix, target, view});
    }

    // Phase B: the fills.
    fillAll(app, entry);

    // Phase C: the submission in the guest's order. Model slot and the instance's 0x2e field are set per entry as
    // the guest sets them.
    size_t next = 0, nextOver = 0;
    index = 0;
    for (reg32 entry = listStart; entry < listEnd; entry += 12, ++index)
    {
        const reg32 model = rc::ld32(entry);
        const reg32 matrix = rc::ld32(entry + 4);
        const reg32 instance = rc::ld32(entry + 8);
        const reg32 modelBase = model & ~3u;
        reg32 flags = reg32(sreg32(sreg16(rc::ld16(instance + 0x2e)))) | detail;
        if ((model & 3) == 2)
            flags |= 4;
        rc::st32(Model, modelBase);
        if (nextOver < s_overflow.size() && s_overflow[nextOver] == index)
        {
            ++nextOver;     // the guest skips the submit of an entry whose light context overflowed
        }
        else
        {
            reg32 slotC, slotD;
            if (matrix == 0)
            {
                slotC = 0x78bcc0u;
                slotD = rc::ld32(view + 0x54);
            }
            else if (next < s_items.size() && s_items[next].entry == index)
            {
                slotC = matrix;
                slotD = s_items[next].context;
                ++next;
            }
            else
            {
                slotC = matrix;
                slotD = 0;
            }
            submit(app, cpu, flags, view, modelBase, slotC, slotD);
        }
        rc::st16(instance + 0x2e, 0);
        rc::st32(Model, 0);
    }

    cpu.eax = info;
    cpu.ecx = listEnd;
    cpu.edx = listEnd;
    cpu.ebx = entry.ebx;
    cpu.esi = entry.esi;
    cpu.edi = entry.edi;
    cpu.ebp = entry.ebp;
    cpu.esp = entry.esp + 8;
    cpu.flags.pf = true;
}

// Shadow: the native path runs into the real light contexts, then the guest body runs from the entry state into a
// restored allocator and the difference is reported. The light arena the native path wrote is saved and compared.
struct Shadow
{
    uint32_t calls = 0, differing = 0, logged = 0;
};
Shadow s_shadow;
const uint32_t ShadowLogLimit = 20;

struct PerfSums
{
    uint64_t us = 0;
    uint32_t calls = 0, items = 0, maxUs = 0;
};
PerfSums s_perf;
uint32_t s_frames = 0;

}

void stuffScenery(win32::WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
    if (!s.native)
    {
        sub_446730_guest(app, cpu);
    }
    else if (s.shadow)
    {
        const x86::CPU entry = cpu;
        const reg32 cursorBefore = rc::ld32(FrameCursor);
        stuffNative(app, cpu);
        const reg32 cursorAfter = rc::ld32(FrameCursor);
        std::vector<reg8> nativeFill(cursorAfter - cursorBefore);
        if (!nativeFill.empty())
            memcpy(nativeFill.data(), rc::host(cursorBefore), nativeFill.size());
        const x86::CPU nativeCpu = cpu;
        rc::st32(FrameCursor, cursorBefore);
        rc::st32(FrameOverflow, 0);
        x86::CPU guest = entry;
        sub_446730_guest(app, guest);
        ++s_shadow.calls;
        char text[160] = {};
        bool same = guest.eax == nativeCpu.eax && guest.edx == nativeCpu.edx
                    && rc::ld32(FrameCursor) == cursorAfter;
        if (!same)
            snprintf(text, sizeof(text), "return eax 0x%08x/0x%08x edx 0x%08x/0x%08x cursor 0x%08x/0x%08x",
                     unsigned(nativeCpu.eax), unsigned(guest.eax), unsigned(nativeCpu.edx), unsigned(guest.edx),
                     unsigned(cursorAfter), unsigned(rc::ld32(FrameCursor)));
        for (reg32 i = 0; same && i < nativeFill.size(); ++i)
        {
            const reg8 g = rc::ld8(cursorBefore + i);
            if (g != nativeFill[i])
            {
                same = false;
                snprintf(text, sizeof(text), "light byte at 0x%08x guest 0x%02x native 0x%02x",
                         unsigned(cursorBefore + i), unsigned(g), unsigned(nativeFill[i]));
            }
        }
        if (!same)
        {
            ++s_shadow.differing;
            if (s_shadow.logged < ShadowLogLimit)
            {
                ++s_shadow.logged;
                pf::logInfo("shadow_prep: call %u: %s", unsigned(s_shadow.calls), text);
            }
        }
    }
    else
    {
        stuffNative(app, cpu);
    }
    if (pf::perf::enabled())
    {
        const uint32_t us = pf::perf::now() - start;
        s_perf.us += us;
        s_perf.calls++;
        s_perf.items += uint32_t(s_items.size());
        if (us > s_perf.maxUs)
            s_perf.maxUs = us;
    }
}

void frame(win32::WinApplication* app, x86::CPU& cpu)
{
    sub_40a580_guest(app, cpu);
    if (!pf::perf::enabled() || ++s_frames < ReportFrames)
        return;
    const Settings& s = settings();
    const mathfn::Usage math = mathfn::takeUsage();
    pf::logInfo("prep: %s light fills %u/frame, %.2f ms/frame (max %.2f); math helpers %u calls/frame, %.2f ms/frame",
                !s.native ? "guest" : s.shadow ? "shadow" : "native",
                s_perf.calls ? s_perf.items / s_perf.calls : 0,
                s_perf.calls ? double(s_perf.us) / 1000.0 / s_frames : 0.0, double(s_perf.maxUs) / 1000.0,
                math.calls / s_frames, math.ms / s_frames);
    if (s.native && s.shadow)
        pf::logInfo("shadow_prep: %u calls compared, %u differ", unsigned(s_shadow.calls), unsigned(s_shadow.differing));
    flares::report(s_frames);
    draw::logUsage(s_frames);
    s_perf = PerfSums();
    s_frames = 0;
}

}
}

namespace nfsu { namespace native
{

void sub_446730(win32::WinApplication* app, x86::CPU& cpu)
{
    prep::stuffScenery(app, cpu);
}

void sub_40a580(win32::WinApplication* app, x86::CPU& cpu)
{
    prep::frame(app, cpu);
}

}}
