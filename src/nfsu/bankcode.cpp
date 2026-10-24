// Machine code inside the game's EA audio banks (Sound/**/*.abk, magic "ABKC"). Each bank entry carries a small
// x86 routine that the audio library runs from its mixer tick through a callback list (0x00652960). The bank loader
// 0x0064DF30 patches the routine's call targets to the library's functions and links the bank into the list at
// 0x00713C74. That code is not part of Speed.exe: the routines of the game's banks are translated at build time
// (banknative.h), and a routine runs translated while its bytes are the ones it was translated from. Other routines
// are interpreted. The routines of all 106 banks use only the instructions below (integer moves, adds, compares,
// pushes and calls); anything else traps with its address. Decoded instructions are kept per routine, linked to
// the instruction that follows, and reused while their bytes are unchanged. shadow_bank=1 runs each translated
// routine both ways, call by call, and compares registers, flags, stores and calls.

#include "audio_mixer.h"
#include "bankcode.h"
#include "banknative.h"

#include <lib/foreigncode.h>
#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <recomp.h>
#include <x86.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nfsu { namespace bankcode {

void unsupported(x86::reg32 ip, const char* what)
{
    pf::logError("bank code: %s at 0x%08x (%02x %02x %02x)", what, ip, rc::ld8(ip), rc::ld8(ip + 1), rc::ld8(ip + 2));
    x86::guestTrap(ip, "bank code");
}

