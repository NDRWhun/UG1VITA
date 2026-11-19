// Exercise the actual row implementation with simulated guest UI calls and Vita hardware.
#include "../src/nfsu/controls_menu.h"
#include <lib/controls.h>
#include <platform/config.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr unsigned MemorySize = 0x1000000, Controller = 0xa00100, Package = 0xa01000;
constexpr unsigned Title = 0xa02000, Value = 0xa02100, Group = 0xa02200, On = 0xa03000, Off = 0xa03010;
unsigned s_callbackAddress = 0, s_shown = 0, s_arrows = 0;
win32::MethodPtr s_callback = nullptr;
unsigned get(unsigned a) { unsigned n; memcpy(&n, win32::g_guestBase + a, 4); return n; }
void put(unsigned a, unsigned n) { memcpy(win32::g_guestBase + a, &n, 4); }
unsigned hash(const char* text)
{
    unsigned h = 0xffffffff;
    for (; *text; ++text) h = h * 33 + (*text >= 'a' && *text <= 'z' ? *text - 'a' + 'A' : *text);
    return h;
}
void package(win32::WinApplication*, x86::CPU& cpu)
{
    assert(cpu.eax == 0x6c2c10);
    cpu.eax = Package;
    cpu.esp += 4;
}
void object(win32::WinApplication*, x86::CPU& cpu)
{
    assert(cpu.edx == Package);
    cpu.eax = cpu.ecx == hash("OptionNametext_3") ? Title : cpu.ecx == hash("OptionData_3") ? Value
            : cpu.ecx == hash("OptionName_3") ? Group : 0;
    assert(cpu.eax);
    cpu.esp += 4;
}
void setText(win32::WinApplication*, x86::CPU& cpu)
{
    const unsigned obj = get(cpu.esp + 4), source = get(cpu.esp + 8);
    assert(obj == Title || obj == Value);
    const unsigned buffer = get(obj + 0x60);
    for (unsigned i = 0;; ++i)
    {
        const unsigned char c = win32::g_guestBase[source + i];
        win32::g_guestBase[buffer + i * 2] = c;
        win32::g_guestBase[buffer + i * 2 + 1] = 0;
        if (!c) break;
    }
    cpu.esp += 12;
}
void language(win32::WinApplication*, x86::CPU& cpu)
{
    assert(cpu.edx == 0x63c || cpu.edx == 0xccfa);
    cpu.eax = cpu.edx == 0x63c ? On : Off;
    cpu.esp += 4;
}
void showRow(win32::WinApplication*, x86::CPU& cpu)
{
    assert(cpu.ebx == Controller && get(cpu.esp + 4) == 3);
    assert(get(cpu.esp + 8) == 1 && get(cpu.esp + 12) == 0);
    ++s_shown;
    cpu.esp += 16;
}
void showArrows(win32::WinApplication*, x86::CPU& cpu)
{
    assert(cpu.ebx == 3 && get(cpu.esp + 4) == Controller && get(cpu.esp + 8) == 0);
    ++s_arrows;
    cpu.esp += 12;
}
void sound(win32::WinApplication*, x86::CPU& cpu)
{
    assert(cpu.eax == 10 || cpu.eax == 11);
    assert(get(cpu.esp + 4) == 0x6c2c10);
    cpu.esp += 4;
}
bool labelIs(unsigned obj, const char* text)
{
    const unsigned buffer = get(obj + 0x60);
    for (unsigned i = 0; i <= strlen(text); ++i)
        if (win32::g_guestBase[buffer + i * 2] != text[i]) return false;
    return true;
}
}

namespace pf {
void logInfo(const char*, ...) {}
void logError(const char*, ...) {}
}
extern "C" int sceCtrlPeekBufferPositive(int, SceCtrlData*, int) { return 0; }
extern "C" int sceTouchSetSamplingState(SceUInt32, SceTouchSamplingState) { return 0; }
extern "C" int sceTouchPeek(SceUInt32, SceTouchData*, SceUInt32) { return 0; }

