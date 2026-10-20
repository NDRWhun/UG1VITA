// Machine code inside the game's EA audio banks (Sound/**/*.abk, magic "ABKC"). Each bank entry carries a small
// x86 routine that the audio library runs from its mixer tick through a callback list (0x00652960). The bank loader
// 0x0064DF30 patches the routine's call targets to the library's functions and links the bank into the list at
// 0x00713C74. That code is not part of Speed.exe, so it has no translation: it is interpreted here. The routines
// of all 106 banks use only the instructions below (integer moves, adds, compares, pushes and calls); anything else
// traps with its address.

#include "bankcode.h"

#include <lib/foreigncode.h>
#include <platform/log.h>
#include <recomp.h>
#include <x86.h>

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

// One decoded ModRM operand: a register index or a memory address.
struct Operand
{
    bool       memory;
    int        reg;         // register index when !memory
    x86::reg32 address;     // when memory
};

class Interpreter
{
public:
    Interpreter(win32::WinApplication* app, x86::CPU& cpu, const Range& range)
        :   m_app(app)
        ,   m_cpu(cpu)
        ,   m_range(range)
        ,   m_regs{ &cpu.eax, &cpu.ecx, &cpu.edx, &cpu.ebx, &cpu.esp, &cpu.ebp, &cpu.esi, &cpu.edi }
    {
    }

    void run(x86::reg32 ip)
    {
        m_ip = ip;
        for (;;)
        {
            m_start = m_ip;
            if (m_ip < m_range.begin || m_ip >= m_range.end)
                unsupported(m_ip, "jump out of the routine");
            const x86::reg8 op = fetch8();
            if (op >= 0x50 && op <= 0x57)           // push r32
            {
                const x86::reg32 value = *m_regs[op & 7];
                m_cpu.esp -= 4;
                rc::st32(m_cpu.esp, value);
                continue;
            }
            if (op >= 0x58 && op <= 0x5F)           // pop r32
            {
                const x86::reg32 value = rc::ld32(m_cpu.esp);
                m_cpu.esp += 4;
                *m_regs[op & 7] = value;
                continue;
            }
            switch (op)
            {
            case 0x89:                              // mov r/m32, r32
            {
                int reg;
                const Operand dst = modrm(reg);
                write32(dst, *m_regs[reg]);
                break;
            }
            case 0x8B:                              // mov r32, r/m32
            {
                int reg;
                const Operand src = modrm(reg);
                *m_regs[reg] = read32(src);
                break;
            }
            case 0x03:                              // add r32, r/m32
            {
                int reg;
                const Operand src = modrm(reg);
                *m_regs[reg] = add(*m_regs[reg], read32(src));
                break;
            }
            case 0x3B:                              // cmp r32, r/m32
            {
                int reg;
                const Operand src = modrm(reg);
                compare(*m_regs[reg], read32(src));
                break;
            }
            case 0x83:                              // add / cmp r/m32, imm8
            {
                int kind;
                const Operand dst = modrm(kind);
                const x86::reg32 imm = x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
                if (kind == 0)
                    write32(dst, add(read32(dst), imm));
                else if (kind == 7)
                    compare(read32(dst), imm);
                else
                    unsupported(m_start, "group 1 operation");
                break;
            }
            case 0xC6:                              // mov r/m8, imm8
            {
                int kind;
                const Operand dst = modrm(kind);
                if (kind != 0)
                    unsupported(m_start, "C6 form");
                const x86::reg8 imm = fetch8();
                if (dst.memory)
                    rc::st8(dst.address, imm);
                else
                    unsupported(m_start, "byte register");
                break;
            }
            case 0x0F:
            {
                if (fetch8() != 0xAF)
                    unsupported(m_start, "two-byte opcode");
                int reg;                            // imul r32, r/m32
                const Operand src = modrm(reg);
                const int64_t product = int64_t(x86::sreg32(*m_regs[reg])) * x86::sreg32(read32(src));
                const x86::reg32 low = x86::reg32(product);
                m_cpu.flags.cf = m_cpu.flags.of = (product != int64_t(x86::sreg32(low)));
                *m_regs[reg] = low;
                break;
            }
            case 0x7C:                              // jl rel8
            case 0x7F:                              // jg rel8
            {
                const x86::reg32 rel = x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
                const x86::reg32 target = m_ip + rel;
                const bool less = m_cpu.flags.sf != m_cpu.flags.of;
                if (op == 0x7C ? less : (!less && !m_cpu.flags.zf))
                    m_ip = target;
                break;
            }
            case 0xE8:                              // call rel32 (a library function patched in by the loader)
            {
                const x86::reg32 target = m_ip + 4 + rc::ld32(m_ip);
                m_ip += 4;
                m_cpu.esp -= 4;
                m_cpu.ip = target;
                m_app->dynamic_call(target, m_cpu);
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

    // ModRM (and SIB, displacement) at m_ip; reg receives the reg field.
    Operand modrm(int& reg)
    {
        const x86::reg8 byte = fetch8();
        const int mod = byte >> 6;
        const int rm = byte & 7;
        reg = (byte >> 3) & 7;
        if (mod == 3)
            return Operand{ false, rm, 0 };
        x86::reg32 address = 0;
        if (rm == 4)
        {
            const x86::reg8 sib = fetch8();
            const int scale = sib >> 6;
            const int index = (sib >> 3) & 7;
            const int base = sib & 7;
            if (base == 5 && mod == 0)
                address = fetch32();
            else
                address = *m_regs[base];
            if (index != 4)
                address += *m_regs[index] << scale;
        }
        else if (rm == 5 && mod == 0)
        {
            address = fetch32();
        }
        else
        {
            address = *m_regs[rm];
        }
        if (mod == 1)
            address += x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
        else if (mod == 2)
            address += fetch32();
        return Operand{ true, 0, address };
    }

    x86::reg32 read32(const Operand& operand) const
    {
        return operand.memory ? rc::ld32(operand.address) : *m_regs[operand.reg];
    }

    void write32(const Operand& operand, x86::reg32 value)
    {
        if (operand.memory)
            rc::st32(operand.address, value);
        else
            *m_regs[operand.reg] = value;
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
    Interpreter(app, cpu, range).run(cpu.ip);
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