namespace {

const x86::reg32 LoadedBanks = 0x00713C74;  // first loaded bank's list node (the node lies at bank + 0x50)
const x86::reg32 BankNode    = 0x50;
const x86::reg32 BankMagic   = 0x434B4241;  // "ABKC"
const int        MaxBanks    = 4096;        // list walk bound

struct Settings
{
    bool native;
    bool shadow;
};

const Settings& settings()
{
    static const Settings s = []
    {
        const Settings r = { pf::Config::getBool("bank_native", true), pf::Config::getBool("shadow_bank", false) };
        pf::logInfo("bank code: %u translated routines, %s%s", unsigned(g_translationCount),
                    r.native ? "native" : "interpreted", r.native && r.shadow ? ", shadow compare" : "");
        return r;
    }();
    return s;
}

// Bytes [begin, end) of one entry's routine, after relocation, in the bank at bank.
struct Range
{
    x86::reg32 begin;
    x86::reg32 end;
    x86::reg32 bank;
};

// Header fields: entry count (u16 at +0x0A), first entry (offset at +0x1C). Entry: routine at +0x28, its data
// (the end of the routine) at +0x2C, both absolute once loaded; the next entry follows after
// 0x3C + 4 * (byte +0x24 + byte +0x27) bytes (as in the loader).
bool findRoutine(x86::reg32 address, Range& range)
{
    int banks = 0;
    for (x86::reg32 node = rc::ld32(LoadedBanks); node && banks < MaxBanks; node = rc::ld32(node), ++banks)
    {
        const x86::reg32 bank = node - BankNode;
        if (rc::ld32(bank) != BankMagic)
            continue;
        const x86::reg32 count = rc::ld16(bank + 0x0A);
        x86::reg32 entry = bank + rc::ld32(bank + 0x1C);
        for (x86::reg32 i = 0; i < count; ++i)
        {
            const x86::reg32 begin = rc::ld32(entry + 0x28);
            const x86::reg32 end = rc::ld32(entry + 0x2C);
            if (address >= begin && address < end)
            {
                range = Range{ begin, end, bank };
                return true;
            }
            entry += 0x3C + 4 * (x86::reg32(rc::ld8(entry + 0x24)) + rc::ld8(entry + 0x27));
        }
    }
    return false;
}

const uint8_t NoRegister = 0xff;
const unsigned MaxLength = 8;               // the longest supported form: C6 /0 with SIB, disp32 and imm8

enum Op : uint8_t
{
    OpPush,         // push r32
    OpPop,          // pop r32
    OpMovStore,     // mov r/m32, r32
    OpMovLoad,      // mov r32, r/m32
    OpAdd,          // add r32, r/m32
    OpCmp,          // cmp r32, r/m32
    OpAddImm,       // add r/m32, imm8
    OpCmpImm,       // cmp r/m32, imm8
    OpStoreByte,    // mov m8, imm8
    OpImul,         // imul r32, r/m32
    OpJl,           // jl rel8
    OpJg,           // jg rel8
    OpCall,         // call rel32 (a library function patched in by the loader)
    OpRet,          // ret
};

// One decoded instruction at a guest address. A memory operand is base + (index << scale) + displacement, without
// base or index when their field says none. word holds the instruction bytes, zero past the length, and mask
// selects them.
struct Instruction
{
    uint32_t   word[2];
    uint32_t   mask[2];
    x86::reg32 start;
    uint32_t   next;        // index + 1 of the instruction at start + length, 0 while not linked
    uint8_t    length;
    uint8_t    op;
    uint8_t    reg;         // ModRM reg field, or the register of push / pop
    bool       memory;      // the ModRM operand is in memory
    bool       wide;        // the 8 bytes from start lie in guest memory
    uint8_t    rm;          // the operand register when !memory
    uint8_t    base;
    uint8_t    index;
    uint8_t    scale;
    x86::reg32 displacement;
    x86::reg32 immediate;   // imm8 sign-extended, or the jump or call target
};

// Decoded instructions of one routine; a slot per routine byte holds an index into instructions plus 1. native is
// the translation whose bytes, relocated for nativeBegin and nativeBank (expected), the routine had at its last
// check; seen holds the routine bytes of the last run that found no translation, in the bank at seenBank.
struct Routine
{
    std::vector<uint32_t>    slots;
    std::vector<Instruction> instructions;
    int                      running = 0;   // interpreters inside it (nested or preempted)
    const Translation*       native = nullptr;
    x86::reg32               nativeBegin = 0;
    x86::reg32               nativeBank = 0;
    std::vector<uint8_t>     expected;
    std::vector<uint8_t>     seen;
    x86::reg32               seenBank = 0;
};

// Routines by begin and end. Bank code runs only with the SND mutex (0x713C50) held: from the bank tick 0x652960,
// on the audio thread in the render loop without the execution lock (audio_mixer.cpp), and from SND calls of the
// game. So no lock here; a routine that is running stays put however others change the cache at preemption points.
std::unordered_map<uint64_t, std::unique_ptr<Routine>> s_routines;
const size_t MaxRoutines = 256;
const size_t MaxRoutineBytes = 0x10000;

// The cached routine of a range; null when the range is too large to cache.
Routine* routine(const Range& range)
{
    const x86::reg32 size = range.end - range.begin;
    if (size > MaxRoutineBytes)
        return nullptr;
    const uint64_t key = (uint64_t(range.begin) << 32) | range.end;
    auto found = s_routines.find(key);
    if (found != s_routines.end())
        return found->second.get();
    if (s_routines.size() >= MaxRoutines)
    {
        for (auto it = s_routines.begin(); it != s_routines.end();)
            it = it->second->running ? std::next(it) : s_routines.erase(it);
    }
    std::unique_ptr<Routine> created(new Routine);
    created->slots.assign(size, 0);
    Routine* result = created.get();
    s_routines.emplace(key, std::move(created));
    return result;
}

// Counts a run inside a routine while it lasts.
class Running
{
public:
    explicit Running(Routine& routine) : m_routine(routine) { ++m_routine.running; }
    ~Running() { --m_routine.running; }

private:
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;
    Routine& m_routine;
};

// Runs a routine from one call to the next: segment() stops at a call with the registers and flags in cpu and
// cpu.ip the target, resume() continues after it. Stores go to record when one is given.
class Interpreter
{
public:
    Interpreter(x86::CPU& cpu, const Range& range, Routine& routine, Recording* record = nullptr)
        :   m_cpu(cpu)
        ,   m_range(range)
        ,   m_routine(routine)
        ,   m_record(record)
        ,   m_running(routine)
    {
    }

    x86::reg32 executed() const { return m_executed; }
    x86::reg32 decoded() const { return m_decoded; }
    // Offset from the routine's start of the instruction after the current one.
    x86::reg32 followingOffset() const
    {
        const Instruction& in = m_routine.instructions[m_current];
        return in.start + in.length - m_range.begin;
    }