namespace win32 {
x86::reg8* const g_guestBase = new x86::reg8[MemorySize]();
x86::reg32 MemMap::s_addressOffset = 0x900000;
MemMap::MemMap(x86::reg32, Placement, pf::perf::Alloc) : m_block(0), m_blockCount(1), m_kind(0) {}
MemMap::~MemMap() {}
bool MemMap::isGuestRange(x86::reg32 a, x86::reg32 n) { return a < MemorySize && n <= MemorySize - a; }
WinApplication::WinApplication(const char*, x86::reg32, const std::vector<Section>&)
    : m_preemptWaiters(0), m_methodSlots(3), m_methodShift(31) {}
WinApplication::~WinApplication() {}
void WinApplication::yieldContext(const x86::CPU&) { assert(false); }
void WinApplication::registerMethod(x86::reg32 a, Method method)
{
    s_callbackAddress = a;
    s_callback = method.method;
}
MethodPtr WinApplication::findMethod(x86::reg32 a) const
{
    if (a == s_callbackAddress) return s_callback;
    switch (a) {
    case 0x4f65d0: return package;
    case 0x4ffb70: return object;
    case 0x41f060: return setText;
    case 0x59fb80: return language;
    case 0x4d1600: return showRow;
    case 0x4d16d0: return showArrows;
    case 0x4f8340: return sound;
    default: assert(false); return nullptr;
    }
}
}
struct App : win32::WinApplication { App() : WinApplication("test", 0, {}) {} };

int main(int argc, char** argv)
{
    assert(argc == 2);
    pf::Config::load(argv[1]);
    put(Controller, 0x6c3e3c);
    put(Controller + 0xc, 0x6c2c10);
    put(Controller + 0x40, 0x4d0350); // Controller Setup
    put(Controller + 0x44, 0x4d02c0); // Vibration
    put(Title + 0x18, 2); put(Value + 0x18, 2);
    put(Title + 0x60, 0xa04000); put(Value + 0x60, 0xa04100);
    strcpy(reinterpret_cast<char*>(win32::g_guestBase + On), "On");
    strcpy(reinterpret_cast<char*>(win32::g_guestBase + Off), "Off");
    App app;
    nfsu::controls_menu::configure(app);
    x86::CPU cpu{};
    cpu.esp = 0xb00000; cpu.eax = 0x12345678;
    const auto before = cpu;
    for (unsigned category = 0; category <= 10; ++category)
    {
        const unsigned shown = s_shown;
        put(0x735e20, category);
        nfsu::controls_menu::appendRearTouch(&app, cpu, Controller);
        assert(s_shown == shown + (category == 3));
    }
    assert(s_shown == 1 && s_arrows == 1);
    assert(cpu.esp == before.esp && cpu.eax == before.eax);
    assert(get(Controller + 0x40) == 0x4d0350 && get(Controller + 0x44) == 0x4d02c0);
    assert(get(Controller + 0x48) == s_callbackAddress);
    assert(labelIs(Title, "Rear Touch Pad") && labelIs(Value, "Off"));
    put(0x735e20, 3);
    auto change = [&](unsigned message) {
        cpu.ecx = Controller; cpu.esp = 0xb00000;
        put(cpu.esp + 4, message);
        app.dynamic_call(s_callbackAddress, cpu);
        assert(cpu.esp == 0xb00008);
    };
    change(0xc407210); // Cross/accept does not secretly toggle the setting
    assert(!win32::controls::rearTouchEnabled());
    change(0xb5971bf1);
    assert(win32::controls::rearTouchEnabled() && labelIs(Value, "On"));
    pf::Config::load(argv[1]);
    assert(pf::Config::getBool("rear_touch", false));
    change(0x9120409e);
    assert(!win32::controls::rearTouchEnabled() && labelIs(Value, "Off"));
    pf::Config::load(argv[1]);
    assert(!pf::Config::getBool("rear_touch", true));
    pf::Config::load("/no-such-directory/nfsu/settings.cfg");
    change(0xb5971bf1);
    assert(!win32::controls::rearTouchEnabled() && labelIs(Value, "Save failed"));
    put(0x735e20, 4);
    change(0xb5971bf1);
    assert(!win32::controls::rearTouchEnabled());
    std::puts("Controller row: native third row/arrows, existing handlers preserved, immediate saved on/off, failure rollback");
}
