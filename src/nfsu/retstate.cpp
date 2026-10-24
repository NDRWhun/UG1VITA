// State read at function returns (retstate.h), from the generated tables.

#include "retstate.h"

#include <nfsu_layout.h>

#include <cstdio>

namespace nfsu { namespace retstate
{

namespace
{

// Index of the entry of function in a sorted table, or count.
template< typename T >
x86::reg32 find(const T* table, x86::reg32 count, x86::reg32 function)
{
    x86::reg32 low = 0, high = count;
    while (low < high)
    {
        const x86::reg32 mid = (low + high) / 2;
        if (table[mid].address < function)
            low = mid + 1;
        else
            high = mid;
    }
    return low < count && table[low].address == function ? low : count;
}

}

uint8_t flags(x86::reg32 function)
{
    const x86::reg32 i = find(layout::flagReturns, layout::flagReturnCount, function);
    return i < layout::flagReturnCount ? layout::flagReturns[i].flags : 0;
}

uint8_t registers(x86::reg32 function)
{
    const x86::reg32 i = find(layout::registerReturns, layout::registerReturnCount, function);
    return i < layout::registerReturnCount ? layout::registerReturns[i].registers : 0xff;
}

bool sameFlags(uint8_t mask, const x86::CPU& guest, const x86::CPU& native, char* text, size_t size)
{
    const struct { const char* name; uint8_t bit; bool g, n; } flags[] = {
        {"cf", Cf, bool(guest.flags.cf), bool(native.flags.cf)}, {"pf", Pf, bool(guest.flags.pf), bool(native.flags.pf)},
        {"af", Af, bool(guest.flags.af), bool(native.flags.af)}, {"zf", Zf, bool(guest.flags.zf), bool(native.flags.zf)},
        {"sf", Sf, bool(guest.flags.sf), bool(native.flags.sf)}, {"of", Of, bool(guest.flags.of), bool(native.flags.of)},
        {"df", 0xff, bool(guest.flags.df), bool(native.flags.df)},
    };
    for (const auto& f : flags)
    {
        if ((mask & f.bit) && f.g != f.n)
        {
            snprintf(text, size, "%s guest %d native %d", f.name, int(f.g), int(f.n));
            return false;
        }
    }
    if ((mask & Fc) && (guest.fpu.status.word & ConditionCodes) != (native.fpu.status.word & ConditionCodes))
    {
        snprintf(text, size, "x87 status guest 0x%04x native 0x%04x", unsigned(guest.fpu.status.word),
                 unsigned(native.fpu.status.word));
        return false;
    }
    return true;
}

bool sameRegisters(uint8_t mask, const x86::CPU& guest, const x86::CPU& native, char* text, size_t size)
{
    const struct { const char* name; uint8_t bit; x86::reg32 g, n; } regs[] = {
        {"eax", Eax, guest.eax, native.eax}, {"ecx", Ecx, guest.ecx, native.ecx}, {"edx", Edx, guest.edx, native.edx},
        {"ebx", Ebx, guest.ebx, native.ebx}, {"esp", Esp, guest.esp, native.esp}, {"ebp", Ebp, guest.ebp, native.ebp},
        {"esi", Esi, guest.esi, native.esi}, {"edi", Edi, guest.edi, native.edi},
    };
    for (const auto& r : regs)
    {
        if ((mask & r.bit) && r.g != r.n)
        {
            snprintf(text, size, "%s guest 0x%08x native 0x%08x", r.name, unsigned(r.g), unsigned(r.n));
            return false;
        }
    }
    return true;
}

}}