    void begin(x86::reg32 ip)
    {
        load();
        m_current = at(ip);
    }

    // True at a call, false at the return.
    bool segment()
    {
        uint32_t current = m_current;
        for (;;)
        {
            ++m_executed;
            if (!matches(m_routine.instructions[current]))
                redecode(current);
            const Instruction& in = m_routine.instructions[current];
            switch (in.op)
            {
            case OpPush:
            {
                const x86::reg32 value = m_r[in.reg];
                m_r[4] -= 4;
                st32(m_r[4], value);
                break;
            }
            case OpPop:
            {
                const x86::reg32 value = rc::ld32(m_r[4]);
                m_r[4] += 4;
                m_r[in.reg] = value;
                break;
            }
            case OpMovStore:
                write32(in, address(in), m_r[in.reg]);
                break;
            case OpMovLoad:
                m_r[in.reg] = read32(in, address(in));
                break;
            case OpAdd:
                m_r[in.reg] = add(m_r[in.reg], read32(in, address(in)));
                break;
            case OpCmp:
                compare(m_r[in.reg], read32(in, address(in)));
                break;
            case OpAddImm:
            {
                const x86::reg32 at = address(in);
                write32(in, at, add(read32(in, at), in.immediate));
                break;
            }
            case OpCmpImm:
                compare(read32(in, address(in)), in.immediate);
                break;
            case OpStoreByte:
                st8(address(in), x86::reg8(in.immediate));
                break;
            case OpImul:
            {
                const int64_t product = int64_t(x86::sreg32(m_r[in.reg])) * x86::sreg32(read32(in, address(in)));
                const x86::reg32 low = x86::reg32(product);
                materialize();
                m_cpu.flags.cf = m_cpu.flags.of = (product != int64_t(x86::sreg32(low)));
                m_r[in.reg] = low;
                break;
            }
            case OpJl:
            case OpJg:
            {
                materialize();
                const bool less = m_cpu.flags.sf != m_cpu.flags.of;
                if (in.op == OpJl ? less : (!less && !m_cpu.flags.zf))
                {
                    current = at(in.immediate);
                    continue;
                }
                break;
            }
            case OpCall:
                m_r[4] -= 4;
                store();
                m_cpu.ip = in.immediate;
                m_current = current;
                return true;
            case OpRet:
                m_r[4] += 4;
                store();
                m_current = current;
                return false;
            }
            current = following(current);
        }
    }

    // After the call: registers from cpu, then the instruction that follows the call.
    void resume()
    {
        load();
        m_current = following(m_current);
    }

private:
    // Kinds of the last flag-setting operation whose flags are not yet in cpu.flags.
    enum Pending : uint8_t
    {
        PendingNone,
        PendingAdd,
        PendingSub,
    };

    void load()
    {
        m_r[0] = m_cpu.eax;
        m_r[1] = m_cpu.ecx;
        m_r[2] = m_cpu.edx;
        m_r[3] = m_cpu.ebx;
        m_r[4] = m_cpu.esp;
        m_r[5] = m_cpu.ebp;
        m_r[6] = m_cpu.esi;
        m_r[7] = m_cpu.edi;
    }

    // Registers and flags into cpu.
    void store()
    {
        m_cpu.eax = m_r[0];
        m_cpu.ecx = m_r[1];
        m_cpu.edx = m_r[2];
        m_cpu.ebx = m_r[3];
        m_cpu.esp = m_r[4];
        m_cpu.ebp = m_r[5];
        m_cpu.esi = m_r[6];
        m_cpu.edi = m_r[7];
        materialize();
    }

    void materialize()
    {
        if (m_pending == PendingNone)
            return;
        const x86::reg32 a = m_flagA, b = m_flagB, r = m_flagResult;
        if (m_pending == PendingAdd)
        {
            m_cpu.flags.cf = r < a;
            m_cpu.flags.of = ((a ^ r) & (b ^ r)) >> 31;
        }
        else
        {
            m_cpu.flags.cf = a < b;
            m_cpu.flags.of = ((a ^ b) & (a ^ r)) >> 31;
        }
        m_cpu.flags.zf = r == 0;
        m_cpu.flags.sf = r >> 31;
        m_pending = PendingNone;
    }

