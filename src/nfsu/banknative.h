#ifndef NFSU_BANKNATIVE_H_
#define NFSU_BANKNATIVE_H_

// Audio bank routines translated at build time (tools/recompiler/bankcode.py, generated/nfsu_bank.cpp) and the
// pieces they run with (bankcode.cpp). A translation does what the bank code interpreter does, step for step:
// the same loads and stores in the same order, registers and flags in cpu at every call and at the return.

#include <cpu.h>
#include <recomp.h>
#include <x86.h>

#include <cstdint>
#include <vector>

namespace nfsu { namespace bankcode {

using x86::reg32;

// Stores straight to guest memory.
struct Direct
{
    void st32(reg32 address, reg32 value) { rc::st32(address, value); }
    void st8(reg32 address, x86::reg8 value) { rc::st8(address, value); }
};

// Stores to guest memory, each logged with the value it replaced.
struct Recording
{
    struct Store
    {
        reg32   address;
        reg32   value;
        reg32   previous;
        uint8_t size;
    };

    std::vector<Store> stores;

    void st32(reg32 address, reg32 value)
    {
        stores.push_back(Store{ address, value, rc::ld32(address), 4 });
        rc::st32(address, value);
    }

    void st8(reg32 address, x86::reg8 value)
    {
        stores.push_back(Store{ address, value, rc::ld8(address), 1 });
        rc::st8(address, value);
    }

    // Puts the replaced values back, last store first, and clears the log.
    void undo()
    {
        for (auto it = stores.rbegin(); it != stores.rend(); ++it)
        {
            if (it->size == 4)
                rc::st32(it->address, it->previous);
            else
                rc::st8(it->address, x86::reg8(it->previous));
        }
        stores.clear();
    }
};

// Operands and result of the last add or cmp; cf, of, zf and sf go to cpu.flags when something reads them.
struct LazyFlags
{
    enum Kind : uint8_t
    {
        None,
        Add,
        Sub,
    };

    uint8_t kind = None;
    reg32   a = 0;
    reg32   b = 0;
    reg32   r = 0;

    reg32 add(reg32 x, reg32 y)
    {
        kind = Add;
        a = x;
        b = y;
        r = x + y;
        return r;
    }

    void sub(reg32 x, reg32 y)
    {
        kind = Sub;
        a = x;
        b = y;
        r = x - y;
    }

    void materialize(x86::CPU& cpu)
    {
        if (kind == None)
            return;
        if (kind == Add)
        {
            cpu.flags.cf = r < a;
            cpu.flags.of = ((a ^ r) & (b ^ r)) >> 31;
        }
        else
        {
            cpu.flags.cf = a < b;
            cpu.flags.of = ((a ^ b) & (a ^ r)) >> 31;
        }
        cpu.flags.zf = r == 0;
        cpu.flags.sf = r >> 31;
        kind = None;
    }
};

// Registers into cpu and the pending flags into cpu.flags.
inline void storeState(x86::CPU& cpu, LazyFlags& flags, reg32 eax, reg32 ecx, reg32 edx, reg32 ebx, reg32 esp,
                       reg32 ebp, reg32 esi, reg32 edi)
{
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.edx = edx;
    cpu.ebx = ebx;
    cpu.esp = esp;
    cpu.ebp = ebp;
    cpu.esi = esi;
    cpu.edi = edi;
    flags.materialize(cpu);
}

// Logs what the bank code at ip does that is not run and traps.
[[noreturn]] void unsupported(reg32 ip, const char* what);

// Resume point result of a routine that returned.
const uint32_t Returned = 0xffffffffu;

// Runs a translated routine from a resume point (0: its first instruction, k: the instruction after its k-th call)
// to its next call, with the registers and flags in cpu and cpu.ip the call target, and returns the resume point
// after that call; or to its return, with the registers and flags in cpu, and returns Returned. begin is the
// routine's address, bank the address of the bank that holds it.
template <typename Memory>
using Run = uint32_t (*)(x86::CPU& cpu, Memory& memory, reg32 begin, reg32 bank, uint32_t resume);

enum RelocationKind : uint8_t
{
    CallTarget,     // rel32 of a call to value: value - (begin + offset + 4)
    BankOffset,     // dword value + bank
};

// A dword the bank loader changes, at offset from the routine's start.
struct Relocation
{
    uint32_t offset;
    uint8_t  kind;
    reg32    value;
};

// Routine bytes the translation was made from: [offset, offset + length) from the routine's start.
struct Span
{
    uint32_t offset;
    uint32_t length;
};

struct Translation
{
    const char*         name;
    uint32_t            length;         // routine bytes: entry +0x2C - entry +0x28
    const uint8_t*      bytes;          // file bytes from the routine's start, byteCount of them
    uint32_t            byteCount;
    const Span*         spans;          // the instructions it runs
    uint32_t            spanCount;
    const Relocation*   relocations;    // inside the spans
    uint32_t            relocationCount;
    const uint32_t*     resumeOffsets;  // offset of the instruction at each resume point
    uint32_t            resumeCount;
    Run<Direct>         run;
    Run<Recording>      record;
};

extern const Translation* const g_translations;
extern const uint32_t g_translationCount;

}}

#endif /* !NFSU_BANKNATIVE_H_ */
