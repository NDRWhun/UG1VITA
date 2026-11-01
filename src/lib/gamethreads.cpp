#include <lib/scheduler.h>

// The guest threads of Speed.exe. Each sets its own Windows priority after it
// is created; the scheduler maps that onto the execution lock: TIME_CRITICAL (the audio mixer) and HIGHEST (EA
// TIMER, network pump, serial) preempt the game thread at its next preemption point, ABOVE_NORMAL (EA FILE)
// preempts normal threads. The network, modem and serial threads only start for online, modem or serial play;
// offline, their libraries fail before that (WSAStartup, lineInitialize with no lines, no COM ports), and the TAPI
// thread, if started, ends at once.
namespace win32 { namespace scheduler
{

void configureGameThreads()
{
    // _beginthreadex passes the CRT's start routine (entry and argument at +0x4C/+0x50 of its thread data); the EA
    // thread library starts its threads through a trampoline (entry and argument at +4/+8 of its block).
    setStartThunk(0x006749CA, 0x4C, 0x50);
    setStartThunk(0x0063E4D0, 0x04, 0x08);
    setThreadRole(0x00654A90, pf::cores::Audio,  "audio mixer");
    setThreadRole(0x0063EA00, pf::cores::Helper, "EA timer");
    setThreadRole(0x006392F0, pf::cores::Io,     "EA file device");
    setThreadRole(0x00663330, pf::cores::Helper, "DNS lookup");
    setThreadRole(0x00668F00, pf::cores::Helper, "network pump");
    setThreadRole(0x00669E60, pf::cores::Helper, "TAPI");
    setThreadRole(0x0066FD80, pf::cores::Helper, "serial");
}

}}
