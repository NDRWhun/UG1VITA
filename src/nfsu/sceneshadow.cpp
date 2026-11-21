// Shadow compare of the native scene ports (sceneshadow.h).

#include "sceneshadow.h"
#include "retstate.h"

#include <platform/log.h>
#include <winapi/d3d9_native.h>

#include <cstdio>
#include <cstring>

namespace nfsu { namespace sceneshadow
{

namespace
{

using x86::reg32;
namespace bridge = win32::d3d9::bridge;

const uint32_t MaxArguments = 8;
const uint32_t ShadowLogLimit = 20;
const reg32 ArenaNext = 0x7355b4, ArenaEnd = 0x7355b8, ArenaFailed = 0x7355c4;
const reg32 ArenaWindow = 0x8000;   // bytes of the arena after its pointer that a compare covers

struct CalleeCall
{
    reg32 address;
    reg32 esp;
    uint32_t depth;
    uint8_t registers;
    uint32_t arguments;
    reg32 values[8];            // eax, ecx, edx, ebx, esp unused, ebp, esi, edi
    reg32 stack[MaxArguments];
    x86::CPU after;
    // The compared spans at the call and at its return.
    std::vector<x86::reg8> spansBefore, spansAfter;
};

struct Session
{
    bridge::Capture capture;
    std::vector<CalleeCall> calls;
    uint32_t next = 0;
    bool differs = false;
    char text[200] = {};
    std::vector<x86::reg8> before, guestAfter, nativeAfter;
    const std::vector<draw::Span>* spans = nullptr;
};

Session s_session;

const uint8_t RegisterBits[8] = { retstate::Eax, retstate::Ecx, retstate::Edx, retstate::Ebx, retstate::Esp,
                                  retstate::Ebp, retstate::Esi, retstate::Edi };
const char* const RegisterNames[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

void readValues(const x86::CPU& cpu, reg32* v)
{
    v[0] = cpu.eax;
    v[1] = cpu.ecx;
    v[2] = cpu.edx;
    v[3] = cpu.ebx;
    v[4] = cpu.esp;
    v[5] = cpu.ebp;
    v[6] = cpu.esi;
    v[7] = cpu.edi;
}

}

Mode g_mode = Off;
bool g_active = false;

bool recordSlow(reg32 address, uint32_t arguments, uint8_t registers, win32::WinApplication* app, x86::CPU& cpu,
                Function function)
{
    Session& s = s_session;
    CalleeCall c;
    c.address = address;
    c.esp = cpu.esp;
    c.depth = cpu.fpu.count;
    c.registers = registers;
    c.arguments = arguments < MaxArguments ? arguments : MaxArguments;
    readValues(cpu, c.values);
    for (uint32_t i = 0; i < c.arguments; ++i)
        c.stack[i] = rc::ld32(cpu.esp + 4 + 4 * i);
    if (s.spans)
        draw::snapshot(*s.spans, c.spansBefore);
    g_mode = Off;
    bridge::end();
    function(app, cpu);
    bridge::begin(s.capture, bridge::Capture::Record);
    g_mode = Recording;
    c.after = cpu;
    if (s.spans)
        draw::snapshot(*s.spans, c.spansAfter);
    s.calls.push_back(c);
    return true;
}

bool call(reg32 address, uint32_t arguments, uint8_t registers, win32::WinApplication* app, x86::CPU& cpu,
          Function function)
{
    if (g_mode != Replaying)
    {
        function(app, cpu);
        return !cpu.terminate;
    }
    Session& s = s_session;
    const uint32_t index = s.next;
    char what[120] = {};
    const auto fail = [&]()
    {
        if (!s.differs)
        {
            s.differs = true;
            snprintf(s.text, sizeof(s.text), "callee call %u (0x%08x): %s", unsigned(index), unsigned(address), what);
        }
    };
    if (s.next >= s.calls.size())
    {
        snprintf(what, sizeof(what), "the guest made %u calls", unsigned(s.calls.size()));
        fail();
        cpu.esp += 4 + 4 * arguments;
        return true;
    }
    const CalleeCall& c = s.calls[s.next++];
    if (c.address != address)
        snprintf(what, sizeof(what), "the guest called 0x%08x", unsigned(c.address)), fail();
    else if (c.esp != cpu.esp)
        snprintf(what, sizeof(what), "esp guest 0x%08x native 0x%08x", unsigned(c.esp), unsigned(cpu.esp)), fail();
    else if (c.depth != cpu.fpu.count)
        snprintf(what, sizeof(what), "x87 depth guest %u native %u", unsigned(c.depth), unsigned(cpu.fpu.count)),
            fail();
    else
    {
        reg32 values[8];
        readValues(cpu, values);
        for (uint32_t i = 0; !s.differs && i < 8; ++i)
        {
            if ((registers & RegisterBits[i]) && values[i] != c.values[i])
                snprintf(what, sizeof(what), "%s guest 0x%08x native 0x%08x", RegisterNames[i], unsigned(c.values[i]),
                         unsigned(values[i])), fail();
        }
        for (uint32_t i = 0; !s.differs && i < c.arguments; ++i)
        {
            const reg32 v = rc::ld32(cpu.esp + 4 + 4 * i);
            if (v != c.stack[i])
                snprintf(what, sizeof(what), "argument %u guest 0x%08x native 0x%08x", unsigned(i),
                         unsigned(c.stack[i]), unsigned(v)), fail();
        }
    }
    // The callee's stores to the compared spans: the native run must have left the spans as the guest run had them
    // at the call; then they become what the callee left.
    if (!s.differs && s.spans && !c.spansBefore.empty())
    {
        std::vector<x86::reg8> now;
        draw::snapshot(*s.spans, now);
        size_t at = 0;
        for (const draw::Span& span : *s.spans)
        {
            for (reg32 i = 0; i < span.size && !s.differs; ++i, ++at)
            {
                if (now[at] != c.spansBefore[at])
                    snprintf(what, sizeof(what), "byte at 0x%08x before the call guest 0x%02x native 0x%02x",
                             unsigned(span.address + i), unsigned(c.spansBefore[at]), unsigned(now[at])), fail();
            }
        }
    }
    if (s.spans && !c.spansAfter.empty())
        draw::restoreSpans(*s.spans, c.spansAfter);
    cpu = c.after;
    return true;
}

void arenaSpans(std::vector<draw::Span>& spans)
{
    spans.push_back(draw::Span{ArenaNext, 4});
    spans.push_back(draw::Span{ArenaFailed, 8});
    const reg32 next = rc::ld32(ArenaNext), end = rc::ld32(ArenaEnd);
    const reg32 size = end > next ? (end - next < ArenaWindow ? end - next : ArenaWindow) : 0;
    if (size)
        spans.push_back(draw::Span{next, size});
}

void compare(const char* tag, const char* name, reg32 function, uint8_t registers, win32::WinApplication* app,
             x86::CPU& cpu, Function guest, Function native, const std::vector<draw::Span>& spans, Stats& stats,
             bool x87)
{
    Session& s = s_session;
    const x86::CPU entry = cpu;
    draw::snapshot(spans, s.before);
    s.capture.clear();
    s.calls.clear();
    s.next = 0;
    s.differs = false;
    s.text[0] = 0;
    s.spans = &spans;
    g_active = true;

    g_mode = Recording;
    bridge::begin(s.capture, bridge::Capture::Record);
    guest(app, cpu);
    bridge::end();
    g_mode = Off;
    if (cpu.terminate)
    {
        g_active = false;
        s.spans = nullptr;
        return;
    }
    const x86::CPU guestCpu = cpu;
    draw::snapshot(spans, s.guestAfter);
    draw::restoreSpans(spans, s.before);

    cpu = entry;
    draw::g_stores.clear();
    draw::g_logStores = true;
    g_mode = Replaying;
    bridge::begin(s.capture, bridge::Capture::Replay);
    native(app, cpu);
    bridge::end();
    g_mode = Off;
    g_active = false;
    s.spans = nullptr;
    draw::g_logStores = false;
    const x86::CPU nativeCpu = cpu;
    draw::snapshot(spans, s.nativeAfter);
    draw::restoreSpans(spans, s.guestAfter);
    cpu = guestCpu;

    ++stats.calls;
    char text[200] = {};
    bool same = true;
    if (s.differs)
    {
        same = false;
        snprintf(text, sizeof(text), "%s", s.text);
    }
    else if (s.next != s.calls.size())
    {
        same = false;
        snprintf(text, sizeof(text), "callee calls guest %u native %u", unsigned(s.calls.size()), unsigned(s.next));
    }
    same = same && draw::sameCalls(s.capture, text, sizeof(text))
           && draw::sameState(function, registers, guestCpu, nativeCpu, text, sizeof(text))
           && draw::sameMemory(spans, s.guestAfter, s.nativeAfter, text, sizeof(text));
    if (same && x87)
    {
        const uint16_t g = guestCpu.fpu.status.word & retstate::ConditionCodes,
                       n = nativeCpu.fpu.status.word & retstate::ConditionCodes;
        if (g != n)
        {
            same = false;
            snprintf(text, sizeof(text), "x87 condition codes guest 0x%04x native 0x%04x", unsigned(g), unsigned(n));
        }
        else if (guestCpu.fpu.count != 0)
        {
            x86::CPU gc = guestCpu, nc = nativeCpu;
            const double gv = double(gc.fpu.st(0)), nv = double(nc.fpu.st(0));
            if (memcmp(&gv, &nv, 8) != 0)
            {
                same = false;
                snprintf(text, sizeof(text), "st0 guest %.17g native %.17g", gv, nv);
            }
        }
    }
    if (same)
        return;
    ++stats.differing;
    if (stats.logged < ShadowLogLimit)
    {
        ++stats.logged;
        pf::logInfo("%s: %s call %u (caller 0x%08x): %s", tag, name, unsigned(stats.calls),
                    unsigned(rc::ld32(entry.esp)), text);
    }
}

}}