    void st32(x86::reg32 address, x86::reg32 value)
    {
        if (m_record)
            m_record->st32(address, value);
        else
            rc::st32(address, value);
    }

    void st8(x86::reg32 address, x86::reg8 value)
    {
        if (m_record)
            m_record->st8(address, value);
        else
            rc::st8(address, value);
    }

    // Index of the instruction at address, decoded when not cached; traps outside the routine.
    uint32_t at(x86::reg32 address)
    {
        if (address < m_range.begin || address >= m_range.end)
        {
            store();
            unsupported(address, "jump out of the routine");
        }
        uint32_t& slot = m_routine.slots[address - m_range.begin];
        if (!slot)
        {
            Instruction in;
            decode(address, in);
            m_routine.instructions.push_back(in);
            slot = uint32_t(m_routine.instructions.size());
        }
        return slot - 1;
    }

    uint32_t following(uint32_t current)
    {
        const uint32_t next = m_routine.instructions[current].next;
        if (next)
            return next - 1;
        const Instruction& in = m_routine.instructions[current];
        const uint32_t index = at(in.start + in.length);
        m_routine.instructions[current].next = index + 1;
        return index;
    }

    bool matches(const Instruction& in) const
    {
        if (in.wide)
        {
            uint32_t word[2];
            memcpy(word, rc::host(in.start), sizeof(word));
            return (((word[0] ^ in.word[0]) & in.mask[0]) | ((word[1] ^ in.word[1]) & in.mask[1])) == 0;
        }
        return memcmp(rc::host(in.start), in.word, in.length) == 0;
    }

    // The guest bytes at a cached instruction changed: decoded again in place.
    void redecode(uint32_t current)
    {
        Instruction in;
        decode(m_routine.instructions[current].start, in);
        m_routine.instructions[current] = in;
    }

    // Decodes the instruction at start; traps on what the interpreter does not run, at the same point and with the
    // same message as running it would.
    void decode(x86::reg32 start, Instruction& in)
    {
        store();
        ++m_decoded;
        m_start = start;
        m_ip = start;
        in.start = start;
        in.next = 0;
        in.memory = false;
        in.reg = 0;
        in.rm = 0;
        in.base = NoRegister;
        in.index = NoRegister;
        in.scale = 0;
        in.displacement = 0;
        in.immediate = 0;
        const x86::reg8 op = fetch8();
        if (op >= 0x50 && op <= 0x5F)
        {
            in.op = op <= 0x57 ? OpPush : OpPop;
            in.reg = op & 7;
        }
        else
        {
            switch (op)
            {
            case 0x89:
            case 0x8B:
            case 0x03:
            case 0x3B:
                in.op = op == 0x89 ? OpMovStore : op == 0x8B ? OpMovLoad : op == 0x03 ? OpAdd : OpCmp;
                modrm(in);
                break;
            case 0x83:
                modrm(in);
                in.immediate = x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
                if (in.reg != 0 && in.reg != 7)
                    unsupported(m_start, "group 1 operation");
                in.op = in.reg == 0 ? OpAddImm : OpCmpImm;
                break;
            case 0xC6:
                modrm(in);
                if (in.reg != 0)
                    unsupported(m_start, "C6 form");
                in.immediate = fetch8();
                if (!in.memory)
                    unsupported(m_start, "byte register");
                in.op = OpStoreByte;
                break;
            case 0x0F:
                if (fetch8() != 0xAF)
                    unsupported(m_start, "two-byte opcode");
                modrm(in);
                in.op = OpImul;
                break;
            case 0x7C:
            case 0x7F:
            {
                const x86::reg32 rel = x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
                in.immediate = m_ip + rel;
                in.op = op == 0x7C ? OpJl : OpJg;
                break;
            }
            case 0xE8:
            {
                const x86::reg32 rel = fetch32();
                in.immediate = m_ip + rel;
                in.op = OpCall;
                break;
            }
            case 0xC3:
                in.op = OpRet;
                break;
            default:
                unsupported(m_start, "instruction");
            }
        }
        in.length = uint8_t(m_ip - m_start);
        uint8_t bytes[MaxLength] = {};
        uint8_t mask[MaxLength] = {};
        memcpy(bytes, rc::host(m_start), in.length);
        memset(mask, 0xff, in.length);
        memcpy(in.word, bytes, sizeof(in.word));
        memcpy(in.mask, mask, sizeof(in.mask));
        in.wide = win32::MemMap::isGuestRange(m_start, MaxLength);
    }

