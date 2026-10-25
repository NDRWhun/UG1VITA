#include <platform/cores.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr.h>

namespace pf { namespace cores
{

namespace
{

// The probe thread gets this long to report where it ran.
const SceUInt ProbeTimeoutUs = 200000;

const char* const s_names[RoleCount] = { "game", "render", "main", "audio", "helper", "io" };

// Core of each role without and with core 3.
const int s_layout[2][RoleCount] =
{
    { 0, 1, 0, 2, 2, 2 },
    { 0, 1, 0, 2, 3, 3 },
};

bool s_core3 = false;
// core3_tasks=helpers (default): helper and io threads, a job phase worker and a GXM command list worker on core 3;
// core3_tasks=jobs: a job worker for posted tasks only.
bool s_core3Full = false;

int probeEntry(SceSize, void*)
{
    return sceKernelGetCpuId();
}

// Core 3 is usable when a thread created with its mask is accepted and actually runs there.
bool probeCore3()
{
    const SceUID id = sceKernelCreateThread("core3_probe",&probeEntry, Thread::PriorityHigh, 4096, 0,
                                            Thread::Cpu3, nullptr);
    if (id < 0)
    {
        logInfo("cores: a core 3 thread is refused (0x%08x)", id);
        return false;
    }
    int cpu = -1;
    bool ran = false;
    int result = sceKernelStartThread(id, 0, nullptr);
    if (result >= 0)
    {
        SceUInt timeout = ProbeTimeoutUs;
        result = sceKernelWaitThreadEnd(id, &cpu, &timeout);
        ran = result >= 0;
    }
    if (!ran)
    {
        // A thread that never got the core cannot be deleted; it is left to finish whenever it runs.
        logInfo("cores: the core 3 probe did not run (0x%08x)", result);
        return false;
    }
    sceKernelDeleteThread(id);
    if (cpu != 3)
        logInfo("cores: the core 3 probe ran on core %d", cpu);
    return cpu == 3;
}

}

void init()
{
    static const char* const Tasks[] = { "helpers", "jobs" };
    const bool allowed = Config::getBool("use_core3", true);
    s_core3 = allowed && probeCore3();
    s_core3Full = s_core3 && Config::getChoice("core3_tasks", Tasks, 2, 0) == 0;
    Thread::setCurrentCpuMask(mask(Main));
    logInfo("cores: %s; game %d, render %d, main %d, audio %d, helpers %d, io %d",
            s_core3 ? (s_core3Full ? "core 3 available (CapUnlocker), core3_tasks=helpers: helpers, io, phases and"
                                     " command lists there"
                                   : "core 3 available (CapUnlocker), core3_tasks=jobs: posted tasks only")
                    : allowed ? "cores 0-2" : "cores 0-2 (use_core3=off)",
            core(Game), core(Render), core(Main), core(Audio), core(Helper), core(Io));
}

bool hasCore3()
{
    return s_core3;
}

bool core3Waited()
{
    return s_core3Full;
}

int core(Role role)
{
    return s_layout[s_core3Full ? 1 : 0][role];
}

int mask(Role role)
{
    return Thread::Cpu0 << core(role);
}

const char* name(Role role)
{
    return s_names[role];
}

}}
