"""Translates the x86 routines inside the game's EA audio banks (Sound/**/*.abk, magic "ABKC") into C++.

Each bank entry carries a routine that the audio library runs from its mixer tick; at run time the bank loader
0x64DF30 patches the routine's call targets (table at header +0x30: rel32 fields holding an index into the library
function table 0x6E9AE8) and adds the bank address to other dwords (table at +0x34). The routines are decoded with
the rules of the bank code interpreter (src/nfsu/bankcode.cpp) and written as functions that run from one call to
the next (src/nfsu/banknative.h). A routine with relocated bytes the translation cannot express is left to the
interpreter.

Output: nfsu_bank.cpp with the translations and their table.
"""

import hashlib
import os
import struct

LIBRARY_TABLE = 0x6E9AE8
REGISTERS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
OUTPUT = 'nfsu_bank.cpp'


def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def sext8(v):
    return v - 0x100 if v & 0x80 else v


class Untranslatable(Exception):
    pass


class Instruction:
    def __init__(self, offset):
        self.offset = offset
        self.length = 0
        self.op = None          # interpreter operation, or 'trap'
        self.what = None        # trap message
        self.reg = 0
        self.memory = False
        self.rm = 0
        self.base = None
        self.index = None
        self.scale = 0
        self.displacement = 0
        self.bank_relative = False  # displacement is bank + displacement
        self.immediate = 0
        self.target = None      # jumps: routine offset; calls: ('library', address) or ('routine', offset)


class Decoder:
    """The interpreter's decode() over the file bytes of one routine. calls and offsets map the relocated dwords
    (routine offsets) to their library target and their bank offset."""
    def __init__(self, code, calls, offsets):
        self.code = code
        self.calls = calls
        self.offsets = offsets

    def byte(self):
        if self.ip >= len(self.code):
            raise Untranslatable('instruction past the bank data at +0x%x' % self.ip)
        value = self.code[self.ip]
        self.ip += 1
        return value

    def dword(self):
        if self.ip + 4 > len(self.code):
            raise Untranslatable('instruction past the bank data at +0x%x' % self.ip)
        value = u32(self.code, self.ip)
        self.ip += 4
        return value

    def displacement32(self, ins):
        at = self.ip
        value = self.dword()
        if at in self.offsets:
            ins.bank_relative = True
        return value, at

    def modrm(self, ins, relocated):
        byte = self.byte()
        mod = byte >> 6
        rm = byte & 7
        ins.reg = (byte >> 3) & 7
        if mod == 3:
            ins.memory = False
            ins.rm = rm
            return
        ins.memory = True
        if rm == 4:
            sib = self.byte()
            base = sib & 7
            index = (sib >> 3) & 7
            if base == 5 and mod == 0:
                ins.displacement, at = self.displacement32(ins)
                relocated.append(at)
            else:
                ins.base = base
            if index != 4:
                ins.index = index
                ins.scale = sib >> 6
        elif rm == 5 and mod == 0:
            ins.displacement, at = self.displacement32(ins)
            relocated.append(at)
        else:
            ins.base = rm
        if mod == 1:
            ins.displacement = (ins.displacement + sext8(self.byte())) & 0xffffffff
        elif mod == 2:
            value, at = self.displacement32(ins)
            relocated.append(at)
            ins.displacement = (ins.displacement + value) & 0xffffffff

    def decode(self, offset):
        ins = Instruction(offset)
        self.ip = offset
        relocated = []          # dword fields of this instruction that may carry a relocation
        try:
            self.decode_into(ins, relocated)
        except TrapAt as trap:
            ins.op = 'trap'
            ins.what = trap.what
        ins.length = self.ip - offset
        # Relocated bytes are allowed only as the dword fields the translation reads from the relocation.
        for at in list(self.calls) + list(self.offsets):
            if at + 4 > offset and at < offset + ins.length and at not in relocated:
                raise Untranslatable('relocated dword at +0x%x inside the instruction at +0x%x' % (at, offset))
        for at in relocated:
            if at in self.calls and not (ins.op == 'call' and at == offset + 1):
                raise Untranslatable('call relocation at +0x%x outside a call' % at)
            if at in self.offsets and ins.op == 'call':
                raise Untranslatable('bank offset relocation in the call at +0x%x' % offset)
        return ins

    def decode_into(self, ins, relocated):
        op = self.byte()
        if 0x50 <= op <= 0x5f:
            ins.op = 'push' if op <= 0x57 else 'pop'
            ins.reg = op & 7
            return
        if op in (0x89, 0x8b, 0x03, 0x3b):
            ins.op = {0x89: 'movstore', 0x8b: 'movload', 0x03: 'add', 0x3b: 'cmp'}[op]
            self.modrm(ins, relocated)
        elif op == 0x83:
            self.modrm(ins, relocated)
            ins.immediate = sext8(self.byte()) & 0xffffffff
            if ins.reg != 0 and ins.reg != 7:
                raise TrapAt('group 1 operation')
            ins.op = 'addimm' if ins.reg == 0 else 'cmpimm'
        elif op == 0xc6:
            self.modrm(ins, relocated)
            if ins.reg != 0:
                raise TrapAt('C6 form')
            ins.immediate = self.byte()
            if not ins.memory:
                raise TrapAt('byte register')
            ins.op = 'storebyte'
        elif op == 0x0f:
            if self.byte() != 0xaf:
                raise TrapAt('two-byte opcode')
            self.modrm(ins, relocated)
            ins.op = 'imul'
        elif op in (0x7c, 0x7f):
            rel = sext8(self.byte())
            ins.target = self.ip + rel
            ins.op = 'jl' if op == 0x7c else 'jg'
        elif op == 0xe8:
            at = self.ip
            rel = self.dword()
            relocated.append(at)
            if at in self.calls:
                ins.target = ('library', self.calls[at])
            else:
                ins.target = ('routine', (self.ip + rel) & 0xffffffff)
            ins.op = 'call'
        elif op == 0xc3:
            ins.op = 'ret'
        else:
            raise TrapAt('instruction')


