#include "../src/nfsu/driving_input.h"
#include <lib/controls.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr unsigned MemorySize = 0x1000000, Node = 0xa00000, Config = 0x703dc0, Stack = 0xb00000;
unsigned s_address;
win32::MethodPtr s_method;
unsigned get(unsigned a) { unsigned v; memcpy(&v, win32::g_guestBase + a, 4); return v; }
void put(unsigned a, unsigned v) { memcpy(win32::g_guestBase + a, &v, 4); }
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
void WinApplication::registerMethod(x86::reg32 a, Method method) { s_address = a; s_method = method.method; }
MethodPtr WinApplication::findMethod(x86::reg32 a) const { assert(a == s_address); return s_method; }
}
struct App : win32::WinApplication { App() : WinApplication("test", 0, {}) {} };
int main()
{
    App app;
    put(Config + 8, 0x88);
    nfsu::driving_input::configure(app);
    assert(get(Config + 12) == s_address);
    x86::CPU cpu{};
    auto scan = [&]() {
        cpu.esp = Stack;
        put(Stack + 4, Node); put(Stack + 16, Config);
        app.dynamic_call(s_address, cpu);
        assert(cpu.esp == Stack + 4);
        return cpu.eax;
    };
    for (float x : {-1.0f, -0.75f, -0.5f, -0.25f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
    {
        nfsu::driving_input::poll(x, 0, true);
        const unsigned byte = win32::controls::steeringValue(x, 0);
        assert(scan() == ((byte << 8) | 0x88));
        assert(scan() == 0); // held unchanged input follows the game's scanner protocol
        put(Node + 0x1c, 1);
        assert(scan() == ((byte << 8) | 0x88));
    }
    nfsu::driving_input::poll(1, 0, false);
    assert(scan() == 0x8088); // pause releases steering
    nfsu::driving_input::poll(1, 0, true);
    assert(scan() == 0xff88); // Resume reapplies a held stick
    std::puts("driving input: fractional analog events, neutral, endpoints, forced update and pause/resume passed");
}
