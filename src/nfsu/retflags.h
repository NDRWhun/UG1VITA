#ifndef NFSU_RETFLAGS_H_
#define NFSU_RETFLAGS_H_

#include <x86.h>
#include <cpu.h>

#include <cstddef>
#include <cstdint>

namespace nfsu { namespace retflags
{

// The flags the callers of a guest function read after it returns, from the recompiler's liveness (nfsu_layout.h):
// any other flag a function leaves is dead, the generated code does not store it and a native need not set it.
enum : uint8_t
{
    Cf = 1, Pf = 2, Af = 4, Zf = 8, Sf = 16, Of = 32,
    Fc = 64     // x87 condition codes C0, C2, C3
};

// C0, C2, C3 of the x87 status word.
const uint16_t ConditionCodes = 0x4500;

uint8_t of(x86::reg32 function);

// Shadow compares: false with a text when a flag of mask, the direction flag, or with Fc in mask the x87 condition
// codes differ. The rest of the x87 status word is the caller's to compare.
bool same(uint8_t mask, const x86::CPU& guest, const x86::CPU& native, char* text, size_t size);

}}

#endif
