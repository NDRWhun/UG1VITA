"""Function discovery and decoding for an MSVC-built executable.

Recursive descent from known entries; nothing is decoded by linear sweep except the gap check at the end.

- Entries: the PE entry point, direct call targets, relocated immediates that point into code (function pointers
  pushed or stored by code), relocated dwords outside code that point into code (function pointer tables, SEH scope
  tables), and code found in the gaps between everything reachable (unreferenced functions).
- Every function is decoded on its own from its entry: all code reachable through jumps belongs to it, jumps to other
  entries are tail calls. Code shared by several entries is decoded into each of them.
- Switches: MSVC emits `jmp dword ptr [reg*4 + table]` with the table (and the byte index table of two-level
  switches) inline in the code section after the function. A table holds the relocated dwords from its start; it ends
  at the case count of the preceding `cmp reg, n; ja` bound or at the first dword that is not a relocated code
  pointer, whichever comes first.
- Instruction bytes covered by data (tables) are never decoded as code.
"""

import bisect

from capstone import x86 as X

from .subroutine import FakeJmpInstruction, FakeTrapInstruction, Subroutine

JCC_PREFIX = 'j'
LOOPS = ('loop', 'loope', 'loopne', 'jecxz', 'jcxz')
# Instructions after which control never falls through.
STOPS = ('ret', 'retf', 'jmp', 'int3', 'hlt', 'ud2', 'iretd', 'ljmp')
PADDING = (0xcc, 0x90, 0x00)
# Instructions no user-mode C or CRT code of this game executes: a candidate entry whose code contains one is data.
IMPLAUSIBLE = {'in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd', 'sldt', 'str', 'lds', 'les', 'lfs',
               'lgs', 'lss', 'into', 'iret', 'iretd', 'retf', 'hlt', 'lcall', 'ljmp', 'arpl', 'bound', 'aaa', 'aas',
               'daa', 'das', 'aam', 'aad', 'salc', 'cli', 'sti', 'int1', 'lar', 'lsl', 'lgdt', 'sgdt', 'lidt', 'sidt',
               'enter'}
# Entries found this way are code by construction; all others are checked (plausible()).
TRUSTED = ('entry point', 'call', 'configured')


def inside(starts, items, address):
    """True when address lies in one of the items (start -> end; starts sorted)."""
    i = bisect.bisect_right(starts, address) - 1
    return i >= 0 and items[starts[i]] > address


def mnemonic_of(ins):
    return ins.mnemonic.split()[-1]


def is_jump(m):
    return m == 'jmp' or m in LOOPS or (m.startswith(JCC_PREFIX) and m != 'jmp')


SUBREGISTERS = {'al': 'eax', 'ah': 'eax', 'ax': 'eax', 'cl': 'ecx', 'ch': 'ecx', 'cx': 'ecx', 'dl': 'edx',
                'dh': 'edx', 'dx': 'edx', 'bl': 'ebx', 'bh': 'ebx', 'bx': 'ebx', 'si': 'esi', 'di': 'edi',
                'bp': 'ebp', 'sp': 'esp'}


def full_register(name):
    return SUBREGISTERS.get(name, name)


def operand_key(ins, op):
    """Comparable text of an operand: the register name, or the memory operand as capstone prints it."""
    if op.type == X.X86_OP_REG:
        return ins.reg_name(op.reg)
    if op.type == X.X86_OP_MEM:
        m = op.mem
        return ('mem', m.segment, m.base, m.index, m.scale, m.disp)
    return ('imm', op.imm)


def ins_writes(ins):
    """Full registers and memory operands an instruction writes."""
    _, written = ins.regs_access()
    result = set(full_register(ins.reg_name(r)) for r in written)
    for op in ins.operands:
        if op.type == X.X86_OP_MEM and op.access & 2:     # CS_AC_WRITE
            result.add(operand_key(ins, op))
    return result


class Table:
    """A jump table: case targets of one `jmp [reg*4 + address]`."""
    def __init__(self, address, end, targets, bounded):
        self.address = address
        self.end = end
        self.targets = targets
        self.bounded = bounded      # the case count came from a cmp/ja bound


