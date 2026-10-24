"""Function discovery and decoding for an MSVC-built executable without base relocations.

Recursive descent from verified entries; no linear sweep produces code.

- Trusted entries: the PE entry point, configured addresses and direct call targets in accepted code.
- Pointer candidates: immediates in accepted code that point into the code section (`push offset f`,
  `mov r/m, offset f`), aligned dwords in the other sections that point into it (vtables, callback and
  function-pointer tables, EH tables), and the dwords of objects at misaligned addresses that accepted code takes
  as an immediate (object_fields(): the CRT x87 dispatch descriptors `mov edx, offset sinh_table`). Without
  relocations any of these may be a plain number, so each one is validated before it becomes an entry
  (validate()). An entry of a table of code pointers may start at or join an instruction of accepted code (CRT
  dispatch targets share tails with their neighbours): it becomes an entry there, like a tail-call target.
- The gaps between functions hold filler: fragments of nearby code (copies with shifted relative branches) and dead
  call sequences. No entry is ever taken from a gap by scanning; a pointer into a gap must pass validation like any
  other.
- Calls to functions that never return end the path (no_return()); the bytes after them are padding or filler.
- Every function is decoded on its own from its entry: all code reachable through jumps belongs to it, jumps to other
  entries are tail calls. Code shared by several entries is decoded into each of them.
- Switches: MSVC emits `jmp dword ptr [reg*4 + table]` with the table (and the byte index table of two-level
  switches) inline in the code section after the function. A table ends at the case count of the preceding
  `cmp reg, n; ja` bound, else at the first dword that is not a code address or that meets other code or data.
- Instruction bytes covered by data (tables) are never decoded as code.
"""

import bisect

from capstone import x86 as X

from .subroutine import FakeJmpInstruction, FakeTrapInstruction, Subroutine

JCC_PREFIX = 'j'
LOOPS = ('loop', 'loope', 'loopne', 'jecxz', 'jcxz')
# Instructions after which control never falls through.
STOPS = ('ret', 'retf', 'jmp', 'int3', 'hlt', 'ud2', 'iretd', 'ljmp')
PADDING = (0xcc, 0x90)
# Instructions no user-mode C or CRT code of this game executes: a candidate entry whose code contains one is data.
IMPLAUSIBLE = {'in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd', 'sldt', 'str', 'lds', 'les', 'lfs',
               'lgs', 'lss', 'into', 'iret', 'iretd', 'retf', 'hlt', 'lcall', 'ljmp', 'arpl', 'bound', 'aaa', 'aas',
               'daa', 'das', 'aam', 'aad', 'salc', 'cli', 'sti', 'int1', 'lar', 'lsl', 'lgdt', 'sgdt', 'lidt', 'sidt',
               'enter', 'icebp'}
# Instructions compiled code uses with esp as the destination; any other (dec esp, ...) marks data.
ESP_WRITERS = ('add', 'sub', 'and', 'mov', 'lea', 'push', 'pop', 'cmp', 'test', 'xchg')
# Entries found this way are code by construction; all others are validated.
TRUSTED = ('entry point', 'call', 'configured', 'jump')
# Imports that never return.
NO_RETURN_IMPORTS = ('ExitProcess', 'ExitThread', 'FreeLibraryAndExitThread', 'FatalAppExitA', 'FatalExit')
# Unbounded jump tables stop after this many entries.
MAX_TABLE = 1024
# Fields of a misaligned object that code refers to are read up to this many bytes from its address.
OBJECT_SIZE = 256
# A table of code pointers: at least this many consecutive dwords that hold code addresses.
POINTER_RUN = 3

# Byte classes of the code section map.
FREE, START, BODY, DATA = 0, 1, 2, 3


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
    def __init__(self, address, end, targets, bounded, start=None):
        self.address = address      # the jump's displacement
        self.start = address if start is None else start    # first entry (below address for negative indexes)
        self.end = end
        self.targets = targets
        self.bounded = bounded      # the case count came from a cmp/ja bound


