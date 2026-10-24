#ifndef NFSU_RETSTATE_H_
#define NFSU_RETSTATE_H_

#include <x86.h>
#include <cpu.h>

#include <cstddef>
#include <cstdint>

namespace nfsu { namespace retstate
{

// What the callers of a guest function read from cpu after it returns, from the recompiler's liveness
// (nfsu_layout.h). The translated guest code stores nothing else at its returns (registers its direct callers keep
// in their locals, esp moved by a known delta, dead flags), so a shadow compare skips the rest. A native still
// writes the registers and esp it leaves: native callers and indirect callers read them from cpu.
enum : uint8_t
{
    Cf = 1, Pf = 2, Af = 4, Zf = 8, Sf = 16, Of = 32,
    Fc = 64     // x87 condition codes C0, C2, C3
};

enum : uint8_t
{
    Eax = 1, Ecx = 2, Edx = 4, Ebx = 8, Esp = 0x10, Ebp = 0x20, Esi = 0x40, Edi = 0x80
};

// C0, C2, C3 of the x87 status word.
const uint16_t ConditionCodes = 0x4500;

// The flags read at the function's return.
uint8_t flags(x86::reg32 function);
// The registers read at the return of a native override; all of them for any other function.
uint8_t registers(x86::reg32 function);

// Shadow compares: false with a text when a flag of mask, the direction flag, or with Fc in mask the x87 condition
// codes differ. The rest of the x87 status word is the caller's to compare.
bool sameFlags(uint8_t mask, const x86::CPU& guest, const x86::CPU& native, char* text, size_t size);
// False with a text when a register of mask differs.
bool sameRegisters(uint8_t mask, const x86::CPU& guest, const x86::CPU& native, char* text, size_t size);

}}

#endif