    x86::reg8 fetch8()
    {
        return rc::ld8(m_ip++);
    }

    x86::reg32 fetch32()
    {
        const x86::reg32 value = rc::ld32(m_ip);
        m_ip += 4;
        return value;
    }

    // ModRM (and SIB, displacement) at m_ip.
    void modrm(Instruction& in)
    {
        const x86::reg8 byte = fetch8();
        const int mod = byte >> 6;
        const int rm = byte & 7;
        in.reg = (byte >> 3) & 7;
        if (mod == 3)
        {
            in.memory = false;
            in.rm = uint8_t(rm);
            return;
        }
        in.memory = true;
        if (rm == 4)
        {
            const x86::reg8 sib = fetch8();
            const int base = sib & 7;
            const int index = (sib >> 3) & 7;
            if (base == 5 && mod == 0)
                in.displacement = fetch32();
            else
                in.base = uint8_t(base);
            if (index != 4)
            {
                in.index = uint8_t(index);
                in.scale = uint8_t(sib >> 6);
            }
        }
        else if (rm == 5 && mod == 0)
        {
            in.displacement = fetch32();
        }
        else
        {
            in.base = uint8_t(rm);
        }
        if (mod == 1)
            in.displacement += x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
        else if (mod == 2)
            in.displacement += fetch32();
    }

    x86::reg32 address(const Instruction& in) const
    {
        x86::reg32 address = in.base != NoRegister ? m_r[in.base] : 0;
        if (in.index != NoRegister)
            address += m_r[in.index] << in.scale;
        return address + in.displacement;
    }

    x86::reg32 read32(const Instruction& in, x86::reg32 address) const
    {
        return in.memory ? rc::ld32(address) : m_r[in.rm];
    }

    void write32(const Instruction& in, x86::reg32 address, x86::reg32 value)
    {
        if (in.memory)
            st32(address, value);
        else
            m_r[in.rm] = value;
    }

    x86::reg32 add(x86::reg32 a, x86::reg32 b)
    {
        const x86::reg32 r = a + b;
        m_pending = PendingAdd;
        m_flagA = a;
        m_flagB = b;
        m_flagResult = r;
        return r;
    }

    void compare(x86::reg32 a, x86::reg32 b)
    {
        m_pending = PendingSub;
        m_flagA = a;
        m_flagB = b;
        m_flagResult = a - b;
    }

