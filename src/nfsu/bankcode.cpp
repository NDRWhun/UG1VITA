// Machine code inside the game's EA audio banks (Sound/**/*.abk, magic "ABKC"). Each bank entry carries a small
// x86 routine that the audio library runs from its mixer tick through a callback list (0x00652960). The bank loader
// 0x0064DF30 patches the routine's call targets to the library's functions and links the bank into the list at
// 0x00713C74. That code is not part of Speed.exe, so it has no translation: it is interpreted here. The routines
// of all 106 banks use only the instructions below (integer moves, adds, compares, pushes and calls); anything else
// traps with its address. Decoded instructions are kept per routine and reused while their bytes are unchanged.

#include "bankcode.h"

#include <lib/foreigncode.h>
#include <platform/log.h>
#include <recomp.h>
#include <x86.h>

#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace nfsu { namespace bankcode {

namespace {

const x86::reg32 LoadedBanks = 0x00713C74;  // first loaded bank's list node (the node lies at bank + 0x50)
const x86::reg32 BankNode    = 0x50;
const x86::reg32 BankMagic   = 0x434B4241;  // "ABKC"
const int        MaxBanks    = 4096;        // list walk bound

// Bytes [begin, end) of one entry's routine, after relocation.
struct Range
{
    x86::reg32 begin;
    x86::reg32 end;
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
                range = Range{ begin, end };
                return true;
            }
            entry += 0x3C + 4 * (x86::reg32(rc::ld8(entry + 0x24)) + rc::ld8(entry + 0x27));
        }
    }
    return false;
}

[[noreturn]] void unsupported(x86::reg32 ip, const char* what)
{
    pf::logError("bank code: %s at 0x%08x (%02x %02x %02x)", what, ip, rc::ld8(ip), rc::ld8(ip + 1), rc::ld8(ip + 2));
    x86::guestTrap(ip, "bank code");
}

const uint8_t NoRegister = 0xff;
const unsigned MaxLength = 8;               // the longest supported form: C6 /0 with SIB, disp32 and imm8

// One decoded instruction. A memory operand is base + (index << scale) + displacement, without base or index
// when their field says none.
struct Instruction
{
    uint8_t    bytes[MaxLength];
    uint8_t    length;
    uint8_t    op;          // the opcode byte; 0x0F for imul
    uint8_t    reg;         // ModRM reg field, or the register of push / pop
    bool       memory;      // the ModRM operand is in memory
    uint8_t    rm;          // its register when !memory
    uint8_t    base;
    uint8_t    index;
    uint8_t    scale;
    x86::reg32 displacement;
    x86::reg32 immediate;   // imm8 sign-extended, or the jump or call target
};

// Decoded instructions of one routine, by offset; a slot holds an index into instructions plus 1.
struct Routine
{
    std::vector<uint16_t>    slots;
    std::vector<Instruction> instructions;
    int                      running = 0;   // interpreters inside it (nested or preempted)
};

// Routines by begin and end. Only threads holding the execution lock run bank code, so no lock here; a routine
// that is running stays put however others change the cache at preemption points.
std::unordered_map<uint64_t, std::unique_ptr<Routine>> s_routines;
const size_t MaxRoutines = 256;
const size_t MaxRoutineBytes = 0x10000;

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

class Interpreter
{
public:
    Interpreter(win32::WinApplication* app, x86::CPU& cpu, const Range& range, Routine* routine)
        :   m_app(app)
        ,   m_cpu(cpu)
        ,   m_range(range)
        ,   m_routine(routine)
        ,   m_regs{ &cpu.eax, &cpu.ecx, &cpu.edx, &cpu.ebx, &cpu.esp, &cpu.ebp, &cpu.esi, &cpu.edi }
    {
        if (m_routine)
            ++m_routine->running;
    }

    ~Interpreter()
    {
        if (m_routine)
            --m_routine->running;
    }