class TrapAt(Exception):
    def __init__(self, what):
        Exception.__init__(self, what)
        self.what = what


class Routine:
    def __init__(self, key, code, length, calls, offsets, sources):
        self.key = key
        self.code = code            # file bytes from the routine's start to the end of the bank file
        self.length = length        # entry +0x2C - entry +0x28
        self.calls = calls          # routine offset of a rel32 -> library function
        self.offsets = offsets      # routine offset of a dword -> its file value (bank offset)
        self.sources = sources      # (bank file, entry index)
        self.instructions = {}
        self.resumes = [0]          # offset of the instruction at each resume point

    @property
    def name(self):
        return 'bank_%s' % self.key

    def analyse(self):
        decoder = Decoder(self.code, self.calls, self.offsets)
        work = [0]
        while work:
            offset = work.pop()
            if offset in self.instructions:
                continue
            ins = decoder.decode(offset)
            self.instructions[offset] = ins
            if ins.op in ('trap', 'ret'):
                continue
            following = offset + ins.length
            if ins.op in ('jl', 'jg') and 0 <= ins.target < self.length:
                work.append(ins.target)
            if following < self.length:
                work.append(following)

    def spans(self):
        result = []
        for offset in sorted(self.instructions):
            end = offset + self.instructions[offset].length
            if result and offset <= result[-1][1]:
                result[-1][1] = max(result[-1][1], end)
            else:
                result.append([offset, end])
        return result

    def relocations(self):
        inside = []
        for begin, end in self.spans():
            for at in sorted(set(self.calls) | set(self.offsets)):
                if begin <= at < end:
                    if at in self.calls:
                        inside.append((at, 'CallTarget', self.calls[at]))
                    else:
                        inside.append((at, 'BankOffset', self.offsets[at]))
        return inside