class Discovery:
    def __init__(self, image, cs, log=print, no_return=()):
        """no_return: functions declared never to return (those that leave through a pointer or a flag the
        analysis cannot follow, such as the CRT's exit)."""
        self.image = image
        self.cs = cs
        self.log = log
        self.decoded = {}           # address -> capstone instruction, or None when undecodable
        self.code_section = image.code_sections[0] if image.code_sections else None
        # Import address table slots of imports that never return.
        self.no_return_slots = set(i.iat_address for i in image.imports if i.name in NO_RETURN_IMPORTS)
        self.declared_no_return = set(no_return)
        self.no_return = set(no_return)     # entries that never return
        self.reset()

    def reset(self):
        """Forgets everything but the decoded instructions and the no-return entries."""
        self.tables = {}            # table address -> Table
        self.index_tables = {}      # byte index table address -> length (None: unknown)
        self.data_refs = set()      # code-section addresses read as data by instructions
        self.entries = set()
        self.origin = {}            # entry -> how it was found
        self.code = set()           # instruction addresses of accepted functions
        self.rejected = {}          # candidate entry -> (origin, reason), for candidates that are not code
        self.code_refs = {}         # code address -> instruction addresses whose immediate holds it
        self.data_pointers = {}     # code address -> addresses of aligned dwords outside code that hold it
        self.string_pointers = {}   # the same for dwords in text, which are not candidates
        self.jump_targets = set()   # targets of direct unconditional jumps in accepted code
        self.flow_targets = set()   # addresses reached by falling through, a conditional jump or a switch
        self.owner = {}             # instruction address -> the entry whose walk accepted it first
        self.conflicts = []         # (entry, instructions its code overlaps at other boundaries)
        self.trusted_overlaps = []  # (entry, owners) overlaps not resolved by dropping a pointer entry
        self.displaced = set()      # data pointer entries in the way of an accepted code pointer
        self.objects = {}           # misaligned non-code addresses held by immediates -> instruction addresses
        s = self.code_section
        self.map = bytearray(max(s.virtual_size, s.raw_size) if s else 0)

    # ---- decoding ------------------------------------------------------------------------------------------------
    def instruction(self, address):
        if address not in self.decoded:
            ins = None
            if self.image.is_code(address):
                for ins in self.cs.disasm(self.image.read(address, 16), address, count=1):
                    break
            self.decoded[address] = ins
        return self.decoded[address]

    def map_index(self, address):
        s = self.code_section
        return address - s.address if s and s.address <= address < s.address + len(self.map) else None

    def mark(self, start, size, kind):
        i = self.map_index(start)
        if i is None:
            return
        if kind == START:
            if self.map[i] != DATA:
                self.map[i] = START
            for j in range(i + 1, min(i + size, len(self.map))):
                if self.map[j] == FREE:
                    self.map[j] = BODY
        else:
            for j in range(i, min(i + size, len(self.map))):
                self.map[j] = kind

    def byte_class(self, address):
        i = self.map_index(address)
        return self.map[i] if i is not None else None

    def instruction_at(self, address):
        """The accepted instruction whose bytes include address, or None."""
        for s in range(address, address - 16, -1):
            if s in self.code and s + self.decoded[s].size > address:
                return s
        return None

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

    def table_entry(self, address, table_start, function):
        """The code address in the dword at address when it can be a table entry, else None. The dword must not
        overlap accepted code or the instructions decoded so far in function."""
        if address != table_start and (address in self.tables or address in self.index_tables
                                       or address in self.data_refs):
            return None
        if any(self.byte_class(address + k) in (START, BODY) for k in range(4)):
            return None
        for s in range(address - 15, address + 4):
            ins = function.get(s)
            if ins is not None and s + ins.size > address:
                return None
        value = self.image.dword(address)
        if value is None or not self.image.is_code(value) or self.instruction(value) is None:
            return None
        return value

    def table(self, jmp, function):
        op = jmp.operands[0]
        address = op.mem.disp & 0xffffffff
        if address in self.tables:
            return self.tables[address]
        bound = self.switch_bound(jmp, function)
        targets = []
        start = a = address
        limit = bound if bound is not None else MAX_TABLE
        while (a - address) // 4 < limit:
            value = self.table_entry(a, start, function)
            if value is None:
                # Leading entries the index never takes may overlap the code before the table (memmove's
                # `jmp [eax*4 + T]` with eax 1..3).
                if bound is not None and not targets:
                    a += 4
                    start = a
                    continue
                break
            targets.append(value)
            a += 4
        if bound is not None and (a - address) // 4 < bound:
            self.log('warning: table 0x%x: %d code entries, the switch bound says %d'
                     % (address, len(targets), bound))
        if not targets:
            # Negative index (`sub r, n; jb` then `jmp [r*4 + end]`): the entries lie just below the address.
            start = a = address
            b = address - 4
            while len(targets) < (bound or MAX_TABLE):
                value = self.table_entry(b, b, function)
                if value is None:
                    break
                targets.insert(0, value)
                start = b
                b -= 4
            if targets:
                self.log('note: table 0x%x indexed downwards from 0x%x, %d entries' % (start, address, len(targets)))
        t = Table(address, a, targets, bound is not None, start)
        self.tables[address] = t
        return t

    def is_table_jump(self, ins):
        if mnemonic_of(ins) != 'jmp' or ins.operands[0].type != X.X86_OP_MEM:
            return False
        m = ins.operands[0].mem
        return not m.base and m.index and m.scale == 4 and not m.segment and self.image.is_code(m.disp & 0xffffffff)

    def calls_no_return(self, ins):
        """True for a call to a function or import that never returns."""
        op = ins.operands[0]
        if op.type == X.X86_OP_IMM:
            return (op.imm & 0xffffffff) in self.no_return
        if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index and not op.mem.segment:
            return (op.mem.disp & 0xffffffff) in self.no_return_slots
        return False

    # ---- one function --------------------------------------------------------------------------------------------
    def walk(self, entry, tails=frozenset()):
        """Instructions reachable from entry (address -> instruction or None), the direct call targets and the
        code addresses held by immediates. Jumps to addresses in tails end the path (tail calls)."""
        function = {}
        calls = set()
        code_refs = {}
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
                        if op.size == 4 and self.image.is_code(value):
                            code_refs.setdefault(value, set()).add(ins.address)
                if m == 'call':
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        calls.add(op.imm & 0xffffffff)
                    if self.calls_no_return(ins):
                        break
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
            if t.end > t.start:
                items[t.start] = max(items.get(t.start, 0), t.end)
        for a, count in self.index_tables.items():
            items[a] = max(items.get(a, 0), a + (count or 1))
        return items

    def in_data(self, address):
        return address in self.data_refs or self.byte_class(address) == DATA

    def covered(self):
        """Bytes covered by the code of accepted functions and by tables: start -> end."""
        items = self.data_items()
        for a in self.code:
            items[a] = max(items.get(a, 0), a + self.decoded[a].size)
        return items

    def mark_data(self):
        for start, end in self.data_items().items():
            self.mark(start, end - start, DATA)

    def accept(self, entry, why, function):
        self.entries.add(entry)
        self.origin[entry] = why
        overlapped = set()
        for a, ins in function.items():
            if ins is None or a in self.code:
                continue
            for k in range(ins.size):
                if self.byte_class(a + k) in (START, BODY):
                    overlapped.add(self.instruction_at(a + k))
            self.code.add(a)
            self.owner[a] = entry
            self.mark(a, ins.size, START)
            m = mnemonic_of(ins)
            if is_jump(m):
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    (self.jump_targets if m == 'jmp' else self.flow_targets).add(op.imm & 0xffffffff)
                elif self.is_table_jump(ins):
                    self.flow_targets.update(self.tables[op.mem.disp & 0xffffffff].targets)
            if m not in STOPS and not (m == 'call' and self.calls_no_return(ins)):
                self.flow_targets.add(a + ins.size)
        self.mark_data()
        overlapped.discard(None)
        if overlapped:
            self.conflicts.append((entry, overlapped))

    def tail_targets(self):
        """Code reached only by unconditional jumps, never by falling through, a conditional jump or a switch:
        the entry of a function that other functions tail-call. Splitting there is always sound (the jump becomes
        a tail call)."""
        return sorted(t for t in self.jump_targets - self.flow_targets - self.entries if t in self.code)

    def note_objects(self, function):
        """Records the misaligned addresses outside the code section that immediates in an accepted function hold
        (object addresses)."""
        for ins in function.values():
            if ins is None:
                continue
            for op in ins.operands:
                if op.type != X.X86_OP_IMM or op.size != 4:
                    continue
                value = op.imm & 0xffffffff
                s = self.image.section_of(value)
                if value % 4 and s is not None and not s.code:
                    self.objects.setdefault(value, set()).add(ins.address)

    def object_fields(self):
        """Code addresses in the dwords of the misaligned objects code refers to, which the aligned scan does not
        read: the first run of fields that hold code addresses, at 4-byte steps from the object's address within
        OBJECT_SIZE bytes. Returns code address -> field addresses."""
        fields = {}
        for start in self.objects:
            run = False
            for a in range(start, start + OBJECT_SIZE, 4):
                value = self.image.dword(a)
                if value is not None and self.image.is_code(value):
                    fields.setdefault(value, set()).add(a)
                    run = True
                elif run:
                    break
        return fields

    def in_pointer_table(self, entry):
        """True when a dword holding entry lies in a table of code pointers: at least POINTER_RUN consecutive dwords
        that each hold an accepted entry or an instruction start of accepted code, two of them other entries."""
        def code_start(value):
            return value is not None and (value in self.entries or self.byte_class(value) == START)

        for address in self.data_pointers.get(entry, ()):
            low = high = address
            while code_start(self.image.dword(low - 4)):
                low -= 4
            while code_start(self.image.dword(high + 4)):
                high += 4
            values = [self.image.dword(a) for a in range(low, high + 4, 4)]
            if len(values) >= POINTER_RUN and len(set(v for v in values if v in self.entries) - {entry}) >= 2:
                return True
        return False

    def validate(self, entry, why):
        """None when a pointer candidate is a function entry, else the reason it is not. Checks: the address starts no
        instruction of an accepted function and lies in no instruction or table; every path from it decodes into
        plausible instructions that stay off accepted code (except at entries: tail calls) and off tables; no
        path falls through into an entry. A code pointer comes from accepted code and outranks a
        data pointer entry in its way: that entry is marked displaced (dropped on the next round). An entry of a
        table of code pointers (in_pointer_table()) may start in accepted code, join it at its instruction starts and
        fall through into an entry: hand-written CRT dispatch targets share their tails."""
        displaced = set()
        table = why == 'data pointer' and self.in_pointer_table(entry)

        def displaces(address):
            """True when the accepted code at address belongs to a data pointer entry this code pointer outranks."""
            start = self.instruction_at(address) if address not in self.entries else address
            owner = address if address in self.entries else self.owner.get(start)
            if why == 'code pointer' and owner is not None and self.origin.get(owner) == 'data pointer':
                displaced.add(owner)
                return True
            return False

        cls = self.byte_class(entry)
        if cls in (START, BODY) and not (cls == START and table) and not displaces(entry):
            return 'interior' if cls == START else 'misaligned'
        if cls == DATA or entry in self.data_refs:
            return 'data'
        function, _, _ = self.walk(entry, frozenset(self.entries))
        if any(ins is None for ins in function.values()):
            return 'undecodable'
        ordered = sorted(function)
        if any(a + function[a].size > b for a, b in zip(ordered, ordered[1:])):
            return 'overlaps'
        for a, ins in function.items():
            if table and a in self.code:
                continue
            m = mnemonic_of(ins)
            # A function never starts in int3/nop padding (text like "ESE\0" = 0x00455345 points there).
            if m in IMPLAUSIBLE or (m in ('int3', 'nop') and a == entry):
                return 'implausible'
            if ins.operands and ins.operands[0].type == X.X86_OP_REG and ins.reg_name(ins.operands[0].reg) == 'esp' \
                    and m not in ESP_WRITERS:
                return 'implausible'
            for k in range(ins.size):
                c = self.byte_class(a + k)
                if c == DATA:
                    return 'data'
                if c is not None and c != FREE and not displaces(a + k):
                    return 'merges' if k == 0 and c == START else 'overlaps'
            if m in STOPS:
                continue
            if m == 'call' and self.calls_no_return(ins):
                continue
            following = a + ins.size
            if following in self.entries and not table and not displaces(following):
                return 'runs into an entry'
        self.displaced |= displaced
        return None

    def closure(self, seeds, origin):
        """Walks the new entries among seeds and everything they reach; returns the number of new entries.
        Entries that are not TRUSTED must pass validate(); rejected ones leave no tables or data references."""
        work = [(a, origin) for a in reversed(seeds)]
        count = 0
        while work:
            entry, why = work.pop()
            if entry in self.entries:
                continue
            if why not in TRUSTED and (entry in self.rejected or entry in self.blacklist):
                continue
            self.rejected.pop(entry, None)
            if why not in TRUSTED:
                saved = (dict(self.tables), dict(self.index_tables), set(self.data_refs))
                reason = self.validate(entry, why)
                self.tables, self.index_tables, self.data_refs = saved
                if reason:
                    self.rejected[entry] = (why, reason)
                    continue
            function, calls, code_refs = self.walk(entry)
            self.accept(entry, why, function)
            self.note_objects(function)
            count += 1
            for value, sites in code_refs.items():
                self.code_refs.setdefault(value, set()).update(sites)
            work += [(t, 'call') for t in sorted(calls, reverse=True)]
        return count

    def scan_data_pointers(self):
        """Aligned dwords outside the code section that hold a code address, except text: a dword right after four
        printable characters (b'OWN\\0' reads as 0x004e574f), or UTF-16 characters after UTF-16 characters."""
        def printable(b):
            return 32 <= b < 127

        def utf16(d):
            return printable(d[0]) and not d[1] and printable(d[2]) and not d[3]

        def word(d):
            """Up to three characters and a NUL."""
            return not d[3] and all(printable(b) or not b for b in d[:3]) and printable(d[0])

        def shorts(d):
            return int.from_bytes(d[:2], 'little') < 0x400 and int.from_bytes(d[2:], 'little') < 0x400

        pointers = {}
        self.string_pointers = {}
        for s in self.image.sections:
            if s.code:
                continue
            data = s.data
            for offset in range(0, len(data) - 3, 4):
                value = int.from_bytes(data[offset:offset + 4], 'little')
                if self.image.is_code(value):
                    before = data[offset - 4:offset] if offset >= 4 else b'\0\0\0\0'
                    this = data[offset:offset + 4]
                    after = data[offset + 4:offset + 8].ljust(4, b'\0')
                    text = all(printable(b) for b in before) or (utf16(before) and utf16(this))
                    if value % 16 and not text:
                        # Short strings next to short strings that are no code address, arrays of 16-bit values.
                        near = [int.from_bytes(x, 'little') for x in (before, after)]
                        text = (word(this) and any(word(x) for x in (before, after))
                                and not any(self.image.is_code(x) for x in near)) or \
                            (shorts(before) and shorts(this) and shorts(after))
                    target = self.string_pointers if text else pointers
                    target.setdefault(value, set()).add(s.address + offset)
        return pointers

    def resolve_conflicts(self):
        """Pointer entries whose code overlaps code reached later from a call (at other instruction boundaries):
        the pointer was a number that happened to decode."""
        bad = set()
        for entry, sites in self.conflicts:
            owners = set(self.owner[a] for a in sites)
            if self.origin.get(entry) in TRUSTED and all(self.origin[o] not in TRUSTED for o in owners):
                bad |= owners
            else:
                self.trusted_overlaps.append((entry, sorted(owners)))
        return bad

    def discover(self, extra_entries=()):
        """Finds every entry. Rounds repeat the search from scratch while the set of no-return functions grows,
        so code after a call that never returns does not contribute entries, and while pointer entries turn out
        to overlap code reached from calls."""
        self.blacklist = {}
        for rounds in range(1, 30):
            self.reset()
            self.closure([self.image.entry_point], 'entry point')
            self.closure(list(extra_entries), 'configured')
            self.data_pointers = self.scan_data_pointers()
            while True:
                # Tail-call targets, then code immediates (they come from accepted code), then data dwords and the
                # fields of misaligned objects. A candidate rejected earlier is checked again: new entries may end the
                # path that hit other code, or complete the table around an interior instruction.
                self.rejected = {}
                new = self.closure(self.tail_targets(), 'jump')
                new += self.closure(sorted(v for v in self.code_refs if v not in self.entries), 'code pointer')
                for value, sites in self.object_fields().items():
                    self.data_pointers.setdefault(value, set()).update(sites)
                new += self.closure(sorted(v for v in self.data_pointers if v not in self.entries), 'data pointer')
                if not new:
                    break
            no_return = self.find_no_return()
            self.drop_data_entries()
            bad = self.resolve_conflicts()
            for e in bad:
                self.blacklist[e] = (self.origin[e], 'overlaps call-reached code')
            for e in self.displaced - bad:
                self.blacklist[e] = (self.origin[e], 'in the way of a code pointer')
            if no_return <= self.no_return and not bad and not self.displaced:
                break
            self.no_return |= no_return
        for e, reason in self.blacklist.items():
            self.rejected[e] = reason
        self.rounds = rounds
        # Entries code or data may hold the address of (text dwords too: being wrong there only costs speed).
        self.address_taken = set(e for e in self.entries if e in self.code_refs or e in self.data_pointers
                                 or e in self.string_pointers)
        self.address_taken.add(self.image.entry_point)

    def find_no_return(self):
        """Entries none of whose paths returns: least fixpoint over ret, tail calls, indirect jumps and falls into
        other entries; calls to no-return functions end their path."""
        tails = frozenset(self.entries)
        exits = {}
        for e in self.entries:
            if e in self.declared_no_return:
                exits[e] = set()
                continue
            function, _, _ = self.walk(e, tails)
            returns = False
            follow = set()
            for a, ins in function.items():
                if ins is None:
                    continue
                m = mnemonic_of(ins)
                if m in ('ret', 'retf', 'iretd'):
                    returns = True
                    break
                if is_jump(m):
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        target = op.imm & 0xffffffff
                        if target in tails and target != e:
                            follow.add(target)
                    elif not self.is_table_jump(ins):
                        if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index \
                                and (op.mem.disp & 0xffffffff) in self.no_return_slots:
                            continue
                        returns = True      # indirect jump: a tail call to anything
                        break
                if m == 'call' and self.calls_no_return(ins):
                    continue
                if m not in STOPS and ins.address + ins.size in tails and ins.address + ins.size != e:
                    follow.add(ins.address + ins.size)
            exits[e] = True if returns else follow
        returning = set(e for e, x in exits.items() if x is True)
        changed = True
        while changed:
            changed = False
            for e, x in exits.items():
                if e not in returning and x is not True and x & returning:
                    returning.add(e)
                    changed = True
        return (set(self.entries) - returning) | self.declared_no_return

    def drop_data_entries(self):
        """Entries found before the table that covers them was known."""
        for e in sorted(self.entries):
            if self.in_data(e) and self.origin[e] not in TRUSTED:
                self.log('warning: entry 0x%x (%s) lies in a jump table, dropped' % (e, self.origin[e]))
                self.entries.discard(e)

    def gaps(self):
        """Code-section ranges that no accepted instruction or table covers, without leading padding."""
        result = []
        s = self.code_section
        if s is None:
            return result
        start = None
        for i, c in enumerate(self.map):
            if c == FREE:
                if start is None:
                    start = i
            elif start is not None:
                result.append((s.address + start, s.address + i))
                start = None
        if start is not None:
            result.append((s.address + start, s.address + len(self.map)))
        trimmed = []
        for start, stop in result:
            data = self.image.read(start, stop - start)
            first = next((i for i, b in enumerate(data) if b not in PADDING + (0,)), None)
            if first is not None:
                trimmed.append((start + first, stop))
        return trimmed

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
            if m == 'call' and self.calls_no_return(ins):
                # Never returns: trap if it does.
                instructions.append(FakeTrapInstruction(ins.address, 'no-return call returned'))
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