    void run(x86::reg32 ip)
    {
        m_ip = ip;
        for (;;)
        {
            m_start = m_ip;
            if (m_ip < m_range.begin || m_ip >= m_range.end)
                unsupported(m_ip, "jump out of the routine");
            Instruction decoded;
            const Instruction& in = instruction(decoded);
            m_ip = m_start + in.length;
            const x86::reg8 op = in.op;
            if (op >= 0x50 && op <= 0x57)           // push r32
            {
                const x86::reg32 value = *m_regs[in.reg];
                m_cpu.esp -= 4;
                rc::st32(m_cpu.esp, value);
                continue;
            }
            if (op >= 0x58 && op <= 0x5F)           // pop r32
            {
                const x86::reg32 value = rc::ld32(m_cpu.esp);
                m_cpu.esp += 4;
                *m_regs[in.reg] = value;
                continue;
            }
            switch (op)
            {
            case 0x89:                              // mov r/m32, r32
                write32(in, *m_regs[in.reg]);
                break;
            case 0x8B:                              // mov r32, r/m32
                *m_regs[in.reg] = read32(in);
                break;
            case 0x03:                              // add r32, r/m32
                *m_regs[in.reg] = add(*m_regs[in.reg], read32(in));
                break;
            case 0x3B:                              // cmp r32, r/m32
                compare(*m_regs[in.reg], read32(in));
                break;
            case 0x83:                              // add / cmp r/m32, imm8
            {
                const x86::reg32 address = effectiveAddress(in);
                if (in.reg == 0)
                    write32(in, address, add(read32(in, address), in.immediate));
                else if (in.reg == 7)
                    compare(read32(in, address), in.immediate);
                else
                    unsupported(m_start, "group 1 operation");
                break;
            }
            case 0xC6:                              // mov m8, imm8
                rc::st8(effectiveAddress(in), x86::reg8(in.immediate));
                break;
            case 0x0F:                              // imul r32, r/m32
            {
                const int64_t product = int64_t(x86::sreg32(*m_regs[in.reg])) * x86::sreg32(read32(in));
                const x86::reg32 low = x86::reg32(product);
                m_cpu.flags.cf = m_cpu.flags.of = (product != int64_t(x86::sreg32(low)));
                *m_regs[in.reg] = low;
                break;
            }
            case 0x7C:                              // jl rel8
            case 0x7F:                              // jg rel8
            {
                const bool less = m_cpu.flags.sf != m_cpu.flags.of;
                if (op == 0x7C ? less : (!less && !m_cpu.flags.zf))
                    m_ip = in.immediate;
                break;
            }
            case 0xE8:                              // call rel32 (a library function patched in by the loader)
            {
                m_cpu.esp -= 4;
                m_cpu.ip = in.immediate;
                m_app->dynamic_call(in.immediate, m_cpu);
                if (m_cpu.terminate)
                    return;
                break;
            }
            case 0xC3:                              // ret
                m_cpu.esp += 4;
                return;
            default:
                unsupported(m_start, "instruction");
            }
        }
    }

private:
    // The instruction at m_start: the cached one while the guest bytes match it, else decoded again (into
    // scratch, then into the cache).
    const Instruction& instruction(Instruction& scratch)
    {
        uint16_t* slot = m_routine ? &m_routine->slots[m_start - m_range.begin] : nullptr;
        if (slot && *slot)
        {
            const Instruction& cached = m_routine->instructions[*slot - 1];
            if (memcmp(rc::host(m_start), cached.bytes, cached.length) == 0)
                return cached;
        }
        decode(scratch);
        if (!slot)
            return scratch;
        if (*slot)
        {
            m_routine->instructions[*slot - 1] = scratch;
        }
        else if (m_routine->instructions.size() < 0xffff)
        {
            m_routine->instructions.push_back(scratch);
            *slot = uint16_t(m_routine->instructions.size());
        }
        return scratch;
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

    // Decodes the instruction at m_start; traps on what the interpreter does not run, at the same point and with
    // the same message as running it would.
    void decode(Instruction& in)
    {
        m_ip = m_start;
        in.memory = false;
        in.reg = 0;
        in.immediate = 0;
        const x86::reg8 op = fetch8();
        in.op = op;
        if (op >= 0x50 && op <= 0x5F)               // push / pop r32
        {
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
                modrm(in);
                break;
            case 0x83:
                modrm(in);
                in.immediate = x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
                if (in.reg != 0 && in.reg != 7)
                    unsupported(m_start, "group 1 operation");
                break;
            case 0xC6:
                modrm(in);
                if (in.reg != 0)
                    unsupported(m_start, "C6 form");
                in.immediate = fetch8();
                if (!in.memory)
                    unsupported(m_start, "byte register");
                break;
            case 0x0F:
                if (fetch8() != 0xAF)
                    unsupported(m_start, "two-byte opcode");
                modrm(in);
                break;
            case 0x7C:
            case 0x7F:
            {
                const x86::reg32 rel = x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
                in.immediate = m_ip + rel;
                break;
            }
            case 0xE8:
            {
                const x86::reg32 rel = fetch32();
                in.immediate = m_ip + rel;
                break;
            }
            case 0xC3:
                break;
            default:
                unsupported(m_start, "instruction");
            }
        }
        in.length = uint8_t(m_ip - m_start);
        memcpy(in.bytes, rc::host(m_start), in.length);
    }