def address_expression(ins):
    parts = []
    if ins.base is not None:
        parts.append(REGISTERS[ins.base])
    if ins.index is not None:
        parts.append('(%s << %d)' % (REGISTERS[ins.index], ins.scale) if ins.scale else REGISTERS[ins.index])
    if ins.bank_relative:
        parts.append('bank')
    if ins.displacement or not parts:
        parts.append('0x%xu' % ins.displacement)
    return ' + '.join(parts)


def operand_read(ins):
    return 'rc::ld32(%s)' % address_expression(ins) if ins.memory else REGISTERS[ins.rm]


STATE = 'storeState(cpu, flags, eax, ecx, edx, ebx, esp, ebp, esi, edi)'


def trap_lines(address, what):
    return ['{', '    %s;' % STATE, '    unsupported(begin + 0x%xu, "%s");' % (address & 0xffffffff, what), '}']


def jump_lines(routine, target):
    if 0 <= target < routine.length:
        return ['goto L_%04x;' % target]
    return trap_lines(target, 'jump out of the routine')


def instruction_lines(routine, ins):
    r = REGISTERS[ins.reg]
    if ins.op == 'trap':
        return trap_lines(ins.offset, ins.what)
    if ins.op == 'push':
        return ['{', '    const reg32 v = %s;' % r, '    esp -= 4;', '    memory.st32(esp, v);', '}']
    if ins.op == 'pop':
        return ['{', '    const reg32 v = rc::ld32(esp);', '    esp += 4;', '    %s = v;' % r, '}']
    if ins.op == 'movstore':
        if ins.memory:
            return ['memory.st32(%s, %s);' % (address_expression(ins), r)]
        return ['%s = %s;' % (REGISTERS[ins.rm], r)]
    if ins.op == 'movload':
        return ['%s = %s;' % (r, operand_read(ins))]
    if ins.op == 'add':
        return ['%s = flags.add(%s, %s);' % (r, r, operand_read(ins))]
    if ins.op == 'cmp':
        return ['flags.sub(%s, %s);' % (r, operand_read(ins))]
    if ins.op == 'addimm':
        if ins.memory:
            return ['{', '    const reg32 at = %s;' % address_expression(ins),
                    '    memory.st32(at, flags.add(rc::ld32(at), 0x%xu));' % ins.immediate, '}']
        rm = REGISTERS[ins.rm]
        return ['%s = flags.add(%s, 0x%xu);' % (rm, rm, ins.immediate)]
    if ins.op == 'cmpimm':
        return ['flags.sub(%s, 0x%xu);' % (operand_read(ins), ins.immediate)]
    if ins.op == 'storebyte':
        return ['memory.st8(%s, 0x%xu);' % (address_expression(ins), ins.immediate)]
    if ins.op == 'imul':
        return ['{', '    const int64_t product = int64_t(x86::sreg32(%s)) * x86::sreg32(%s);' % (r, operand_read(ins)),
                '    const reg32 low = reg32(product);', '    flags.materialize(cpu);',
                '    cpu.flags.cf = cpu.flags.of = (product != int64_t(x86::sreg32(low)));', '    %s = low;' % r, '}']
    if ins.op in ('jl', 'jg'):
        condition = ('cpu.flags.sf != cpu.flags.of' if ins.op == 'jl'
                     else 'cpu.flags.sf == cpu.flags.of && !cpu.flags.zf')
        return ['flags.materialize(cpu);', 'if (%s)' % condition, '{'] + \
            ['    ' + line for line in jump_lines(routine, ins.target)] + ['}']
    if ins.op == 'call':
        kind, value = ins.target
        target = '0x%xu' % value if kind == 'library' else 'begin + 0x%xu' % value
        routine.resumes.append(ins.offset + ins.length)
        return ['esp -= 4;', '%s;' % STATE, 'cpu.ip = %s;' % target, 'return %d;' % (len(routine.resumes) - 1),
                'R_%d:' % (len(routine.resumes) - 1)]
    if ins.op == 'ret':
        return ['esp += 4;', '%s;' % STATE, 'return Returned;']
    raise AssertionError(ins.op)


