#include <recomp.h>
#include <cpu.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <unordered_map>

namespace rc
{

namespace
{

struct JumpTarget
{
    jmp_buf     buffer;
    x86::reg32  fpuCount;   // x87 depth at the setjmp call
};

// Guest jmp_buf address -> host target. Elements keep their address when the map grows.
std::unordered_map<x86::reg32, JumpTarget> s_targets;
pf::LwMutex s_targetsMutex("setjmp");

}

jmp_buf& jumpBuffer(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 guestBuffer)
{
    RC_USE(app);
    pf::LwLockGuard guard(s_targetsMutex);
    JumpTarget& target = s_targets[guestBuffer];
    target.fpuCount = cpu.fpu.count;
    return target.buffer;
}

void longJump(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 guestBuffer)
{
    RC_USE(app);
    JumpTarget* target = nullptr;
    {
        pf::LwLockGuard guard(s_targetsMutex);
        const auto it = s_targets.find(guestBuffer);
        if (it != s_targets.end())
            target = &it->second;
    }
    if (!target)
        x86::guestTrap(guestBuffer, "longjmp to a jmp_buf no setjmp call filled");
    cpu.fpu.count = target->fpuCount;
    longjmp(target->buffer, 1);
}

}
