// Native world views of the frame render (views.h): 0x409580 with its view begin 0x40abd0, the mirror car submit
// 0x571870 and the reflection scenery pass 0x4468a0 inline; every other function it calls runs through its own entry.

#include "views.h"

#include "draw.h"
#include "groups.h"

#include <winapi/d3d9_native.h>
#include <platform/config.h>
#include <platform/log.h>

#include <cstdio>
#include <cstring>
#include <native.h>
#include <vector>

namespace rc
{

NativeHook g_nativeHook = nullptr;

}

namespace nfsu
{

// The translated guest bodies (generated).
void sub_409580_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_570950_guest(win32::WinApplication* app, x86::CPU& cpu);
// Entries of the functions the views call (generated; overridden ones run their native code).
void sub_408f00(win32::WinApplication* app, x86::CPU& cpu);
void sub_446730(win32::WinApplication* app, x86::CPU& cpu);
void sub_570db0(win32::WinApplication* app, x86::CPU& cpu);
void sub_570950(win32::WinApplication* app, x86::CPU& cpu);
void sub_4ae650(win32::WinApplication* app, x86::CPU& cpu);
void sub_40ed50(win32::WinApplication* app, x86::CPU& cpu);
void sub_40e6d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_544f30(win32::WinApplication* app, x86::CPU& cpu);
void sub_570d00(win32::WinApplication* app, x86::CPU& cpu);
void sub_570c70(win32::WinApplication* app, x86::CPU& cpu);
void sub_4ae7d0(win32::WinApplication* app, x86::CPU& cpu);
void sub_571050(win32::WinApplication* app, x86::CPU& cpu);
void sub_404110(win32::WinApplication* app, x86::CPU& cpu);
void sub_409180(win32::WinApplication* app, x86::CPU& cpu);
void sub_408b20(win32::WinApplication* app, x86::CPU& cpu);
void sub_4090a0(win32::WinApplication* app, x86::CPU& cpu);
void sub_58a240(win32::WinApplication* app, x86::CPU& cpu);
void sub_592100(win32::WinApplication* app, x86::CPU& cpu);
void sub_40cd40(win32::WinApplication* app, x86::CPU& cpu);
void sub_409b70(win32::WinApplication* app, x86::CPU& cpu);
void sub_40c910(win32::WinApplication* app, x86::CPU& cpu);

namespace views
{

namespace
{

using win32::DWORD;
using win32::WinApplication;
using x86::reg8;
using x86::reg32;
using x86::sreg32;
namespace bridge = win32::d3d9::bridge;

typedef void (*Function)(WinApplication*, x86::CPU&);

// Guest data.
const reg32 DevicePointer = 0x73636c;
const reg32 CurrentGroup = 0x73640c;
const reg32 CurrentView = 0x736458;
const reg32 Groups = 0x71abb8;          // 7 effect groups of 0xe0 bytes: vertex declaration +4, effect +0x18,
const reg32 GroupSize = 0xe0;           // start function +0xd8, end function +0xdc
const reg32 GroupCount = 7;
const reg32 ClearFlag = 0x6d3a00;       // the 3D pass clear test's copy of [0x73645c] (phases.cpp)

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
        const Settings r = { pf::Config::getBool("views_native", true), pf::Config::getBool("shadow_views", false) };
        pf::logInfo("views: world views 0x409580 %s%s", r.native ? "native" : "guest",
                    r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// Stores of the native code; the shadow compare logs their addresses during its native run.
bool s_logStores = false;

inline reg32 ld(reg32 address)
{
    return rc::ld32(address);
}

inline void store(reg32 address, reg32 value)
{
    if (__builtin_expect(s_logStores, 0))
        draw::g_stores.push_back(address);
    rc::st32(address, value);
}

inline void store16(reg32 address, x86::reg16 value)
{
    if (__builtin_expect(s_logStores, 0))
        draw::g_stores.push_back(address & ~3u);
    rc::st16(address, value);
}

inline void store8(reg32 address, reg8 value)
{
    if (__builtin_expect(s_logStores, 0))
        draw::g_stores.push_back(address & ~3u);
    rc::st8(address, value);
}

inline void push(x86::CPU& cpu, reg32 value)
{
    cpu.esp -= 4;
    rc::st32(cpu.esp, value);
}

// Bytes of stack arguments a function the views call removes at its return.
reg32 popsOf(Function f)
{
    if (f == &sub_446730)
        return 4;
    if (f == &sub_4ae650 || f == &sub_4ae7d0 || f == &sub_404110)
        return 8;
    if (f == &sub_40c910)
        return 0x14;
    return 0;
}

// A call instruction: the return address pushed, then the function's entry; esp and the callee-saved registers after
// it as the x86 calling convention leaves them (translated guest code stores at its returns only what its callers
// read). False when the guest thread ends.
inline bool call(WinApplication* app, x86::CPU& cpu, reg32 back, Function f)
{
    const reg32 esp = cpu.esp, ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;
    push(cpu, back);
    f(app, cpu);
    if (cpu.terminate)
        return false;
    cpu.esp = esp + popsOf(f);
    cpu.ebx = ebx;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.edi = edi;
    return true;
}

// A call through an effect group table slot (no arguments), as call.
inline bool groupCall(WinApplication* app, x86::CPU& cpu, reg32 back, reg32 target)
{
    const reg32 esp = cpu.esp, ebx = cpu.ebx, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;
    push(cpu, back);
    groups::call(app, cpu, target);
    if (cpu.terminate)
        return false;
    cpu.esp = esp;
    cpu.ebx = ebx;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.edi = edi;
    return true;
}

inline void loadByte(x86::CPU& cpu, reg32 address)
{
    cpu.eax = (cpu.eax & ~0xffu) | rc::ld8(address);
}

// SetRenderState as the guest calls it: the device's vtable in ecx (or edx), the result in eax.
inline void renderState(WinApplication* app, x86::CPU& cpu, DWORD state, DWORD value, bool edx)
{
    const reg32 device = ld(DevicePointer);
    (edx ? cpu.edx : cpu.ecx) = ld(device);
    cpu.eax = bridge::setRenderState(app, cpu, device, state, value);
}

// ---- 0x40abd0: view begin ----

// esi = the view (render target +8, depth surface +0xc, width +0x14, height +0x18), [esp + 4] = clear: the queue
// flushed, the view's targets, a clear when asked, its viewport; the view becomes current with no effect group.
bool viewBegin(WinApplication* app, x86::CPU& cpu, reg32 clear)
{
    push(cpu, clear);
    push(cpu, 0);
    const reg32 entry = cpu.esp;
    const reg32 l = entry - 0x18;
    cpu.esp = l;
    if (!call(app, cpu, 0x0040abd8, &sub_40ed50))
        return false;
    const reg32 view = cpu.esi;
    reg32 device = ld(DevicePointer);
    cpu.edx = ld(view + 8);
    cpu.ecx = ld(device);
    cpu.eax = bridge::setRenderTarget(app, cpu, device, 0, cpu.edx);
    device = ld(DevicePointer);
    cpu.edx = ld(view + 0xc);
    cpu.ecx = ld(device);
    cpu.eax = bridge::setDepthStencilSurface(app, cpu, device, cpu.edx);
    cpu.eax = ld(entry + 4);
    if (cpu.eax != 0)
    {
        device = ld(DevicePointer);
        cpu.ecx = ld(device);
        cpu.eax = bridge::clear(app, cpu, device, 0, nullptr, 7, 0xff000000u, 1.0f, 0);
    }
    cpu.edx = ld(view + 0x14);
    cpu.eax = ld(view + 0x18);
    rc::st32(l + 8, cpu.edx);
    rc::st32(l + 0xc, cpu.eax);
    device = ld(DevicePointer);
    rc::st32(l, 0);
    rc::st32(l + 4, 0);
    rc::st32(l + 0x10, 0);
    rc::st32(l + 0x14, 0x3f800000u);
    cpu.edx = l;
    cpu.ecx = ld(device);
    DWORD viewport[6];
    memcpy(viewport, rc::host(l), sizeof(viewport));
    cpu.eax = bridge::setViewport(app, cpu, device, viewport);
    store(CurrentView, view);
    store(CurrentGroup, 0);
    cpu.esp = entry + 4;
    // The caller's add esp, 4 of the clear flag.
    cpu.esp += 4;
    return true;
}

// ---- 0x571870: the car model of a view's mirror or a player view ----

// ecx = the view: views 1-3 submit model 0x73c1c8 when [0x6eef60] is set, views 10-15 model 0x73c1b0 when
// [0x6f1a78] is set.
bool mirrorCar(WinApplication* app, x86::CPU& cpu, reg32 back)
{
    push(cpu, back);
    const reg32 entry = cpu.esp;
    const reg32 view = cpu.ecx;
    cpu.eax = ld(view + 4);
    const sreg32 id = sreg32(cpu.eax);
    reg32 flag = 0, model = 0, world = 0, after = 0;
    if (id >= 1 && id <= 3)
    {
        flag = 0x6eef60;
        model = 0x73c1c8;
        world = 0x73c220;
        after = 0x0057189c;
    }
    else if (id >= 0xa && id <= 0xf)
    {
        flag = 0x6f1a78;
        model = 0x73c1b0;
        world = 0x73c1e0;
        after = 0x005718c6;
    }
    if (flag != 0)
    {
        cpu.eax = ld(flag);
        if (cpu.eax != 0)
        {
            push(cpu, 0);
            push(cpu, 0);
            push(cpu, world);
            push(cpu, model);
            cpu.eax = 0;
            push(cpu, view);
            if (!call(app, cpu, after, &sub_40c910))
                return false;
        }
    }
    cpu.esp = entry + 4;
    return true;
}

// ---- 0x4468a0: instances of a view's scenery pass ----

// eax = the cull info (0xd0-byte entries from +0, their count at +0x820), edx = the view, [esp + 4] = the
// callback: the callback for each instance (0xc bytes) of the view's entry, from +0x88 to +0x8c. The callback
// 0x409090 sets the instance's flags word +0x2e to 0x200.
bool instances(WinApplication* app, x86::CPU& cpu, reg32 callback)
{
    push(cpu, callback);
    push(cpu, 0x004098d8);
    const reg32 entry = cpu.esp;
    reg32 info = cpu.eax;
    const reg32 view = cpu.edx;
    cpu.ecx = ld(info + 0x820);
    cpu.eax = 0;
    if (sreg32(cpu.ecx) > 0)
    {
        bool found = false;
        do
        {
            if (ld(info + 0x80) == view)
            {
                found = true;
                break;
            }
            ++cpu.eax;
            info += 0xd0;
        } while (sreg32(cpu.eax) < sreg32(cpu.ecx));
        if (found)
        {
            reg32 item = ld(info + 0x88);
            if (item < ld(info + 0x8c))
            {
                do
                {
                    if (callback == 0x409090)
                    {
                        cpu.eax = item;
                        cpu.ecx = ld(item + 8);
                        store16(cpu.ecx + 0x2e, 0x200);
                    }
                    else
                    {
                        const reg32 esi = cpu.esi, edi = cpu.edi, ebx = cpu.ebx;
                        cpu.esi = item;
                        cpu.edi = info;
                        cpu.ebx = callback;
                        cpu.esp = entry - 0x14;
                        rc::st32(cpu.esp + 4, item);
                        rc::st32(cpu.esp, 0x004468e3);
                        bridge::callGuest(app, cpu, callback);
                        if (cpu.terminate)
                            return false;
                        cpu.esi = esi;
                        cpu.edi = edi;
                        cpu.ebx = ebx;
                    }
                    cpu.eax = ld(info + 0x8c);
                    item += 0xc;
                } while (item < cpu.eax);
            }
        }
    }
    cpu.esp = entry + 8;
    // The caller's push of the callback is popped by ret 4.
    return true;
}

// ---- 0x409580 ----

// The inline effect group switch to group 0 with its first pass; back: the return addresses of the end and start
// function calls; begin: the guest address of Begin's pass count.
bool groupZero(WinApplication* app, x86::CPU& cpu, reg32 endBack, reg32 startBack, reg32 passes)
{
    cpu.eax = ld(CurrentGroup);
    cpu.esi = Groups;
    if (cpu.eax == cpu.esi)
        return true;
    if (cpu.eax != 0)
    {
        const reg32 effect = ld(cpu.eax + 0x18);
        cpu.eax = effect;
        cpu.edx = ld(effect);
        cpu.eax = bridge::effectEnd(app, cpu, effect);
        cpu.eax = ld(CurrentGroup);
        if (!groupCall(app, cpu, endBack, ld(cpu.eax + 0xdc)))
            return false;
    }
    store(CurrentGroup, cpu.esi);
    if (!groupCall(app, cpu, startBack, ld(Groups + 0xd8)))
        return false;
    cpu.edx = ld(Groups + 4);
    reg32 device = ld(DevicePointer);
    cpu.eax = device;
    cpu.ecx = ld(device);
    cpu.eax = bridge::setVertexDeclaration(app, cpu, device, cpu.edx);
    reg32 effect = ld(Groups + 0x18);
    cpu.ecx = ld(effect);
    cpu.edx = passes;
    cpu.eax = bridge::effectBegin(app, cpu, effect, reinterpret_cast<win32::UINT*>(rc::host(passes)), 0);
    effect = ld(Groups + 0x18);
    cpu.ecx = ld(effect);
    cpu.eax = bridge::effectPass(app, cpu, effect, 0);
    return true;
}

// The two world light flare and mini world flare draws of a view with car and FE flares (0x409b0f-0x409b5c): z
// writes off around them.
bool flares(WinApplication* app, x86::CPU& cpu, reg32 view)
{
    renderState(app, cpu, 0xe, 0, true);
    push(cpu, 0);
    push(cpu, view);
    if (!call(app, cpu, 0x00409b29, &sub_544f30))
        return false;
    cpu.esp += 8;
    push(cpu, view);
    push(cpu, 0x746dd0);
    if (!call(app, cpu, 0x00409b37, &sub_4ae7d0))
        return false;
    push(cpu, 0);
    cpu.eax = view;
    if (!call(app, cpu, 0x00409b40, &sub_570d00))
        return false;
    push(cpu, 0);
    if (!call(app, cpu, 0x00409b47, &sub_570c70))
        return false;
    cpu.esp += 8;
    renderState(app, cpu, 0xe, 1, false);
    return true;
}

// Particle systems 0x78e980 and 0x78e9e0 of a view.
bool particles(WinApplication* app, x86::CPU& cpu, reg32 view, reg32 first, reg32 second)
{
    push(cpu, view);
    push(cpu, 0x78e980);
    if (!call(app, cpu, first, &sub_404110))
        return false;
    push(cpu, view);
    push(cpu, 0x78e9e0);
    return call(app, cpu, second, &sub_404110);
}

// The player view (view 1, 0x7404c0) and the pass views before it: env map faces 0x7405e0, the reflection views
// 0x740820-0x740a00, the rear view mirror 0x740580.
bool viewsNative(WinApplication* app, x86::CPU& cpu)
{
    const reg32 entry = cpu.esp;
    rc::st32(entry - 4, cpu.ebp);
    cpu.ebp = entry - 4;
    const reg32 f = ((entry - 4) & ~15u) - 0x844;
    rc::st32(f - 4, cpu.ebx);
    rc::st32(f - 8, cpu.esi);
    rc::st32(f - 12, cpu.edi);
    const reg32 s = f - 12;
    cpu.esp = s;
    const reg32 info = s + 0x20;
    cpu.edi = 0;
    cpu.eax = info;
    store8(0x71db20, 0);
    store(0x71db24, cpu.edi);
    rc::st32(s + 0x840, cpu.edi);
    if (!call(app, cpu, 0x004095ae, &sub_408f00))
        return false;

    // Env map faces.
    bool done = false;
    if (ld(0x700e90) != cpu.edi)
    {
        loadByte(cpu, 0x7405e8);
        cpu.esi = ld(0x740638);
        if ((cpu.eax & 0xff) == 0 || ld(cpu.esi + 0x10) == cpu.edi)
            done = true;
        else
        {
            if (!viewBegin(app, cpu, 1))
                return false;
            cpu.eax = ld(0x700e90);
            if (sreg32(cpu.eax) > 1)
            {
                push(cpu, 0x7405e0);
                cpu.ecx = 0;
                cpu.edx = info;
                if (!call(app, cpu, 0x004095fa, &sub_446730))
                    return false;
                cpu.esi = 0x7405e0;
                if (!call(app, cpu, 0x00409604, &sub_570db0))
                    return false;
                push(cpu, 1);
                push(cpu, 1);
                push(cpu, 1);
                push(cpu, cpu.edi);
                push(cpu, cpu.esi);
                if (!call(app, cpu, 0x00409611, &sub_570950))
                    return false;
                cpu.esp += 0x14;
            }
            push(cpu, 0x7405e0);
            push(cpu, 0x746dd0);
            if (!call(app, cpu, 0x00409623, &sub_4ae650) || !call(app, cpu, 0x00409628, &sub_40ed50))
                return false;
            cpu.edi = 0;
            cpu.eax = Groups;
            if (!call(app, cpu, 0x00409634, &sub_40e6d0))
                return false;
            cpu.eax = ld(0x700ea0);
            if (cpu.eax != 0)
            {
                push(cpu, 2);
                push(cpu, 0x7405e0);
                if (!call(app, cpu, 0x00409649, &sub_544f30))
                    return false;
                push(cpu, 1);
                cpu.eax = 0x7405e0;
                if (!call(app, cpu, 0x00409655, &sub_570d00))
                    return false;
                push(cpu, 1);
                cpu.edi = 0x7405e0;
                if (!call(app, cpu, 0x00409661, &sub_570c70))
                    return false;
                cpu.esp += 0x10;
                push(cpu, cpu.edi);
                push(cpu, 0x746dd0);
                if (!call(app, cpu, 0x0040966f, &sub_4ae7d0))
                    return false;
            }
        }
    }
    if (done)
    {
        cpu.esp = s;
        return true;
    }

    // Reflection views.
    cpu.eax = ld(0x700e74);
    if (cpu.eax != 0)
    {
        cpu.ebx = 0x740820;
        do
        {
            loadByte(cpu, cpu.ebx + 8);
            if ((cpu.eax & 0xff) == 0)
                continue;
            cpu.esi = ld(cpu.ebx + 0x58);
            cpu.eax = ld(cpu.esi + 0x10);
            if (cpu.eax == 0)
                continue;
            if (!viewBegin(app, cpu, 1))
                return false;
            renderState(app, cpu, 7, 0, false);
            renderState(app, cpu, 0xe, 0, true);
            cpu.ecx = cpu.ebx;
            if (!mirrorCar(app, cpu, 0x004096cf))
                return false;
            renderState(app, cpu, 0xe, 1, false);
            renderState(app, cpu, 7, 1, true);
            cpu.edi = 0;
            cpu.eax = Groups;
            if (!call(app, cpu, 0x004096ff, &sub_40e6d0) || !call(app, cpu, 0x00409704, &sub_40ed50))
                return false;
            cpu.eax = ld(0x700e78);
            if (cpu.eax != 0)
            {
                push(cpu, cpu.ebx);
                cpu.ecx = 0;
                cpu.edx = info;
                if (!call(app, cpu, 0x00409719, &sub_446730))
                    return false;
            }
            push(cpu, cpu.ebx);
            push(cpu, 0x746dd0);
            if (!call(app, cpu, 0x00409724, &sub_4ae650) || !call(app, cpu, 0x00409729, &sub_40ed50))
                return false;
            cpu.edi = 0;
            cpu.eax = Groups;
            if (!call(app, cpu, 0x00409735, &sub_40e6d0))
                return false;
            cpu.eax = ld(0x700ea0);
            if (cpu.eax != 0)
            {
                push(cpu, 1);
                push(cpu, cpu.ebx);
                if (!call(app, cpu, 0x00409746, &sub_544f30))
                    return false;
                cpu.esp += 8;
                push(cpu, cpu.ebx);
                push(cpu, 0x746dd0);
                if (!call(app, cpu, 0x00409754, &sub_4ae7d0))
                    return false;
            }
        } while (cpu.ebx += 0x60, sreg32(cpu.ebx) <= sreg32(0x740a00));
    }
    if (!call(app, cpu, 0x00409768, &sub_40ed50))
        return false;

    // Rear view mirror.
    loadByte(cpu, 0x740588);
    if ((cpu.eax & 0xff) != 0)
    {
        cpu.esi = 0x71aaa0;
        if (!viewBegin(app, cpu, 1))
            return false;
        push(cpu, 0x740580);
        cpu.ecx = 1;
        cpu.edx = info;
        if (!call(app, cpu, 0x00409797, &sub_446730))
            return false;
        push(cpu, 0x740580);
        if (!call(app, cpu, 0x004097a1, &sub_571050))
            return false;
        cpu.esp += 4;
        if (!call(app, cpu, 0x004097a9, &sub_40ed50))
            return false;
        cpu.eax = ld(0x700ea8);
        if (cpu.eax != 0 && !particles(app, cpu, 0x740580, 0x004097c1, 0x004097d0))
            return false;
        cpu.eax = ld(0x700ea0);
        if (cpu.eax != 0)
        {
            push(cpu, 0);
            cpu.eax = 0x740580;
            if (!call(app, cpu, 0x004097e5, &sub_570d00))
                return false;
            cpu.eax = ld(0x700ea0);
            cpu.esp += 4;
            if (cpu.eax != 0)
            {
                push(cpu, 0);
                push(cpu, 0x740580);
                if (!call(app, cpu, 0x004097fd, &sub_544f30))
                    return false;
                cpu.esp += 8;
            }
        }
        cpu.eax = ld(0x7364ac);
        if (cpu.eax != 0)
        {
            cpu.edi = 0;
            cpu.eax = Groups;
            if (!call(app, cpu, 0x00409815, &sub_40e6d0))
                return false;
            cpu.eax = ld(0x7364ac);
            cpu.ecx = ld(cpu.eax);
            cpu.edx = ld(cpu.ecx + 0x18);
            push(cpu, 6);
            push(cpu, 5);
            push(cpu, cpu.edx);
            cpu.eax = 0xffffffffu;
            if (!call(app, cpu, 0x0040982c, &sub_409180))
                return false;
            cpu.esp += 0xc;
        }
        cpu.eax = ld(0x7364b0);
        if (cpu.eax != 0)
        {
            cpu.esi = ld(cpu.eax);
            renderState(app, cpu, 0xa8, 8, false);
            cpu.edx = ld(cpu.esi + 0x18);
            push(cpu, 1);
            push(cpu, 2);
            push(cpu, cpu.edx);
            cpu.eax = 0xffffffffu;
            if (!call(app, cpu, 0x0040985f, &sub_409180))
                return false;
            cpu.esp += 0xc;
        }
        if (!call(app, cpu, 0x00409867, &sub_408b20))
            return false;
    }
    if (!call(app, cpu, 0x0040986c, &sub_40ed50))
        return false;

    // The player view.
    cpu.ebx = 1;
    do
    {
        cpu.edi = cpu.ebx * 0x60 + 0x740460;
        loadByte(cpu, cpu.edi + 8);
        if ((cpu.eax & 0xff) == 0)
            continue;
        cpu.eax = ld(cpu.edi + 0x58);
        cpu.ecx = ld(cpu.eax + 0x10);
        if (cpu.ecx == 0)
            continue;
        cpu.esi = 0x71a05c;
        cpu.eax = cpu.edi;
        if (!call(app, cpu, 0x004098a2, &sub_4090a0))
            return false;
        const reg32 clear = ld(ClearFlag) == 1 ? 1 : 0;
        cpu.esi = ld(cpu.edi + 0x58);
        if (!viewBegin(app, cpu, clear))
            return false;
        cpu.eax = ld(0x700e80);
        if (cpu.eax != 0)
        {
            cpu.edx = 0x740760;
            cpu.eax = info;
            if (!instances(app, cpu, 0x409090))
                return false;
        }
        cpu.eax = ld(0x77a920);
        bool car = false;
        if (cpu.eax == 6)
        {
            cpu.ecx = ld(0x7361f0);
            car = ld(cpu.ecx) != 1;
        }
        if (!car)
        {
            cpu.ecx = ld(0x6fb038);
            car = cpu.ecx != 0 && cpu.eax == 3;
        }
        if (car)
        {
            cpu.ecx = cpu.edi;
            if (!mirrorCar(app, cpu, 0x00409903))
                return false;
        }
        if (!groupZero(app, cpu, 0x0040992c, 0x00409938, s + 0x1c))
            return false;
        if (!call(app, cpu, 0x00409977, &sub_40ed50))
            return false;
        push(cpu, cpu.edi);
        cpu.ecx = 0;
        cpu.edx = info;
        if (!call(app, cpu, 0x00409983, &sub_446730) || !call(app, cpu, 0x00409988, &sub_40ed50))
            return false;
        cpu.eax = cpu.edi;
        if (!call(app, cpu, 0x0040998f, &sub_58a240))
            return false;
        push(cpu, 0);
        push(cpu, cpu.edi);
        if (!call(app, cpu, 0x00409997, &sub_592100))
            return false;
        cpu.eax = ld(0x736384);
        cpu.esp += 8;
        if (cpu.eax != 0 && ld(0x73645c) == 1)
        {
            if (!call(app, cpu, 0x004099b1, &sub_40ed50))
                return false;
            cpu.eax = ld(0x700e98);
            if (cpu.eax != 0 && !call(app, cpu, 0x004099bf, &sub_40cd40))
                return false;
            if (!call(app, cpu, 0x004099c4, &sub_408b20))
                return false;
        }
        push(cpu, cpu.edi);
        if (!call(app, cpu, 0x004099ca, &sub_571050))
            return false;
        push(cpu, 1);
        push(cpu, 1);
        push(cpu, 0);
        push(cpu, 1);
        push(cpu, cpu.edi);
        if (!call(app, cpu, 0x004099d8, &sub_570950))
            return false;
        cpu.esp += 0x18;
        push(cpu, cpu.edi);
        push(cpu, 0x746dd0);
        if (!call(app, cpu, 0x004099e6, &sub_4ae650))
            return false;
        if (cpu.ebx == 1 && !groupZero(app, cpu, 0x00409a0f, 0x00409a1b, s + 0x14))
            return false;
        if (!call(app, cpu, 0x00409a5a, &sub_40ed50))
            return false;
        cpu.eax = ld(0x700ea8);
        store(0x7187b4, 0);
        if (cpu.eax != 0 && !particles(app, cpu, cpu.edi, 0x00409a78, 0x00409a83))
            return false;
        if (!groupZero(app, cpu, 0x00409aa7, 0x00409ab3, s + 0x18))
            return false;
        cpu.eax = ld(0x700ea0);
        if (cpu.eax == 0)
            continue;
        cpu.eax = ld(0x700e9c);
        if (cpu.eax != 0 && ld(0x73645c) == 1)
        {
            if (!call(app, cpu, 0x00409b0d, &sub_409b70))
                return false;
            continue;
        }
        if (!flares(app, cpu, cpu.edi))
            return false;
    } while (++cpu.ebx, sreg32(cpu.ebx) <= 1);
    cpu.esp = s;
    return true;
}

// The return of 0x409580 from esp = its frame's register saves.
void leave(x86::CPU& cpu)
{
    cpu.edi = ld(cpu.esp);
    cpu.esi = ld(cpu.esp + 4);
    cpu.ebx = ld(cpu.esp + 8);
    cpu.esp = cpu.ebp;
    cpu.ebp = ld(cpu.esp);
    cpu.esp += 8;
}

void viewsEntry(WinApplication* app, x86::CPU& cpu)
{
    if (viewsNative(app, cpu))
        leave(cpu);
}

// ---- shadow compare ----

// Stack arguments of the functions the views call, compared with the calls.
uint32_t argumentsOf(reg32 address)
{
    switch (address)
    {
    case 0x446730: case 0x570d00: case 0x570c70: case 0x571050:
        return 1;
    case 0x4ae650: case 0x544f30: case 0x4ae7d0: case 0x404110: case 0x592100:
        return 2;
    case 0x409180:
        return 3;
    case 0x40c910: case 0x570950:
        return 5;
    default:
        return 0;
    }
}

// The caller-saved registers the functions the views call read (eax 1, ecx 2, edx 4); the translated guest code
// leaves the others as they were at its calls. The callee-saved ones and esp are always compared.
const uint8_t SavedRegisters = 0xf8;

uint8_t registersOf(reg32 address)
{
    switch (address)
    {
    case 0x408f00: case 0x40e6d0: case 0x570d00: case 0x409180: case 0x58a240: case 0x4090a0: case 0x40c910:
        return 1;
    case 0x446730:
        return 2 | 4;
    default:
        return 0;
    }
}

struct Shadow
{
    bridge::Capture capture;
    std::vector<draw::Span> spans;
    std::vector<reg8> before, guestAfter, nativeAfter;
    std::vector<reg32> groups;      // the shared words after each recorded call
    uint32_t next = 0;
    bool replaying = false;
    reg32 fs = 0;
    uint32_t depth = 0;
    uint32_t calls = 0, differing = 0, logged = 0;
};

Shadow s_shadow;

// The call hook of the shadow's runs: a function entered from the views (not from inside a function they called)
// is one recorded call, run once by the guest run and replayed in the native run with the shared words it left.
void hook(reg32 address, rc::NativeFunction run, WinApplication* app, x86::CPU& cpu)
{
    Shadow& s = s_shadow;
    if (s.depth != 0 || cpu.efs != s.fs || !bridge::capturingCalls())
    {
        run(app, cpu);
        return;
    }
    ++s.depth;
    bridge::callNative(app, cpu, address, run, argumentsOf(address), registersOf(address));
    --s.depth;
    // The words the views and the functions they call both store to, as the call left them.
    static const reg32 Shared[] = { 0x71db20, 0x71db24, 0x7187b4, CurrentGroup, CurrentView };
    const uint32_t count = sizeof(Shared) / sizeof(Shared[0]);
    if (!s.replaying)
    {
        for (const reg32 address : Shared)
            s.groups.push_back(ld(address));
        ++s.next;
    }
    else if (s.next < s.groups.size() / count)
    {
        for (uint32_t i = 0; i < count; ++i)
            rc::st32(Shared[i], s.groups[s.next * count + i]);
        ++s.next;
    }
}

// Memory the views themselves store to: the flags at 0x71db20, 0x7187b4, the current group and view, and the
// instance flags the reflection scenery pass sets.
void spansOf(const x86::CPU& cpu, std::vector<draw::Span>& spans)
{
    spans.clear();
    spans.push_back(draw::Span{0x71db20, 8});
    spans.push_back(draw::Span{0x7187b4, 4});
    spans.push_back(draw::Span{CurrentGroup, 4});
    spans.push_back(draw::Span{CurrentView, 4});
    (void)cpu;
}

// Instance flag words of the reflection pass, known after the cull (the guest run): added to the spans.
void instanceSpans(reg32 info, std::vector<draw::Span>& spans)
{
    const sreg32 count = sreg32(ld(info + 0x820));
    for (sreg32 i = 0; i < count; ++i)
    {
        const reg32 entry = info + 0xd0 * reg32(i);
        if (ld(entry + 0x80) != 0x740760)
            continue;
        for (reg32 item = ld(entry + 0x88); item < ld(entry + 0x8c); item += 0xc)
            spans.push_back(draw::Span{(ld(item + 8) + 0x2e) & ~3u, 4});
        break;
    }
}

void shadow(WinApplication* app, x86::CPU& cpu)
{
    Shadow& s = s_shadow;
    const x86::CPU entry = cpu;
    spansOf(entry, s.spans);
    draw::snapshot(s.spans, s.before);

    // The group functions called through the group table go through the callouts while the guest code runs.
    reg32 slots[2 * GroupCount], targets[2 * GroupCount];
    for (reg32 k = 0; k < GroupCount; ++k)
    {
        slots[2 * k] = Groups + k * GroupSize + 0xd8;
        slots[2 * k + 1] = Groups + k * GroupSize + 0xdc;
    }
    for (uint32_t i = 0; i < 2 * GroupCount; ++i)
    {
        targets[i] = ld(slots[i]);
        bridge::setCalloutTarget(i, targets[i]);
        rc::st32(slots[i], bridge::calloutAddress(i));
    }
    s.capture.clear();
    s.groups.clear();
    s.next = 0;
    s.replaying = false;
    s.fs = cpu.efs;
    s.depth = 0;
    bridge::setCallRegisters(SavedRegisters);
    bridge::begin(s.capture, bridge::Capture::Record);
    rc::g_nativeHook = &hook;
    sub_409580_guest(app, cpu);
    rc::g_nativeHook = nullptr;
    bridge::end();
    for (uint32_t i = 0; i < 2 * GroupCount; ++i)
        rc::st32(slots[i], targets[i]);
    if (cpu.terminate)
    {
        bridge::setCallRegisters(0xff);
        return;
    }
    const x86::CPU guest = cpu;
    // The cull info is in the views' stack frame at the same place in both runs.
    const reg32 info = (((entry.esp - 4) & ~15u) - 0x844 - 12) + 0x20;
    const size_t base = s.spans.size();
    instanceSpans(info, s.spans);
    draw::snapshot(s.spans, s.guestAfter);
    // The instance flag words are known only after the guest run's cull: the native run starts from the values the
    // guest left there.
    draw::restoreSpans(std::vector<draw::Span>(s.spans.begin(), s.spans.begin() + base), s.before);

    cpu = entry;
    const uint32_t recorded = s.next;
    s.next = 0;
    draw::g_stores.clear();
    s_logStores = true;
    s.replaying = true;
    bridge::begin(s.capture, bridge::Capture::Replay);
    rc::g_nativeHook = &hook;
    viewsEntry(app, cpu);
    rc::g_nativeHook = nullptr;
    bridge::end();
    bridge::setCallRegisters(0xff);
    s_logStores = false;
    s.replaying = false;
    const x86::CPU native = cpu;
    draw::snapshot(s.spans, s.nativeAfter);
    draw::restoreSpans(s.spans, s.guestAfter);
    cpu = guest;

    ++s.calls;
    char text[200] = {};
    bool same = draw::sameCalls(s.capture, text, sizeof(text));
    if (same && s.next != recorded)
    {
        same = false;
        snprintf(text, sizeof(text), "calls guest %u native %u", unsigned(recorded), unsigned(s.next));
    }
    same = same && draw::sameState(0x409580, 0xff, guest, native, text, sizeof(text))
           && draw::sameMemory(s.spans, s.guestAfter, s.nativeAfter, text, sizeof(text));
    draw::g_stores.clear();
    if (same)
        return;
    ++s.differing;
    if (s.logged < ShadowLogLimit)
    {
        ++s.logged;
        const bridge::Capture& c = s.capture;
        reg32 address = 0;
        if (c.differs && c.differingCall < c.calls.size() && c.calls[c.differingCall].method == bridge::MethodGuestCall)
            address = c.words[c.calls[c.differingCall].first];
        pf::logInfo("shadow_views: call %u: %s (guest call to 0x%08x, views esp 0x%08x)", unsigned(s.calls), text,
                    unsigned(address), unsigned(entry.esp));
    }
}

}

void run(WinApplication* app, x86::CPU& cpu)
{
    const Settings& s = settings();
    if (!s.native || bridge::capturing())
    {
        sub_409580_guest(app, cpu);
        return;
    }
    if (__builtin_expect(s.shadow, 0) && !draw::g_logStores)
    {
        shadow(app, cpu);
        return;
    }
    viewsEntry(app, cpu);
}

void logShadowTotal()
{
    const Settings& s = settings();
    if (s.native && s.shadow)
        pf::logInfo("shadow_views: %u calls compared, %u differ", unsigned(s_shadow.calls),
                    unsigned(s_shadow.differing));
}

}

}