def disassembly(cs, routine, ins):
    data = bytes(routine.code[ins.offset:ins.offset + ins.length])
    text = ' '.join(next(((i.mnemonic, i.op_str) for i in cs.disasm(data, ins.offset)), ('(bad)', '')))
    if ins.op == 'call' and ins.target[0] == 'library':
        text = 'call 0x%x' % ins.target[1]
    elif ins.op == 'call':
        text = 'call routine +0x%x' % ins.target[1]
    return '// %04x  %-24s %s' % (ins.offset, data.hex(), text.strip())


def function_lines(cs, routine):
    lines = ['// %s' % ', '.join('%s entry %d' % s for s in routine.sources[:2])
             + (' and %d more' % (len(routine.sources) - 2) if len(routine.sources) > 2 else ''),
             'template <typename Memory>',
             'uint32_t %s(x86::CPU& cpu, Memory& memory, reg32 begin, reg32 bank, uint32_t resume)' % routine.name,
             '{']
    body = []
    order = sorted(routine.instructions)
    for position, offset in enumerate(order):
        ins = routine.instructions[offset]
        body.append('L_%04x:' % offset)
        body.append(disassembly(cs, routine, ins))
        body.extend(instruction_lines(routine, ins))
        if ins.op in ('trap', 'ret'):
            continue
        following = offset + ins.length
        next_offset = order[position + 1] if position + 1 < len(order) else None
        if following >= routine.length:
            body.extend(trap_lines(following, 'jump out of the routine'))
        elif following != next_offset:
            body.append('goto L_%04x;' % following)
    lines.append('    reg32 eax = cpu.eax, ecx = cpu.ecx, edx = cpu.edx, ebx = cpu.ebx;')
    lines.append('    reg32 esp = cpu.esp, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;')
    lines.append('    LazyFlags flags;')
    lines.append('    switch (resume)')
    lines.append('    {')
    lines.append('    case 0:')
    lines.append('        break;')
    for k in range(1, len(routine.resumes)):
        lines.append('    case %d:' % k)
        lines.append('        goto R_%d;' % k)
    lines.append('    default:')
    lines.append('        __builtin_unreachable();')
    lines.append('    }')
    for line in body:
        if line.startswith(('L_', 'R_')):
            lines.append(line)
        else:
            lines.append('    ' + line)
    lines.append('}')
    return lines


def find_banks(game_dir):
    banks = []
    sound = None
    for name in os.listdir(game_dir):
        if name.lower() == 'sound' and os.path.isdir(os.path.join(game_dir, name)):
            sound = os.path.join(game_dir, name)
    if sound is None:
        return banks
    for root, dirs, files in os.walk(sound):
        dirs.sort()
        for name in sorted(files):
            if name.lower().endswith('.abk'):
                banks.append(os.path.join(root, name))
    return banks


