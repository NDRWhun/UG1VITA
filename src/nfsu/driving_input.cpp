#include "driving_input.h"
#include <lib/controls.h>
#include <platform/log.h>
#include <cstring>

namespace nfsu { namespace driving_input {
namespace {
using x86::reg32;
uint8_t s_steering = 128;
bool s_driving = false, s_force = true;
reg32 get(reg32 a) { reg32 v; memcpy(&v, win32::g_guestBase + a, 4); return v; }
void put(reg32 a, reg32 v) { memcpy(win32::g_guestBase + a, &v, 4); }

// Same scanner ABI/state protocol as 0x5747B0. Only JOY_EVENT_STEER_ANALOG uses this scanner; drag lane-change
// scanners, digital d-pad steering and every other original event keep their existing paths.
void scanSteering(win32::WinApplication*, x86::CPU& cpu)
{
    const reg32 node = get(cpu.esp + 4);
    const reg32 config = get(cpu.esp + 16);
    cpu.eax = 0;
    if (node && config && win32::MemMap::isGuestRange(node, 0x20)
        && win32::MemMap::isGuestRange(config, 0x20) && get(config + 8) == 0x88)
    {
        if (s_force || get(node + 0x1c) || get(node + 0x18) != s_steering)
        {
            put(node + 0x18, s_steering);
            put(node + 0x1c, 0);
            cpu.eax = (reg32(s_steering) << 8) | 0x88;
            s_force = false;
        }
    }
    cpu.esp += 4; // cdecl scanner, arguments removed by caller
}
}

void configure(win32::WinApplication& app)
{
    auto* memory = new win32::MemMap(4096, win32::MemMap::High);
    const reg32 callback = memory->getBlockStart();
    app.registerMethod(callback, {"VitaAnalogSteeringScanner", scanSteering});
    put(0x00703DCC, callback); // scanner for JOY_EVENT_STEER_ANALOG (config 0x703DC0)
    pf::logInfo("input: Vita-calibrated analog steering, 8%% radial dead zone, proportional full range");
}

void poll(float x, float y, bool driving)
{
    if (driving != s_driving)
        s_force = true; // reassert a held steering value when returning from pause
    s_driving = driving;
    s_steering = driving ? win32::controls::steeringValue(x, y) : 128;
}
}}
