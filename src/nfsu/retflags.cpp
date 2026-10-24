// Flags read at function returns (retflags.h), from the generated table.

#include "retflags.h"

#include <nfsu_layout.h>

#include <cstdio>

namespace nfsu { namespace retflags
{

uint8_t of(x86::reg32 function)
{
    x86::reg32 low = 0, high = layout::flagReturnCount;
    while (low < high)
    {
        const x86::reg32 mid = (low + high) / 2;
        if (layout::flagReturns[mid].address < function)
            low = mid + 1;
        else
            high = mid;
    }
    return low < layout::flagReturnCount && layout::flagReturns[low].address == function
         ? layout::flagReturns[low].flags : 0;
}

bool same(uint8_t mask, const x86::CPU& guest, const x86::CPU& native, char* text, size_t size)
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

}}
