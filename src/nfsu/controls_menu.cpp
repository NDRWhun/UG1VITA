#include "controls_menu.h"

#include <lib/controls.h>
#include <lib/guest.h>
#include <platform/config.h>
#include <platform/log.h>
#include <cstring>

namespace nfsu { namespace controls_menu {
namespace {
using x86::reg32;
reg32 s_title = 0, s_error = 0, s_callback = 0;

reg32 get(reg32 a) { reg32 v; memcpy(&v, win32::g_guestBase + a, 4); return v; }
void put(reg32 a, reg32 v) { memcpy(win32::g_guestBase + a, &v, 4); }
bool valid(reg32 a, reg32 n) { return a && win32::MemMap::isGuestRange(a, n); }

constexpr reg32 hash(const char* text)
{
    reg32 value = 0xffffffff;
    while (*text)
    {
        const unsigned char c = static_cast<unsigned char>(*text++);
        value = value * 33 + (c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }
    return value;
}

bool controllerPage(reg32 controller)
{
    return valid(controller, 0x4ec) && get(controller) == 0x006C3E3C && get(0x00735E20) == 3;
}

reg32 find(win32::WinApplication* app, x86::CPU& cpu, reg32 controller, reg32 name)
{
    cpu.eax = get(controller + 0xc);
    ::native::callGuest(app, cpu, 0x4f65d0);
    if (!cpu.eax)
        return 0;
    cpu.edx = cpu.eax;
    cpu.ecx = name;
    ::native::callGuest(app, cpu, 0x4ffb70);
    return cpu.eax;
}

void setText(win32::WinApplication* app, x86::CPU& cpu, reg32 object, reg32 text)
{
    if (!valid(object, 0x68) || get(object + 0x18) != 2 || !text)
        return;
    cpu.esp -= 8;
    put(cpu.esp, object);
    put(cpu.esp + 4, text);
    ::native::callGuest(app, cpu, 0x41f060); // FEString's own text conversion/allocation
    put(object + 0x1c, get(object + 0x1c) | 2);
}

void updateValue(win32::WinApplication* app, x86::CPU& cpu, reg32 controller, bool failed = false)
{
    // Use the game's localized ON/OFF strings, exactly like the Vibration row (0x4D0210).
    cpu.edx = win32::controls::rearTouchEnabled() ? 0x63c : 0xccfa;
    ::native::callGuest(app, cpu, 0x59fb80);
    const reg32 text = failed ? s_error : cpu.eax;
    const reg32 object = find(app, cpu, controller, hash("OptionData_3"));
    setText(app, cpu, object, text);
}

void rearTouchChanged(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 controller = cpu.ecx, message = get(cpu.esp + 4);
    if (controllerPage(controller) && (message == 0x9120409e || message == 0xb5971bf1))
    {
        const x86::CPU saved = cpu;
        const bool old = win32::controls::rearTouchEnabled();
        bool failed = !win32::controls::setRearTouchEnabled(!old);
        if (!failed && !pf::Config::setBool("rear_touch", !old))
        {
            win32::controls::setRearTouchEnabled(old);
            failed = true;
        }
        updateValue(app, cpu, controller, failed);
        // The same left/right selection sound used by Vibration's callback.
        cpu.eax = message == 0x9120409e ? 10 : 11;
        cpu.esp -= 4;
        put(cpu.esp, get(controller + 0xc));
        ::native::callGuest(app, cpu, 0x4f8340);
        cpu.esp += 4;
        pf::logInfo("Controller menu: rear_touch=%s%s", win32::controls::rearTouchEnabled() ? "on" : "off",
                    failed ? " (change failed)" : "");
        const bool terminated = cpu.terminate;
        cpu = saved;
        cpu.terminate |= terminated;
    }
    cpu.esp += 8; // thiscall callback, ret 4
}
}

void configure(win32::WinApplication& app)
{
    auto* memory = new win32::MemMap(4096, win32::MemMap::High);
    s_title = memory->getBlockStart();
    s_error = s_title + 64;
    s_callback = s_title + 128;
    strcpy(reinterpret_cast<char*>(win32::g_guestBase + s_title), "Rear Touch Pad");
    strcpy(reinterpret_cast<char*>(win32::g_guestBase + s_error), "Save failed");
    app.registerMethod(s_callback, {"ControllerRearTouchChanged", rearTouchChanged});
}

void appendRearTouch(win32::WinApplication* app, x86::CPU& cpu, reg32 controller)
{
    if (!s_callback || !controllerPage(controller))
        return;
    const x86::CPU saved = cpu;
    const reg32 title = find(app, cpu, controller, hash("OptionNametext_3"));
    const reg32 value = find(app, cpu, controller, hash("OptionData_3"));
    const reg32 group = find(app, cpu, controller, hash("OptionName_3"));
    if (valid(title, 0x68) && valid(value, 0x68) && valid(group, 0x20))
    {
        // The package already contains ten complete rows. These are the exact show/enable helpers called
        // for Vibration's second row by the original Controller builder; row 3 keeps the native art/navigation.
        cpu.ebx = controller;
        cpu.esp -= 12;
        put(cpu.esp, 3);
        put(cpu.esp + 4, 1); // show the value
        put(cpu.esp + 8, 0); // no extra Select button
        ::native::callGuest(app, cpu, 0x4d1600);
        cpu.ebx = 3;
        cpu.esp -= 8;
        put(cpu.esp, controller);
        put(cpu.esp + 4, 0); // left/right arrows rather than +/- icons
        ::native::callGuest(app, cpu, 0x4d16d0);
        put(controller + 0x48, s_callback); // third row's callback; first two retain their existing handlers
        setText(app, cpu, title, s_title);
        updateValue(app, cpu, controller);
        pf::logInfo("Controller menu: native Rear Touch Pad row added below Vibration");
    }
    else
        pf::logError("Controller menu: row 3 assets missing; rear-touch row not added");
    const bool terminated = cpu.terminate;
    cpu = saved;
    cpu.terminate |= terminated;
}
}}