    x86::CPU&              m_cpu;
    Range                  m_range;
    Routine&               m_routine;
    Recording*             m_record;
    Running                m_running;
    x86::reg32             m_r[8];          // eax ecx edx ebx esp ebp esi edi, in cpu at calls and on return
    Pending                m_pending = PendingNone;
    x86::reg32             m_flagA = 0;
    x86::reg32             m_flagB = 0;
    x86::reg32             m_flagResult = 0;
    uint32_t               m_current = 0;   // the instruction segment() continues with, or stopped at
    x86::reg32             m_ip = 0;        // decode position
    x86::reg32             m_start = 0;     // the instruction being decoded
    x86::reg32             m_executed = 0;
    x86::reg32             m_decoded = 0;
};

// Library functions (table 0x6E9AE8) a routine calls without the execution lock in the render loop: they read and
// write only the data their arguments point at (the routine's and the bank's), the SND random generator state
// (0x6511B0) and constants, and call nothing else. The sound state 0x64D860 and the instance teardown 0x64C690 run
// without it when their audit passes (mixer::libraryAudited); the others (the listener walks 0x64C800, 0x64C870,
// 0x64C9B0) and failed audits take the lock for the rest of the render loop's run.
const x86::reg32 s_unlockedLibrary[] = {
    0x64c650, 0x64c660, 0x64c670, 0x64c680, 0x64ca00, 0x64ca50, 0x64ca80, 0x64cb30, 0x64cb90, 0x64cbd0, 0x64cc20,
    0x64cc60, 0x64ccc0, 0x64cdd0, 0x64cef0, 0x64cf90, 0x64cfb0, 0x64cff0, 0x64d020, 0x64d030, 0x64d060, 0x64d070,
    0x64d0b0, 0x64d0d0, 0x64d0f0, 0x64d100, 0x64d110, 0x64d120, 0x64d140, 0x64d960, 0x64da70, 0x64db20, 0x64db50,
    0x64db70, 0x64db90,
};

// A routine's call of cpu.ip. In the render loop without the execution lock, a call of a translated function
// outside the list above that fails its audit takes the lock for the rest of the loop's run; calls of bank code run
// as they are.
void callTarget(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 target = cpu.ip;
    const char* reason = nullptr;
    x86::reg32 value = 0;
    if (mixer::unlocked(cpu)
        && !std::binary_search(std::begin(s_unlockedLibrary), std::end(s_unlockedLibrary), target)
        && app->getMethodName(target)
        && !mixer::libraryAudited(target, cpu, reason, value))
    {
        mixer::lockRest(app, cpu);
        if (pf::perf::enabled())
        {
            pf::perf::audio(pf::perf::AudioLibraryLocked, 1);
            pf::perf::audioLocked(reason, value);
        }
    }
    app->dynamic_call(target, cpu);
}

void countInterpreted(const Interpreter& interpreter)
{
    if (pf::perf::enabled())
    {
        pf::perf::audio(pf::perf::AudioBankInterpreted, 1);
        pf::perf::audio(pf::perf::AudioBankInstructions, interpreter.executed());
        pf::perf::audio(pf::perf::AudioBankDecodes, interpreter.decoded());
    }
}

void interpret(win32::WinApplication* app, x86::CPU& cpu, const Range& range, Routine& routine)
{
    Interpreter interpreter(cpu, range, routine);
    interpreter.begin(cpu.ip);
    while (interpreter.segment())
    {
        callTarget(app, cpu);
        if (cpu.terminate)
            break;
        interpreter.resume();
    }
    countInterpreted(interpreter);
}

// ---- translations ----

// The translation's bytes as the loader leaves them for a routine at begin in the bank at bank.
void relocate(const Translation& t, x86::reg32 begin, x86::reg32 bank, std::vector<uint8_t>& out)
{
    out.assign(t.bytes, t.bytes + t.byteCount);
    for (uint32_t i = 0; i < t.relocationCount; ++i)
    {
        const Relocation& r = t.relocations[i];
        const x86::reg32 value = r.kind == CallTarget ? r.value - (begin + r.offset) - 4 : r.value + bank;
        memcpy(&out[r.offset], &value, sizeof(value));
    }
}

bool spansMatch(const Translation& t, x86::reg32 begin, const std::vector<uint8_t>& expected)
{
    for (uint32_t i = 0; i < t.spanCount; ++i)
    {
        const Span& s = t.spans[i];
        if (memcmp(rc::host(begin + s.offset), &expected[s.offset], s.length) != 0)
            return false;
    }
    return true;
}

// Content hashes of the routines logged as having no translation.
std::unordered_set<uint32_t> s_untranslated;

void logUntranslated(const Range& range)
{
    const x86::reg32 size = range.end - range.begin;
    uint32_t hash = 2166136261u;
    for (x86::reg32 i = 0; i < size; ++i)
        hash = (hash ^ rc::ld8(range.begin + i)) * 16777619u;
    if (s_untranslated.size() < 256 && s_untranslated.insert(hash).second)
        pf::logInfo("bank code: routine 0x%08x-0x%08x (%u bytes, FNV-1a %08x) has no translation, interpreted",
                    unsigned(range.begin), unsigned(range.end), unsigned(size), unsigned(hash));
}

// The translation of the routine's current bytes, or null.
const Translation* translation(Routine& routine, const Range& range)
{
    if (routine.native && routine.nativeBegin == range.begin && routine.nativeBank == range.bank
        && spansMatch(*routine.native, range.begin, routine.expected))
        return routine.native;
    routine.native = nullptr;
    const x86::reg32 size = range.end - range.begin;
    if (!routine.seen.empty() && routine.seenBank == range.bank
        && memcmp(rc::host(range.begin), routine.seen.data(), size) == 0)
        return nullptr;
    for (uint32_t i = 0; i < g_translationCount; ++i)
    {
        const Translation& t = g_translations[i];
        if (t.length != size)
            continue;
        relocate(t, range.begin, range.bank, routine.expected);
        if (spansMatch(t, range.begin, routine.expected))
        {
            routine.native = &t;
            routine.nativeBegin = range.begin;
            routine.nativeBank = range.bank;
            routine.seen.clear();
            return &t;
        }
    }
    routine.seen.assign(rc::host(range.begin), rc::host(range.begin) + size);
    routine.seenBank = range.bank;
    logUntranslated(range);
    return nullptr;
}

void runNative(win32::WinApplication* app, x86::CPU& cpu, const Translation& t, const Range& range)
{
    Direct memory;
    uint32_t resume = 0;
    for (;;)
    {
        resume = t.run(cpu, memory, range.begin, range.bank, resume);
        if (resume == Returned)
            break;
        callTarget(app, cpu);
        if (cpu.terminate)
            break;
    }
    if (pf::perf::enabled())
        pf::perf::audio(pf::perf::AudioBankNative, 1);
}

// ---- shadow compare ----

struct Shadow
{
    uint32_t runs = 0;
    uint32_t differing = 0;
    uint32_t logged = 0;
};

Shadow s_shadow;
const uint32_t ShadowSummaryRuns = 5000;
const uint32_t ShadowDetails = 16;

// The first difference between a segment run translated (cpu native, stores of native) and interpreted (cpu, stores
// of guest), written to text; false when there is none.
bool segmentDiffers(const x86::CPU& native, const x86::CPU& guest, const Recording& nativeStores,
                    const Recording& guestStores, char* text, size_t size)
{
    static const char* const names[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    const x86::reg32 n[] = { native.eax, native.ecx, native.edx, native.ebx, native.esp, native.ebp, native.esi,
                             native.edi };
    const x86::reg32 g[] = { guest.eax, guest.ecx, guest.edx, guest.ebx, guest.esp, guest.ebp, guest.esi,
                             guest.edi };
    for (int i = 0; i < 8; ++i)
    {
        if (n[i] != g[i])
        {
            snprintf(text, size, "%s native 0x%08x interpreted 0x%08x", names[i], unsigned(n[i]), unsigned(g[i]));
            return true;
        }
    }
    if (native.flags.eflags != guest.flags.eflags)
    {
        snprintf(text, size, "eflags native 0x%08x interpreted 0x%08x", unsigned(native.flags.eflags),
                 unsigned(guest.flags.eflags));
        return true;
    }
    if (native.ip != guest.ip)
    {
        snprintf(text, size, "ip native 0x%08x interpreted 0x%08x", unsigned(native.ip), unsigned(guest.ip));
        return true;
    }
    const size_t count = nativeStores.stores.size() < guestStores.stores.size() ? nativeStores.stores.size()
                                                                                 : guestStores.stores.size();
    for (size_t i = 0; i < count; ++i)
    {
        const Recording::Store& a = nativeStores.stores[i];
        const Recording::Store& b = guestStores.stores[i];
        if (a.address != b.address || a.size != b.size || a.value != b.value)
        {
            snprintf(text, size, "store %u native [0x%08x]:%u = 0x%08x interpreted [0x%08x]:%u = 0x%08x",
                     unsigned(i), unsigned(a.address), unsigned(a.size), unsigned(a.value), unsigned(b.address),
                     unsigned(b.size), unsigned(b.value));
            return true;
        }
    }
    if (nativeStores.stores.size() != guestStores.stores.size())
    {
        snprintf(text, size, "stores native %u interpreted %u", unsigned(nativeStores.stores.size()),
                 unsigned(guestStores.stores.size()));
        return true;
    }
    return false;
}

// Each segment runs translated from the state before it, its stores undone, then interpreted; the interpreted run
// is kept. After a difference the rest of the run is interpreted only.
void runShadow(win32::WinApplication* app, x86::CPU& cpu, const Translation& t, const Range& range,
               Routine& routine)
{
    Shadow& s = s_shadow;
    Recording nativeStores, guestStores;
    Interpreter interpreter(cpu, range, routine, &guestStores);
    bool compare = true;
    uint32_t resume = 0;
    char text[160] = {};
    interpreter.begin(cpu.ip);
    for (uint32_t segment = 0; ; ++segment)
    {
        bool differs = false;
        x86::CPU native;
        if (compare)
        {
            const x86::CPU before = cpu;
            resume = t.record(cpu, nativeStores, range.begin, range.bank, resume);
            native = cpu;
            nativeStores.undo();
            cpu = before;
        }
        guestStores.stores.clear();
        const bool called = interpreter.segment();
        if (compare)
        {
            if (called != (resume != Returned))
            {
                snprintf(text, sizeof(text), "native %s, interpreted %s", resume == Returned ? "returned" : "called",
                         called ? "called" : "returned");
                differs = true;
            }
            else if (called && t.resumeOffsets[resume] != interpreter.followingOffset())
            {
                snprintf(text, sizeof(text), "call returns to +0x%x native, +0x%x interpreted",
                         unsigned(t.resumeOffsets[resume]), unsigned(interpreter.followingOffset()));
                differs = true;
            }
            else
            {
                differs = segmentDiffers(native, cpu, nativeStores, guestStores, text, sizeof(text));
            }
            nativeStores.stores.clear();
            if (differs)
            {
                compare = false;
                ++s.differing;
                if (s.logged < ShadowDetails)
                {
                    ++s.logged;
                    pf::logInfo("shadow_bank: run %u %s at 0x%08x segment %u: %s", unsigned(s.runs + 1), t.name,
                                unsigned(range.begin), unsigned(segment), text);
                }
            }
        }
        if (!called)
            break;
        callTarget(app, cpu);
        if (cpu.terminate)
            break;
        interpreter.resume();
    }
    countInterpreted(interpreter);
    if (++s.runs % ShadowSummaryRuns == 0)
        pf::logInfo("shadow_bank: %u runs compared, %u differ", unsigned(s.runs), unsigned(s.differing));
}

// Bank routines running (perf_log times the outermost one).
int s_depth = 0;

// The range resolve found for the address dynamic_call is about to run.
x86::reg32 s_resolvedAddress = 0;
Range      s_resolvedRange = { 0, 0, 0 };

// Entered like a translated function: cpu.ip is the routine, cpu.esp points at the return address slot.
void runRoutine(win32::WinApplication* app, x86::CPU& cpu)
{
    Range range;
    const bool resolved = s_resolvedAddress == cpu.ip && cpu.ip;
    s_resolvedAddress = 0;
    if (resolved)
        range = s_resolvedRange;
    else if (!findRoutine(cpu.ip, range))
        unsupported(cpu.ip, "no loaded bank routine");
    const bool timed = pf::perf::enabled() && s_depth == 0;
    const uint32_t start = timed ? pf::perf::now() : 0;
    ++s_depth;
    const Settings& options = settings();
    Routine* cached = routine(range);
    const Translation* native = cached && options.native && cpu.ip == range.begin ? translation(*cached, range)
                                                                                  : nullptr;
    if (native && options.shadow)
    {
        runShadow(app, cpu, *native, range, *cached);
    }
    else if (native)
    {
        runNative(app, cpu, *native, range);
    }
    else if (cached)
    {
        interpret(app, cpu, range, *cached);
    }
    else
    {
        Routine uncached;
        uncached.slots.assign(range.end - range.begin, 0);
        interpret(app, cpu, range, uncached);
    }
    --s_depth;
    if (timed)
    {
        pf::perf::audio(pf::perf::AudioBankRuns, 1);
        pf::perf::audio(pf::perf::AudioBankUs, pf::perf::now() - start);
    }
}

win32::MethodPtr resolve(x86::reg32 address)
{
    Range range;
    if (!findRoutine(address, range))
        return nullptr;
    s_resolvedAddress = address;
    s_resolvedRange = range;
    return &runRoutine;
}

}

void install()
{
    win32::setForeignCodeResolver(&resolve);
}

}}