    // ModRM (and SIB, displacement) at m_ip.
    void modrm(Instruction& in)
    {
        const x86::reg8 byte = fetch8();
        const int mod = byte >> 6;
        const int rm = byte & 7;
        in.reg = (byte >> 3) & 7;
        in.base = NoRegister;
        in.index = NoRegister;
        in.scale = 0;
        in.displacement = 0;
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

    x86::reg32 effectiveAddress(const Instruction& in) const
    {
        x86::reg32 address = in.base != NoRegister ? *m_regs[in.base] : 0;
        if (in.index != NoRegister)
            address += *m_regs[in.index] << in.scale;
        return address + in.displacement;
    }

    x86::reg32 read32(const Instruction& in) const
    {
        return in.memory ? rc::ld32(effectiveAddress(in)) : *m_regs[in.rm];
    }

    x86::reg32 read32(const Instruction& in, x86::reg32 address) const
    {
        return in.memory ? rc::ld32(address) : *m_regs[in.rm];
    }

    void write32(const Instruction& in, x86::reg32 value)
    {
        if (in.memory)
            rc::st32(effectiveAddress(in), value);
        else
            *m_regs[in.rm] = value;
    }

    void write32(const Instruction& in, x86::reg32 address, x86::reg32 value)
    {
        if (in.memory)
            rc::st32(address, value);
        else
            *m_regs[in.rm] = value;
    }

    void setResultFlags(x86::reg32 result)
    {
        m_cpu.flags.zf = result == 0;
        m_cpu.flags.sf = result >> 31;
    }

    x86::reg32 add(x86::reg32 a, x86::reg32 b)
    {
        const x86::reg32 r = a + b;
        m_cpu.flags.cf = r < a;
        m_cpu.flags.of = ((a ^ r) & (b ^ r)) >> 31;
        setResultFlags(r);
        return r;
    }

    void compare(x86::reg32 a, x86::reg32 b)
    {
        const x86::reg32 r = a - b;
        m_cpu.flags.cf = a < b;
        m_cpu.flags.of = ((a ^ b) & (a ^ r)) >> 31;
        setResultFlags(r);
    }

    win32::WinApplication* m_app;
    x86::CPU&              m_cpu;
    Range                  m_range;
    Routine*               m_routine;
    x86::reg32*            m_regs[8];
    x86::reg32             m_ip = 0;
    x86::reg32             m_start = 0;
};

// Entered like a translated function: cpu.ip is the routine, cpu.esp points at the return address slot.
void runRoutine(win32::WinApplication* app, x86::CPU& cpu)
{
    Range range;
    if (!findRoutine(cpu.ip, range))
        unsupported(cpu.ip, "no loaded bank routine");
    Interpreter(app, cpu, range, routine(range)).run(cpu.ip);
}

win32::MethodPtr resolve(x86::reg32 address)
{
    Range range;
    return findRoutine(address, range) ? &runRoutine : nullptr;
}

}

void install()
{
    win32::setForeignCodeResolver(&resolve);
}

}}