class Discovery:
    def __init__(self, image, cs, log=print):
        self.image = image
        self.cs = cs
        self.log = log
        self.decoded = {}           # address -> capstone instruction, or None when undecodable
        self.tables = {}            # table address -> Table
        self.index_tables = {}      # byte index table address -> length (None: unknown)
        self.data_refs = set()      # code-section addresses read as data by instructions
        self.entries = set()
        self.origin = {}            # entry -> how it was found
        self.code = set()           # instruction addresses of accepted functions
        self.rejected = {}          # candidate entry -> origin, for candidates that decode as data
        self.stats = {}

    # ---- decoding ------------------------------------------------------------------------------------------------
    def instruction(self, address):
        if address not in self.decoded:
            ins = None
            if self.image.is_code(address):
                for ins in self.cs.disasm(self.image.read(address, 16), address, count=1):
                    break
            self.decoded[address] = ins
        return self.decoded[address]

    def switch_bound(self, jmp, function):
        """Case count from the code before a table jump, following the index value back through moves:
        `cmp v, n; ja default` or `and r, n` bounds it; `mov r8, byte ptr [x + table]` (after `xor r, r`) reads it
        from a byte index table, whose length the bound gives. None when no bound is found."""
        tracked = jmp.reg_name(jmp.operands[0].mem.index)
        previous = sorted(a for a in function if a < jmp.address)[-10:]
        index_table = None
        limit_register = None       # cmp against a register: its value comes from mov r, imm
        for a in reversed(previous):
            ins = function[a]
            if ins is None:
                return None
            m = mnemonic_of(ins)
            ops = ins.operands
            first = operand_key(ins, ops[0]) if ops else None
            if m in ('ja', 'jae', 'jb', 'jbe', 'jg', 'jge', 'jl', 'jle', 'je', 'jne'):
                continue
            if m == 'xor' and len(ops) == 2 and first == operand_key(ins, ops[1]):
                continue
            if m in ('mov', 'movzx') and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_MEM \
                    and ops[1].size == 1 and full_register(first) == tracked and index_table is None:
                table = ops[1].mem.disp & 0xffffffff
                registers = [r for r in (ops[1].mem.base, ops[1].mem.index) if r]
                if not self.image.is_code(table) or len(registers) != 1:
                    return None
                index_table = table
                tracked = ins.reg_name(registers[0])
                continue
            if m == 'mov' and ops[0].type == X.X86_OP_REG and first == tracked:
                tracked = operand_key(ins, ops[1])
                continue
            if m == 'cmp' and ops[1].type == X.X86_OP_REG and first == tracked and limit_register is None:
                limit_register = ins.reg_name(ops[1].reg)
                continue
            limit = None
            if m in ('cmp', 'and') and ops[1].type == X.X86_OP_IMM and first == tracked and limit_register is None:
                limit = ops[1].imm & 0xffffffff
            elif m == 'mov' and ops[1].type == X.X86_OP_IMM and first == limit_register:
                limit = ops[1].imm & 0xffffffff
            if limit is not None:
                count = limit + 1
                if index_table is not None:
                    self.index_tables[index_table] = count
                    data = self.image.read(index_table, count)
                    return max(data) + 1 if data else None
                return count
            if tracked in ins_writes(ins) or limit_register in ins_writes(ins):
                return None
        return None

    def table(self, jmp, function):
        op = jmp.operands[0]
        address = op.mem.disp & 0xffffffff
        if address in self.tables:
            return self.tables[address]
        bound = self.switch_bound(jmp, function)
        # Leading entries may be unused (the index never takes those values): they are not relocated.
        targets = []
        a = address
        while bound is not None and a not in self.image.relocations and (a - address) // 4 < bound:
            a += 4
        while a in self.image.relocations and (bound is None or (a - address) // 4 < bound):
            if a != address and (a in self.tables or a in self.index_tables or a in self.data_refs):
                break
            value = self.image.dword(a)
            if not self.image.is_code(value):
                break
            targets.append(value)
            a += 4
        if bound is not None and (a - address) // 4 < bound:
            self.log('warning: table 0x%x: %d relocated entries, the switch bound says %d'
                     % (address, (a - address) // 4, bound))
        if not targets and address not in self.image.relocations:
            # Negative index (`sub r, n; jb` then `jmp [r*4 + end]`): the entries lie just below the address.
            b = address - 4
            while b in self.image.relocations and self.image.is_code(self.image.dword(b)):
                targets.insert(0, self.image.dword(b))
                b -= 4
            if targets:
                self.log('note: table 0x%x indexed downwards from 0x%x, %d entries' % (b + 4, address, len(targets)))
        t = Table(address, a, targets, bound is not None)
        self.tables[address] = t
        return t

    def is_table_jump(self, ins):
        if mnemonic_of(ins) != 'jmp' or ins.operands[0].type != X.X86_OP_MEM:
            return False
        m = ins.operands[0].mem
        return not m.base and m.index and m.scale == 4 and not m.segment and self.image.is_code(m.disp & 0xffffffff)

    # ---- one function --------------------------------------------------------------------------------------------
    def walk(self, entry, tails=frozenset()):
        """Instructions reachable from entry (address -> instruction or None) and the references found on the way.
        Jumps to addresses in tails end the path (tail calls)."""
        function = {}
        calls = set()
        code_refs = set()
        work = [entry]
        while work:
            address = work.pop()
            while address not in function:
                ins = self.instruction(address)
                function[address] = ins
                if ins is None:
                    break
                m = mnemonic_of(ins)
                for op in ins.operands:
                    if op.type == X.X86_OP_MEM and op.mem.disp and not op.mem.segment:
                        disp = op.mem.disp & 0xffffffff
                        if self.image.is_code(disp) and not (m in ('jmp', 'call') and self.is_table_jump(ins)):
                            self.data_refs.add(disp)
                    if op.type == X.X86_OP_IMM and not is_jump(m) and m != 'call':
                        value = op.imm & 0xffffffff
                        if self.image.is_code(value) and ins.address + ins.imm_offset in self.image.relocations:
                            code_refs.add(value)
                if m == 'call':
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        calls.add(op.imm & 0xffffffff)
                elif is_jump(m):
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        target = op.imm & 0xffffffff
                        if target == entry or target not in tails:
                            work.append(target)
                    elif self.is_table_jump(ins):
                        self.data_refs.discard(op.mem.disp & 0xffffffff)
                        for target in self.table(ins, function).targets:
                            work.append(target)
                    if m == 'jmp':
                        break
                if m in STOPS:
                    break
                address = ins.address + ins.size
                if address in tails and address != entry:
                    break
        return function, calls, code_refs

    # ---- whole program -------------------------------------------------------------------------------------------
    def data_items(self):
        """Tables and index tables: start -> end."""
        items = {}
        for t in self.tables.values():
            items[t.address] = max(items.get(t.address, 0), t.end)
        for a, count in self.index_tables.items():
            items[a] = max(items.get(a, 0), a + (count or 1))
        return items

    def in_data(self, address):
        items = self.data_items()
        return address in self.data_refs or inside(sorted(items), items, address)

    def covered(self):
        """Bytes covered by the code of accepted functions and by tables: start -> end."""
        items = self.data_items()
        for a in self.code:
            items[a] = max(items.get(a, 0), a + self.decoded[a].size)
        return items

    def plausible(self, function):
        """False when decoded code looks like data: undecodable bytes, instructions from IMPLAUSIBLE, or a relocated
        dword that is not an instruction's displacement or immediate."""
        for a, ins in function.items():
            if ins is None or mnemonic_of(ins) in IMPLAUSIBLE:
                return False
            fields = set()
            if ins.disp_size:
                fields.add(a + ins.disp_offset)
            if ins.imm_size:
                fields.add(a + ins.imm_offset)
            for r in range(a - 3, a + ins.size):
                if r in self.image.relocations and r not in fields:
                    return False
        return True

    def closure(self, seeds, origin):
        """Walks the new entries among seeds and everything they reach; returns the number of new entries. Entries
        that are not TRUSTED and do not look like code are rejected and leave no tables or data references."""
        work = [(a, origin) for a in reversed(seeds)]
        count = 0
        while work:
            entry, why = work.pop()
            if entry in self.entries or entry in self.rejected or self.in_data(entry):
                continue
            saved = (dict(self.tables), dict(self.index_tables), set(self.data_refs))
            function, calls, code_refs = self.walk(entry)
            if why not in TRUSTED and not self.plausible(function):
                self.tables, self.index_tables, self.data_refs = saved
                self.rejected[entry] = why
                continue
            self.entries.add(entry)
            self.origin[entry] = why
            count += 1
            self.code.update(a for a, ins in function.items() if ins is not None)
            work += [(t, 'call') for t in sorted(calls, reverse=True)]
            work += [(t, 'pointer') for t in sorted(code_refs, reverse=True)]
        return count

    def gaps(self):
        """Code-section ranges that no accepted instruction or table covers, without leading padding."""
        items = sorted(self.covered().items())
        result = []
        for section in self.image.code_sections:
            position = section.address
            end = section.address + section.virtual_size
            for start, stop in items + [(end, end)]:
                if start < section.address or start > end:
                    continue
                if start > position:
                    result.append((position, start))
                position = max(position, stop)
        trimmed = []
        for start, stop in result:
            data = self.image.read(start, stop - start)
            first = next((i for i, b in enumerate(data) if b not in PADDING), None)
            if first is not None:
                trimmed.append((start + first, stop))
        return trimmed

    def decodes_cleanly(self, start, stop):
        """True when every path from start decodes into valid instructions inside [start, stop) or reaches a known
        entry. A rejected candidate leaves no tables or data references behind."""
        saved = (dict(self.tables), dict(self.index_tables), set(self.data_refs))
        function, _, _ = self.walk(start, frozenset(self.entries))
        clean = all(ins is not None and start <= a < stop for a, ins in function.items()) and self.plausible(function)
        if not clean:
            self.tables, self.index_tables, self.data_refs = saved
        return clean

    def discover(self, extra_entries=()):
        image = self.image
        self.closure([image.entry_point], 'entry point')
        self.closure(extra_entries, 'configured')
        self.closure([image.dword(a) for a in sorted(image.relocations)
                      if not image.is_code(a) and image.is_code(image.dword(a))], 'data pointer')
        while True:
            # Code no reference reaches (dead functions), then relocated code pointers stored in the code section
            # outside instructions and tables.
            found = [start for start, stop in self.gaps()
                     if start not in self.data_refs and start not in self.rejected
                     and self.decodes_cleanly(start, stop)]
            new = self.closure(found, 'unreferenced')
            covered = self.covered()
            starts = sorted(covered)
            loose = [image.dword(a) for a in sorted(image.relocations)
                     if image.is_code(a) and not inside(starts, covered, a) and image.is_code(image.dword(a))]
            new += self.closure(loose, 'code-section pointer')
            if not new:
                break
        self.drop_data_entries()

    def drop_data_entries(self):
        """Entries found before the table that covers them was known."""
        for e in sorted(self.entries):
            if self.in_data(e):
                self.log('warning: entry 0x%x (%s) lies in a jump table, dropped' % (e, self.origin[e]))
                self.entries.discard(e)

    # ---- final functions -----------------------------------------------------------------------------------------
    def subroutine(self, entry):
        """The decoded function as a Subroutine (instructions in address order)."""
        tails = frozenset(self.entries)
        function, calls, _ = self.walk(entry, tails)
        sub = Subroutine(entry)
        ordered = sorted(function)
        instructions = []
        for i, address in enumerate(ordered):
            ins = function[address]
            if ins is None:
                ins = FakeTrapInstruction(address)
                instructions.append(ins)
                continue
            instructions.append(ins)
            m = mnemonic_of(ins)
            if m == 'call' and ins.operands[0].type == X.X86_OP_IMM:
                sub.calls.append((ins.address, ins.operands[0].imm & 0xffffffff))
            if is_jump(m):
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    target = op.imm & 0xffffffff
                    if target in function and (target == entry or target not in tails):
                        sub.static_labels.append((ins.address, target))
                elif self.is_table_jump(ins):
                    ins.potential_destinations = self.tables[op.mem.disp & 0xffffffff].targets
                    for target in ins.potential_destinations:
                        sub.dynamic_labels.append((ins.address, target))
            if m in STOPS:
                continue
            fallthrough = ins.address + ins.size
            following = ordered[i + 1] if i + 1 < len(ordered) else None
            if following != fallthrough:
                # Falls into another entry (a tail call) or into code that is not next in address order.
                instructions.append(FakeJmpInstruction(ins.address, fallthrough))
                if fallthrough in function and fallthrough not in tails:
                    sub.static_labels.append((ins.address, fallthrough))
        sub.instructions = instructions
        return sub
