#include <lib/fpucontrol.h>
#include <platform/log.h>

namespace win32 { namespace fpucontrol
{

static const x86::reg32 D3DCREATE_FPU_PRESERVE = 0x00000002;
// Single precision (PC 00), round to nearest (RC 00), all six exceptions masked, reserved bit 6 set.
static const x86::reg16 Direct3DControlWord = 0x007F;

void deviceCreated(x86::CPU& cpu, x86::reg32 behaviorFlags)
{
    if (behaviorFlags & D3DCREATE_FPU_PRESERVE)
        return;
    pf::logDebug("x87 control word 0x%04x -> 0x%04x (CreateDevice without FPU_PRESERVE)", cpu.fpu.control.word,
                 Direct3DControlWord);
    cpu.fpu.control.word = Direct3DControlWord;
}

}}