def read_routines(game_dir, image):
    """Distinct routines of every bank, by their file bytes and relocations; and notes on skipped ones."""
    routines = {}
    for path in find_banks(game_dir):
        with open(path, 'rb') as f:
            b = f.read()
        if len(b) < 0x40 or b[:4] != b'ABKC':
            continue
        relative = os.path.relpath(path, game_dir).replace('\\', '/')
        calls_table, offsets_table = u32(b, 0x30), u32(b, 0x34)
        calls = {}
        for i in range(u32(b, calls_table)):
            at = u32(b, calls_table + 4 + 4 * i)
            calls[at] = image.dword(LIBRARY_TABLE + 4 * u32(b, at))
        offsets = {}
        for i in range(u32(b, offsets_table)):
            at = u32(b, offsets_table + 4 + 4 * i)
            offsets[at] = u32(b, at)
        entry = u32(b, 0x1c)
        for index in range(u16(b, 0x0a)):
            begin, end = u32(b, entry + 0x28), u32(b, entry + 0x2c)
            code = b[begin:]
            length = end - begin
            rcalls = {at - begin: target for at, target in calls.items() if at >= begin}
            roffsets = {at - begin: value for at, value in offsets.items() if at >= begin}
            routine = Routine(None, code, length, rcalls, roffsets, [(relative, index)])
            try:
                routine.analyse()
            except Untranslatable as e:
                print('bank code: %s entry %d not translated: %s' % (relative, index, e))
                entry += 0x3c + 4 * (b[entry + 0x24] + b[entry + 0x27])
                continue
            identity = hashlib.sha256()
            identity.update(struct.pack('<I', length))
            for s, e in routine.spans():
                identity.update(struct.pack('<II', s, e))
                identity.update(code[s:e])
            for at, kind, value in routine.relocations():
                identity.update(struct.pack('<I', at) + kind.encode() + struct.pack('<I', value))
            key = identity.hexdigest()[:16]
            if key in routines:
                routines[key].sources.append((relative, index))
            else:
                routine.key = key
                routines[key] = routine
            entry += 0x3c + 4 * (b[entry + 0x24] + b[entry + 0x27])
    return [routines[k] for k in sorted(routines, key=lambda k: (routines[k].length, k))]


def write(game_dir, image, cs, out_dir):
    routines = read_routines(game_dir, image)
    out = ['// Generated by tools/recompiler: the routines of the audio banks in Sound/ (src/nfsu/banknative.h).',
           '#include <banknative.h>', '', 'namespace nfsu { namespace bankcode {', '', 'namespace', '{', '']
    for routine in routines:
        out.extend(function_lines(cs, routine))
        out.append('')
        spans = routine.spans()
        byte_count = spans[-1][1]
        data = bytes(routine.code[:byte_count])
        out.append('const uint8_t %s_bytes[] = {' % routine.name)
        for i in range(0, len(data), 16):
            out.append('    ' + ' '.join('0x%02x,' % v for v in data[i:i + 16]))
        out.append('};')
        out.append('const Span %s_spans[] = { %s };'
                   % (routine.name, ', '.join('{ 0x%x, 0x%x }' % (s, e - s) for s, e in spans)))
        relocations = routine.relocations()
        if relocations:
            out.append('const Relocation %s_relocations[] = {' % routine.name)
            for at, kind, value in relocations:
                out.append('    { 0x%x, %s, 0x%xu },' % (at, kind, value))
            out.append('};')
        out.append('const uint32_t %s_resumes[] = { %s };'
                   % (routine.name, ', '.join('0x%x' % r for r in routine.resumes)))
        out.append('')
    out.append('const Translation s_translations[] = {')
    if not routines:
        out.append('    { nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, nullptr },')
    for routine in routines:
        source, index = routine.sources[0]
        name = '%s:%d' % (os.path.basename(source), index)
        relocations = routine.relocations()
        out.append('    { "%s", 0x%x, %s_bytes, sizeof(%s_bytes), %s_spans, %d, %s, %d, %s_resumes, %d, &%s<Direct>,'
                   ' &%s<Recording> },'
                   % (name, routine.length, routine.name, routine.name, routine.name, len(routine.spans()),
                      '%s_relocations' % routine.name if relocations else 'nullptr', len(relocations),
                      routine.name, len(routine.resumes), routine.name, routine.name))
    out.append('};')
    out.append('')
    out.append('}')
    out.append('')
    out.append('const Translation* const g_translations = s_translations;')
    out.append('const uint32_t g_translationCount = %d;' % len(routines))
    out.append('')
    out.append('}}')
    with open(os.path.join(out_dir, OUTPUT), 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    entries = sum(len(r.sources) for r in routines)
    print('bank code: %d routines translated (%d bank entries, %d instructions)'
          % (len(routines), entries, sum(len(r.instructions) for r in routines)))
