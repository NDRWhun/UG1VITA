"""Control flow and dataflow analyses over the recompiled functions.

Flags live in C++ locals (f_cf...) inside a function; cpu.flags is the copy other code sees. A flag is stored to
cpu.flags at a boundary (call, return, tail call, pushfd) only when code outside can read it there (interprocedural
liveness) and this function changed it since cpu.flags was last valid (a local producer reaches the boundary).

Every flag read is served either from the local or, when exactly one lazy producer reaches it, from that producer's
saved operands (cmp a, b; jl -> a < b). Producers write their local only for reads that cannot be served lazily.

State masks: bits 0-6 are FLAGS (ctx.FLAG_BIT), bits 8-15 general registers (ctx.REG_BIT << 8).
"""

from capstone import x86 as X

from .codegen import ctx as C
from . import codegen, fpustack, stack


FCOM = ('fcom', 'fcomp', 'fcompp', 'ftst')
JCC = set('j' + cc for cc in C.CC_ALIASES) | {'jcxz', 'jecxz', 'loop', 'loope', 'loopne'}
REG_SHIFT = 8
ALL_REG_STATE = C.ALL_REGS << REG_SHIFT
ALL_STATE = C.ALL_FLAGS | ALL_REG_STATE
MEMORY_DEFS = ('popfd', 'popf')     # write cpu.flags directly
# pf and af are produced only by sahf, popf and x87 compares; these capstone flags mark instructions that set them on
# x86 (integer arithmetic) without the generator modelling it.
X86_SETS = {'pf': X.X86_EFLAGS_MODIFY_PF | X.X86_EFLAGS_RESET_PF | X.X86_EFLAGS_SET_PF,
            'af': X.X86_EFLAGS_MODIFY_AF | X.X86_EFLAGS_RESET_AF | X.X86_EFLAGS_SET_AF}
# Every way an instruction may write a flag on x86 (undefined results included).
X86_WRITES = dict((f, sum(getattr(X, 'X86_EFLAGS_%s_%s' % (k, f.upper()))
                          for k in ('MODIFY', 'RESET', 'SET', 'UNDEFINED', 'PRIOR')))
                  for f in C.CPU_FLAGS)


class Exit:
    """An edge leaving the function (or a call out of it) at one instruction."""
    __slots__ = ('edge', 'kind', 'target')

    def __init__(self, edge, kind, target=None):
        self.edge = edge        # 'exit' (ret, tail, fall-off), 'taken' (jcc to a tail target), 'call'
        self.kind = kind        # 'ret', 'tail', 'dyntail', 'call', 'dyncall'
        self.target = target    # entry address for 'tail' and 'call'


class Function:
    ENTRY = -1

    def __init__(self, module, subroutine, thread_segments, skip_instructions, function_names):
        self.module = module
        self.sub = subroutine
        self.instructions = subroutine.instructions
        self.entry_address = subroutine.get_entry_point()
        self.thread_segments = thread_segments
        self.skip = set(skip_instructions)
        self.function_names = function_names
        self.index_of = {}
        for i, ins in enumerate(self.instructions):
            self.index_of.setdefault(ins.address, i)
        self.static_targets = set(x for _, x in subroutine.static_labels)
        self.dynamic_targets = set(x for _, x in subroutine.dynamic_labels)
        self.entry = self.index_of[self.entry_address]
        self.n = len(self.instructions)
        self.program = None

        self.branch = [None] * self.n
        self.succ = [[] for _ in range(self.n)]
        self.pred = None
        self.exits = [[] for _ in range(self.n)]
        self.effects = [None] * self.n
        self.use = [0] * self.n     # state read (flags | regs << 8)
        self.kill = [0] * self.n    # state always written
        self.unmodeled = [0] * self.n   # pf/af set on x86 but not by the generated code
        self.fused = {}             # index of fcom / fnstsw / sahf -> index of the sahf of its triple
        self.live_in = None
        self.live_out = None
        self._rd_cache = {}
        self.local_needed = set()   # (producer index, flag): the producer writes f_<flag>
        self.lazy_readers = set()   # producers with lazily served reads
        self.reloads = set()        # (memory producer, flag): reload f_<flag> from cpu.flags after it
        self.stores = {}            # (index, edge) -> flag mask stored to cpu.flags there
        self.reg_stores = {}        # (index, edge) -> registers stored to cpu there
        self.reg_reloads = {}       # call index -> registers reloaded from cpu after it
        self.fc_needed = set()      # x87 condition code producers a later reader (or boundary) sees

    @property
    def local_registers(self):
        """Registers live in C++ locals (else every access goes to cpu)."""
        p = self.program
        return p.local_registers and self.entry_address not in p.memory_functions

    # ---- structure ----------------------------------------------------------------------------------------
    def is_label(self, i):
        a = self.instructions[i].address
        return a in self.static_targets or a in self.dynamic_targets or a == self.entry_address

    def in_function(self, address):
        return address in self.index_of

    def classify(self):
        """Control flow of every instruction: successors and exit edges."""
        switch_cases = sorted(self.index_of[a] for a in self.dynamic_targets if a in self.index_of)
        for i, ins in enumerate(self.instructions):
            nxt = [i + 1] if i + 1 < self.n else []
            fall_off = [] if nxt else [Exit('exit', 'ret')]
            mnemonic = ins.mnemonic.split()[-1]
            if ins.address in self.skip:
                self.branch[i] = ('skip',)
                self.succ[i] = nxt
                self.exits[i] = fall_off
            elif mnemonic == 'jmp' or mnemonic in JCC:
                target = self.jump_target(ins)
                conditional = mnemonic != 'jmp'
                self.branch[i] = target
                succ = list(nxt) if conditional else []
                exits = list(fall_off) if conditional else []
                edge = 'taken' if conditional else 'exit'
                if target[0] == 'label':
                    succ.append(self.index_of[target[1]])
                elif target[0] == 'tail':
                    exits.append(Exit(edge, 'tail', target[1]))
                elif target[0] == 'dyntail':
                    exits.append(Exit(edge, 'dyntail'))
                elif target[0] == 'switch':
                    succ.extend(switch_cases)
                self.succ[i] = succ
                self.exits[i] = exits
            elif mnemonic == 'ret':
                self.branch[i] = ('ret',)
                self.exits[i] = [Exit('exit', 'ret')]
            elif mnemonic == 'call':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    self.branch[i] = ('call', op.imm)
                    self.exits[i] = [Exit('call', 'call', op.imm)] + fall_off
                else:
                    self.branch[i] = ('dyncall',)
                    self.exits[i] = [Exit('call', 'dyncall')] + fall_off
                self.succ[i] = nxt
            else:
                self.branch[i] = ('next',)
                self.succ[i] = nxt
                self.exits[i] = fall_off
        self.pred = [[] for _ in range(self.n)]
        for i, s in enumerate(self.succ):
            for j in s:
                self.pred[j].append(i)
        self.find_fused_compares()

    def jump_target(self, ins):
        """('label', a) inside the function, ('tail', a) to another function's entry, ('switch',) through a jump
        table whose cases are all in the function, ('dyntail',) for any other indirect jump."""
        op = ins.operands[0]
        if op.type == X.X86_OP_IMM:
            target = op.imm & 0xffffffff
            return ('label', target) if target in self.static_targets else ('tail', target)
        destinations = getattr(ins, 'potential_destinations', None)
        if op.type == X.X86_OP_REG or not destinations:
            return ('dyntail',)
        if all(self.in_function(d) for d in destinations):
            return ('switch',)
        return ('dyntail',)

    def find_fused_compares(self):
        """fcom*; fnstsw ax; sahf (or test ah, mask of C0, C2, C3) with no label inside: the flags come from the
        compared values."""
        for i in range(self.n - 2):
            a, b, c = self.instructions[i:i + 3]
            if a.mnemonic not in FCOM or b.mnemonic not in ('fnstsw', 'fstsw'):
                continue
            if c.mnemonic != 'sahf' and self.ftest_mask(c) is None:
                continue
            if b.operands[0].type != X.X86_OP_REG or b.reg_name(b.operands[0].reg) != 'ax':
                continue
            if self.is_label(i + 1) or self.is_label(i + 2):
                continue
            if any(x.address in self.thread_segments or x.address in self.skip for x in (a, b, c)):
                continue
            self.fused[i] = self.fused[i + 1] = self.fused[i + 2] = i + 2

    @staticmethod
    def ftest_mask(ins):
        """The mask of test ah, imm when it tests only x87 condition codes C0, C2 and C3, else None."""
        if ins.mnemonic != 'test' or len(ins.operands) != 2:
            return None
        dst, src = ins.operands
        if dst.type != X.X86_OP_REG or ins.reg_name(dst.reg) != 'ah' or src.type != X.X86_OP_IMM:
            return None
        mask = src.imm & 0xff
        return mask if mask and not mask & ~C.FTEST_BITS else None

    def is_call(self, i):
        return self.branch[i][0] in ('call', 'dyncall')

    def is_fused_fnstsw(self, i):
        return self.fused.get(i) is not None and self.instructions[i].mnemonic in ('fnstsw', 'fstsw')

    # ---- effects ------------------------------------------------------------------------------------------
    def record(self):
        ctx = C.Ctx(self, self.function_names, record=True)
        for i, ins in enumerate(self.instructions):
            ctx.begin(i)
            if self.branch[i][0] != 'skip':
                codegen.generate(ctx, ins)
            e = ctx.effects
            self.effects[i] = e
            # A partial register write merges into the register's current value: it reads it.
            self.use[i] = e.flag_use | e.flag_sync | (e.reg_use | e.reg_partial) << REG_SHIFT
            self.kill[i] = e.flag_def | e.reg_def << REG_SHIFT
            eflags = getattr(ins, 'eflags', 0) if self.branch[i][0] != 'skip' else 0
            for f, sets in X86_SETS.items():
                if eflags & sets and not (e.flag_def | e.flag_maydef) & C.FLAG_BIT[f]:
                    self.unmodeled[i] |= C.FLAG_BIT[f]
            if self.program.abi_flags:
                self.kill[i] |= self.unmodeled[i]
            if self.is_fused_fnstsw(i) or self.masks_condition_codes(i):
                self.use[i] &= ~C.FLAG_BIT['fc']    # fused: read only when eax is live (see liveness)
        # A failed assertion traps: nothing runs after an instruction the generator cannot translate.
        for i in range(self.n):
            if self.effects[i].boundary == 'trap':
                self.succ[i] = []
                self.exits[i] = []
        self.pred = [[] for _ in range(self.n)]
        for i, s in enumerate(self.succ):
            for j in s:
                self.pred[j].append(i)
        self.reachable = set()
        stack = [self.entry]
        while stack:
            i = stack.pop()
            if i not in self.reachable:
                self.reachable.add(i)
                stack.extend(self.succ[i])

    def masks_condition_codes(self, i):
        """fnstsw ax directly followed by and eax/ax, imm clearing C0, C2 and C3."""
        ins = self.instructions[i]
        if ins.mnemonic not in ('fnstsw', 'fstsw') or i + 1 >= self.n or self.is_label(i + 1):
            return False
        op = ins.operands[0]
        if op.type != X.X86_OP_REG or ins.reg_name(op.reg) != 'ax':
            return False
        nxt = self.instructions[i + 1]
        if nxt.mnemonic != 'and' or nxt.operands[0].type != X.X86_OP_REG or nxt.operands[1].type != X.X86_OP_IMM:
            return False
        return nxt.reg_name(nxt.operands[0].reg) in ('eax', 'ax') and not (nxt.operands[1].imm & 0x4500)

    def may_def_local(self):
        """State (flags, registers) this function may change, before its callees."""
        mask = 0
        for i in self.reachable:
            e = self.effects[i]
            mask |= e.flag_def | e.flag_maydef | (e.reg_def | e.reg_partial) << REG_SHIFT
            if self.instructions[i].mnemonic in MEMORY_DEFS:
                mask |= C.ALL_FLAGS
            for x in self.exits[i]:
                if x.kind in ('dyncall', 'dyntail'):
                    mask |= ALL_STATE
        return mask

    def flag_writes_local(self):
        """Flags this function may write on x86 before its callees, the ones the generator does not model too."""
        mask = 0
        for i in self.reachable:
            e = self.effects[i]
            mask |= e.flag_def | e.flag_maydef
            if self.branch[i][0] != 'skip':
                eflags = getattr(self.instructions[i], 'eflags', 0)
                for f, bits in X86_WRITES.items():
                    if eflags & bits:
                        mask |= C.FLAG_BIT[f]
            if self.instructions[i].mnemonic in MEMORY_DEFS:
                mask |= C.ALL_FLAGS
            for x in self.exits[i]:
                if x.kind in ('dyncall', 'dyntail'):
                    mask |= C.ALL_FLAGS
        return mask

    def flag_must_def(self):
        """Flags written on every path from the entry to every exit, the callees' summaries included."""
        must = self.program.flag_must
        state = [None] * self.n
        state[self.entry] = 0
        work = [self.entry]
        result = C.ALL_FLAGS
        while work:
            i = work.pop()
            s = state[i] | self.effects[i].flag_def
            if self.instructions[i].mnemonic in MEMORY_DEFS:
                s |= C.ALL_FLAGS
            if self.branch[i][0] == 'call':
                s |= must.get(self.branch[i][1], 0)
            for x in self.exits[i]:
                if x.edge != 'call':
                    result &= s | (must.get(x.target, 0) if x.kind == 'tail' else 0)
            for j in self.succ[i]:
                new = s if state[j] is None else state[j] & s
                if new != state[j]:
                    state[j] = new
                    work.append(j)
        return result

    def call_clobber(self, x):
        """Flags whose liveness ends at call x: the ones the callee may write and the x87 condition codes; every
        flag at an indirect call."""
        p = self.program
        if not p.abi_flags:
            return 0
        if x.kind == 'call':
            return p.flag_clobber.get(x.target, C.ALL_FLAGS) | C.FLAG_BIT['fc']
        return C.ALL_FLAGS

    # ---- liveness -----------------------------------------------------------------------------------------
    def exit_use(self, x):
        p = self.program
        if x.kind == 'ret':
            return p.ret_live[self.entry_address]
        if x.kind == 'tail':
            return p.live_in.get(x.target, ALL_STATE) | p.ret_live[self.entry_address]
        if x.kind == 'dyntail':
            return p.indirect_live_in | p.ret_live[self.entry_address]
        if x.kind == 'call':
            return p.live_in.get(x.target, ALL_STATE)
        if x.kind == 'dyncall':
            return p.indirect_live_in
        raise AssertionError(x.kind)

    def liveness(self):
        """Backward liveness of flags and registers; calls pass live state through."""
        n = self.n
        live_in = self.live_in if self.live_in else [0] * n
        out = [0] * n
        eax = C.REG_BIT['eax'] << REG_SHIFT
        fc = C.FLAG_BIT['fc']
        changed = True
        while changed:
            changed = False
            for i in range(n - 1, -1, -1):
                o = 0
                for s in self.succ[i]:
                    o |= live_in[s]
                call_use = 0
                clobber = 0
                for x in self.exits[i]:
                    if x.edge == 'call':
                        call_use |= self.exit_use(x)
                        clobber |= self.call_clobber(x)
                    else:
                        o |= self.exit_use(x)
                use = self.use[i]
                if (o & eax) and self.is_fused_fnstsw(i):
                    use |= fc
                v = use | call_use | (o & ~self.kill[i] & ~clobber)
                out[i] = o
                if v != live_in[i]:
                    live_in[i] = v
                    changed = True
        self.live_in = live_in
        self.live_out = out
        return live_in[self.entry]

    def writes_ax(self, i):
        return bool(self.live_out[i] & (C.REG_BIT['eax'] << REG_SHIFT))

    def flag_live_out(self, i, f):
        return bool(self.live_out[i] & C.FLAG_BIT[f])

    def reg_live_out(self, i, r):
        return bool(self.live_out[i] & (C.REG_BIT[r] << REG_SHIFT))

    def writes_fc(self, i):
        """The x87 condition codes set at i are read later."""
        return self.flag_live_out(i, 'fc') or i in self.fc_needed

    # ---- reaching definitions -----------------------------------------------------------------------------
    def callee_delta(self, i):
        """esp change of a direct call (the call's own -4 included), or None."""
        b = self.branch[i]
        if b[0] != 'call':
            return None
        info = self.program.stack.get(b[1])
        return None if info is None or info.delta is None else info.delta - 4

    def callee_may_def(self, i):
        b = self.branch[i]
        if b[0] == 'call':
            return self.program.may_def.get(b[1], ALL_STATE)
        return ALL_STATE

    def is_memory_producer(self, p, f):
        """p leaves cpu.flags authoritative for f: entry, a call that may change f, popfd."""
        if p == self.ENTRY:
            return True
        return self.is_call(p) or self.instructions[p].mnemonic in MEMORY_DEFS

    def reaching(self, i, f):
        """Producers of flag f reaching the start of instruction i (indices or ENTRY)."""
        key = (i, f)
        cached = self._rd_cache.get(key)
        if cached is not None:
            return cached
        bit = C.FLAG_BIT[f]
        result = set()
        seen = set()
        stack = list(self.pred[i])
        if i == self.entry:
            result.add(self.ENTRY)
        while stack:
            p = stack.pop()
            if p in seen:
                continue
            seen.add(p)
            if self.is_call(p):
                if self.callee_may_def(p) & bit:
                    result.add(p)
                    continue
            elif self.instructions[p].mnemonic in MEMORY_DEFS:
                result.add(p)
                continue
            else:
                e = self.effects[p]
                if e.flag_def & bit:
                    result.add(p)
                    continue
                if e.flag_maydef & bit:
                    result.add(p)
            if p == self.entry:
                result.add(self.ENTRY)
            stack.extend(self.pred[p])
        result = frozenset(result)
        self._rd_cache[key] = result
        return result

    def check_modeled(self, i, f):
        """Fails when a read of pf/af at i can see a value an unmodelled producer set on x86."""
        bit = C.FLAG_BIT[f]
        seen = set()
        stack = list(self.pred[i])
        while stack:
            p = stack.pop()
            if p in seen:
                continue
            seen.add(p)
            if self.unmodeled[p] & bit:
                raise AssertionError('0x%x (%s) reads %s set by 0x%x (%s), which the generator does not model'
                                     % (self.instructions[i].address, self.instructions[i].mnemonic, f,
                                        self.instructions[p].address, self.instructions[p].mnemonic))
            if not (self.is_call(p) or self.instructions[p].mnemonic in MEMORY_DEFS or self.effects[p].flag_def & bit):
                stack.extend(self.pred[p])

    def lazy_producer(self, i, f):
        """The single lazy producer serving flag f at instruction i, or None."""
        rd = self.reaching(i, f)
        if len(rd) != 1:
            return None
        (p,) = rd
        if self.is_memory_producer(p, f):
            return None
        e = self.effects[p]
        if not e.lazy or e.flag_maydef:
            return None
        if f not in C.LAZY_KINDS[e.lazy[0]][2]:
            return None
        return p

    # ---- decisions ----------------------------------------------------------------------------------------
    def boundary_mask(self, i, x):
        """CPU flags code outside may read on exit edge x of instruction i."""
        if x.edge == 'call':
            after = self.live_out[i]
            return (self.exit_use(x) | (after & self.callee_may_def(i))) & C.CPU_FLAG_MASK
        return (self.exit_use(x) | self.indirect_return_use(x)) & C.CPU_FLAG_MASK

    def indirect_return_use(self, x):
        """Flags read at a return of a function an indirect jump may reach."""
        p = self.program
        if x.kind == 'ret' and self.entry_address in p.indirect_returners:
            return p.indirect_tail_flags
        return 0

    def need_valid(self, i, f):
        """Every producer reaching a read of f at i must provide a valid f_<flag>."""
        p = self.lazy_producer(i, f)
        if p is not None:
            self.lazy_readers.add(p)
            return
        for q in self.reaching(i, f):
            if self.is_memory_producer(q, f):
                self.reloads.add((q, f))
            else:
                self.local_needed.add((q, f))

    def dirty(self, i, f):
        return any(not self.is_memory_producer(q, f) for q in self.reaching(i, f))

    def decide(self):
        fc = C.FLAG_BIT['fc']
        for i in range(self.n):
            read = self.use[i] | (fc if self.is_fused_fnstsw(i) and self.writes_ax(i) else 0)
            for x in self.exits[i]:
                read |= self.exit_use(x) | self.indirect_return_use(x)
            if read & fc:
                self.fc_needed.update(q for q in self.reaching(i, 'fc') if not self.is_memory_producer(q, 'fc'))
        for i in range(self.n):
            mask = self.effects[i].flag_use & C.CPU_FLAG_MASK
            for f in C.CPU_FLAGS:
                if mask & C.FLAG_BIT[f]:
                    if f in X86_SETS and i in self.reachable:
                        self.check_modeled(i, f)
                    self.need_valid(i, f)
            stores = {}
            for x in self.exits[i]:
                mask = self.boundary_mask(i, x)
                for f in C.CPU_FLAGS:
                    if mask & C.FLAG_BIT[f] and self.dirty(i, f):
                        stores[x.edge] = stores.get(x.edge, 0) | C.FLAG_BIT[f]
                        self.need_valid(i, f)
            if self.effects[i].boundary == 'flags':
                for f in C.CPU_FLAGS:
                    if self.dirty(i, f):
                        stores['flags'] = stores.get('flags', 0) | C.FLAG_BIT[f]
                        self.need_valid(i, f)
            for edge, mask in stores.items():
                self.stores[(i, edge)] = mask
        if self.local_registers:
            self.decide_registers()

    def boundary_registers(self, i, x):
        """Registers code outside may read on exit edge x of instruction i."""
        if x.edge == 'call':
            mask = self.exit_use(x) | (self.live_out[i] & self.callee_may_def(i))
        else:
            mask = self.exit_use(x)
        return (mask >> REG_SHIFT) & C.ALL_REGS

    def decide_registers(self):
        """Registers live in locals: store the ones changed since cpu was last valid where code outside reads
        them, reload after a call the ones it may change that are read later.

        Callers keep the registers this function preserves in their locals and may still count them as valid
        in cpu. So when cpu's copy of a preserved register was overwritten here (stored at a boundary, or
        changed by a callee), it is stored back before leaving: the local holds the entry value there."""
        info = self.program.stack.get(self.entry_address)
        preserved = info.preserved if info else 0
        state_in = [None] * self.n
        state_in[self.entry] = (0, 0)       # (dirty: local newer than cpu, clobbered: cpu differs from entry)
        work = [self.entry]
        while work:
            i = work.pop()
            e = self.effects[i]
            dirty, clobbered = state_in[i]
            if e.boundary == 'runtime':
                # cpuid/rdtsc: their inputs are stored to cpu and their outputs written there.
                clobbered |= e.reg_use | e.reg_def
            dirty |= e.reg_def | e.reg_partial
            for x in self.exits[i]:
                stored = dirty & self.boundary_registers(i, x)
                if x.edge != 'call':
                    # A preserved register holds its entry value here, as cpu does unless clobbered.
                    stored = (stored & ~preserved) | (clobbered & preserved)
                if stored:
                    key = (i, x.edge)
                    self.reg_stores[key] = self.reg_stores.get(key, 0) | stored
                if x.edge == 'call':
                    changed = (self.callee_may_def(i) >> REG_SHIFT) & C.ALL_REGS
                    self.reg_reloads[i] = changed & (self.live_out[i] >> REG_SHIFT)
                    clobbered |= stored | changed
                    dirty &= ~stored & ~changed
                    if self.callee_delta(i) is not None:
                        # esp is moved by the callee's known delta here, not reloaded: cpu.esp may be stale.
                        dirty |= C.REG_BIT['esp']
            out = (dirty, clobbered)
            for s in self.succ[i]:
                old = state_in[s]
                new = out if old is None else (old[0] | dirty, old[1] | clobbered)
                if new != old:
                    state_in[s] = new
                    work.append(s)

    # ---- queries used by the code generators --------------------------------------------------------------
    def needs_local(self, i, f):
        return (i, f) in self.local_needed

    def saves_operands(self, i):
        return i in self.lazy_readers

    def fcmp_saved(self, sahf):
        return sahf in self.lazy_readers or any(self.needs_local(sahf, f) for f in ('cf', 'pf', 'zf', 'sf', 'of'))

    def lazy_values(self, p):
        kind, bits = self.effects[p].lazy
        address = self.instructions[p].address
        return kind, bits, {name: C.lazy_local(kind, address, name) for name in C.LAZY_KINDS[kind][0]}

    def flag_source(self, i, f):
        p = self.lazy_producer(i, f)
        if p is None:
            return 'f_' + f
        kind, bits, values = self.lazy_values(p)
        return C.lazy_flags(kind, bits, values)[f]

    def condition(self, i, cc):
        flags = C.CONDITIONS[cc]
        producers = set(self.lazy_producer(i, f) for f in flags)
        if len(producers) == 1:
            (p,) = producers
            if p is not None:
                kind, bits, values = self.lazy_values(p)
                direct = C.lazy_condition(kind, bits, values, cc)
                if direct:
                    return direct
        return C.generic_condition(cc, {f: self.flag_source(i, f) for f in flags})

    def store_lines(self, i, edge, fdepth=None):
        """Stores before leaving on edge; fdepth is the x87 depth there when the stack is in locals."""
        mask = self.stores.get((i, edge), 0)
        regs = self.reg_stores.get((i, edge), 0)
        lines = ['cpu.%s = %s;' % (r, r) for r in C.REGS if regs & C.REG_BIT[r]]
        lines += ['cpu.flags.%s = %s;' % (f, self.flag_source(i, f)) for f in C.CPU_FLAGS if mask & C.FLAG_BIT[f]]
        if self.program.flag_audit:
            lines += self.audit_store_lines(i, edge, mask)
        if fdepth is not None and edge != 'flags':
            for j in range(1, fdepth + 1):
                lines.append('cpu.fpu.regs[(fpu_count + %d) & 7] = fp%d;' % (j, j))
            lines.append('cpu.fpu.count = fpu_count %s %d;' % ('+' if fdepth >= 0 else '-', abs(fdepth)))
        return lines

    def audit_store_lines(self, i, edge, stored):
        """Flag audit: marks the changed flags not stored at this edge unknown in cpu, the stored ones known;
        pushfd then checks all of them."""
        address = self.instructions[i].address
        dirty = sum(C.FLAG_BIT[f] for f in C.CPU_FLAGS if self.dirty(i, f))
        lines = ['cpu.flagsUnknown = (cpu.flagsUnknown & ~0x%xu) | 0x%xu;' % (stored, dirty & ~stored)]
        if edge == 'flags':
            lines.append('RC_FLAG_AUDIT(cpu, 0x%xu, 0x%xu);' % (C.CPU_FLAG_MASK, address))
        return lines

    def audit_reload_lines(self, mask, address):
        return ['RC_FLAG_AUDIT(cpu, 0x%xu, 0x%xu);' % (mask, address)] if mask and self.program.flag_audit else []

    def reload_lines(self, i):
        regs = self.reg_reloads.get(i, 0)
        lines = ['%s = cpu.%s;' % (r, r) for r in C.REGS if regs & C.REG_BIT[r]]
        delta = self.callee_delta(i) if self.local_registers else None
        if delta is not None:
            lines.append('esp = esp + %d;' % (delta + 4))
        if self.program.flag_audit and self.instructions[i].mnemonic in MEMORY_DEFS:
            lines.append('cpu.flagsUnknown &= ~0x%xu;' % C.CPU_FLAG_MASK)
        reloaded = sum(C.FLAG_BIT[f] for f in C.CPU_FLAGS if (i, f) in self.reloads)
        lines += self.audit_reload_lines(reloaded, self.instructions[i].address)
        lines += ['f_%s = cpu.flags.%s;' % (f, f) for f in C.CPU_FLAGS if (i, f) in self.reloads]
        d = self.fpu.depth[i]
        after = self.fpu_after_call(i)
        if self.program.fpu_check and after is not None:
            lines.append('RC_ASSERT(cpu.fpu.count == fpu_count %s %d);' % ('+' if after >= 0 else '-', abs(after)))
        elif self.branch[i][0] == 'dyncall' and self.program.indirect_fpu_checked and d is not None \
                and self.fpu.evidence.get(i) == 'inferred':
            # The callee keeps the x87 depth or pushes the float result the analysis inferred; the runtime logs and
            # corrects any other depth. Import calls need no check; open calls keep the stack in cpu.fpu.
            d += self.indirect_fpu_delta(i)
            lines.append('RC_FPU_CALL(cpu, fpu_count %s %d, 0x%xu);'
                         % ('+' if d >= 0 else '-', abs(d), self.instructions[i].address))
        if after is not None:
            for j in range(1, after + 1):
                lines.append('fp%d = cpu.fpu.regs[(fpu_count + %d) & 7];' % (j, j))
        return lines

    # ---- x87 stack in locals ------------------------------------------------------------------------------
    def fpu_depth(self, i):
        """x87 depth above the entry stack before instruction i, when the function keeps it in locals."""
        if not self.fpu.local:
            return None
        return self.fpu.depth[i]

    def fpu_after_call(self, i):
        d = self.fpu_depth(i)
        if d is None:
            return None
        b = self.branch[i]
        delta = self.program.fpu_deltas.get(b[1]) if b[0] == 'call' else self.indirect_fpu_delta(i)
        if delta is None or delta == fpustack.NEVER:
            return None
        return d + delta

    def indirect_fpu_delta(self, i):
        """Net x87 pushes of the callee of the indirect call at i."""
        return self.fpu.sites.get(i, self.program.indirect_fpu_delta)

    def fpu_exit_depth(self, i):
        """x87 depth when control falls off the end after instruction i."""
        d = self.fpu_depth(i)
        if d is None:
            return None
        if self.is_call(i):
            return self.fpu_after_call(i)
        return d + fpustack.effect(self.instructions[i].mnemonic.split()[-1])

    def entry_loads(self):
        return [f for f in C.CPU_FLAGS if (self.ENTRY, f) in self.reloads]

    def lazy_locals(self):
        """Declarations of the saved operands of lazy producers."""
        names = []
        for p in sorted(self.lazy_readers):
            kind, _, values = self.lazy_values(p)
            if C.is_fcompare_kind(kind):
                continue
            for name, local in values.items():
                names.append((C.lazy_local_type(name), local))
        for i, s in self.fused.items():
            if i == s and self.fcmp_saved(s):
                address = self.instructions[s].address
                for name in ('fa', 'fb'):
                    names.append(('double', C.lazy_local('fcmp', address, name)))
        return sorted(set(names), key=lambda x: x[1])


class Program:
    def __init__(self, modules, thread_segments, skip_instructions, function_names, flags_preserved=(), not_code=(),
                 native_overrides=(), local_registers=True, memory_functions=(), local_x87=True, setjmp=None,
                 longjmp_jumps=None, fpu_check=False, fpu_deltas=None, preempt_loops=False, address_taken=None,
                 abi_flags=False, jump_targets=None, flag_audit=False):
        self.functions = {}
        # Backward jumps are preemption points (WinApplication::allowPreemption).
        self.preempt_loops = preempt_loops
        # Check the x87 depth after every call with a known result depth (diagnostic builds).
        self.fpu_check = fpu_check
        # Declared net x87 pushes of functions whose exits the analysis cannot follow.
        self.declared_fpu_deltas = dict(fpu_deltas or {})
        # setjmp: calls to it keep a host jump buffer, and the code after them reloads every register it reads
        # (a longjmp resumes there with the registers setjmp saved). longjmp_jumps: indirect jumps that resume
        # after a setjmp call (address -> register holding the guest jmp_buf).
        self.setjmp = setjmp
        self.longjmp_jumps = dict(longjmp_jumps or {})
        self.local_registers = local_registers
        self.local_x87 = local_x87
        self.memory_functions = set(memory_functions)
        self.native_overrides = set(native_overrides)
        # Functions that save the caller's flags on entry (pushfd) and restore them before returning (popfd):
        # callers neither provide nor lose flags across a call to them.
        self.flags_preserved = set(flags_preserved)
        for module in modules:
            for sub in module.subroutines:
                fn = Function(module, sub, thread_segments, skip_instructions, function_names)
                fn.program = self
                self.functions[fn.entry_address] = fn
        # Import address table slots: indirect calls through them reach host shims.
        self.import_slots = set(imp.iat_address for module in modules for imp in module.image.imports)
        if address_taken is None:
            address_taken = self.find_address_taken(modules)
        self.address_taken = (set(address_taken) & set(self.functions)) - set(not_code)
        self.live_in = {}
        self.ret_live = {}
        self.may_def = {}
        self.indirect_live_in = 0
        # Flags pass a direct call only where the code reads them (callee before writing them, caller after the
        # call) and no indirect call or indirect jump (except to jump_targets).
        self.abi_flags = abi_flags
        # Generated code checks that every flag it reads from cpu was stored there (RC_FLAG_AUDIT).
        self.flag_audit = flag_audit
        self.indirect_mask = ALL_REG_STATE if abi_flags else ALL_STATE
        self.flag_clobber = {}
        self.flag_must = {}
        # Flags read by the callers of a function leaving through an indirect jump; the returns of the functions
        # that jump reaches store them (jump_targets: site -> entries, else every address-taken function).
        self.jump_targets = dict(jump_targets or {})
        self.indirect_tail_flags = 0
        self.indirect_returners = set()

    def find_address_taken(self, modules):
        """Entries that may be called indirectly, for an image with relocations: module entry points and every entry
        a relocated dword points to (function pointers in data, pushed or stored immediates, table slots). Images
        without relocations pass the set pointer discovery found (Program(address_taken=...))."""
        entries = set(self.functions)
        taken = set()
        for module in modules:
            taken.add(module.entry_point)
            image = module.image
            for r in image.relocations:
                value = image.dword(r)
                if value in entries:
                    taken.add(value)
        return taken & entries

    def kept(self, a, stack_infos=None):
        """Registers direct callers of a keep in their locals across the call: the ones it restores before
        returning, and esp when its change is known."""
        stack_infos = self.stack if stack_infos is None else stack_infos
        info = stack_infos.get(a) if stack_infos else None
        if not info:
            return 0
        mask = info.preserved << REG_SHIFT
        if info.delta is not None:
            mask |= C.REG_BIT['esp'] << REG_SHIFT
        return mask

    def compute_may_def(self, fns, stack_infos=None):
        """State each function may change, its callees included. With stack_infos, registers a function restores
        before returning and an esp change known to its callers do not count."""
        def kept(a):
            return self.kept(a, stack_infos or {})
        local = dict((fn.entry_address, fn.may_def_local() & ~kept(fn.entry_address)) for fn in fns)
        callees = dict((fn.entry_address, set(x.target for i in fn.reachable for x in fn.exits[i]
                                              if x.kind in ('call', 'tail'))) for fn in fns)
        may_def = dict(local)
        changed = True
        while changed:
            changed = False
            for a, targets in callees.items():
                v = may_def[a]
                for t in targets:
                    v |= may_def.get(t, ALL_STATE)
                v &= ~kept(a)
                if a in self.flags_preserved:
                    v &= ~C.ALL_FLAGS
                if v != may_def[a]:
                    may_def[a] = v
                    changed = True
        self.may_def = may_def

    def compute_flag_summaries(self, fns):
        """Per function: the flags it may write on x86 (callees included) and the ones it writes on every path."""
        clobber = dict((fn.entry_address, fn.flag_writes_local()) for fn in fns)
        callees = dict((fn.entry_address, set(x.target for i in fn.reachable for x in fn.exits[i]
                                              if x.kind in ('call', 'tail'))) for fn in fns)
        changed = True
        while changed:
            changed = False
            for a, targets in callees.items():
                v = clobber[a]
                for t in targets:
                    v |= clobber.get(t, C.ALL_FLAGS)
                if a in self.flags_preserved:
                    v = 0
                if v != clobber[a]:
                    clobber[a] = v
                    changed = True
        self.flag_clobber = clobber
        self.flag_must = dict((fn.entry_address, C.ALL_FLAGS) for fn in fns)
        changed = True
        while changed:
            changed = False
            for fn in fns:
                v = fn.flag_must_def()
                if v != self.flag_must[fn.entry_address]:
                    self.flag_must[fn.entry_address] = v
                    changed = True

    def provided(self, fn, q, f):
        """The value of flag f that producer q of fn leaves is where a reader after a later call finds it: in a
        local (CPU flags), written to cpu.fpu (x87 condition codes), or in cpu because the caller or callee
        provides it."""
        bit = C.FLAG_BIT[f]
        if q == fn.ENTRY:
            return bool(self.live_in[fn.entry_address] & bit)
        b = fn.branch[q]
        if b[0] == 'call':
            return bool(self.ret_live.get(b[1], 0) & bit)
        if b[0] == 'dyncall':
            return False
        if fn.instructions[q].mnemonic in MEMORY_DEFS:
            return True
        return fn.writes_fc(q) if f == 'fc' else True

    def tail_closure(self, entries):
        """The functions and every function they reach by tail jumps."""
        out = set(entries)
        work = list(out)
        while work:
            fn = self.functions[work.pop()]
            for i in fn.reachable:
                for x in fn.exits[i]:
                    if x.kind == 'tail' and x.target in self.functions and x.target not in out:
                        out.add(x.target)
                        work.append(x.target)
        return out

    def result_mask(self, a):
        """State a function's return can publish: flags only when it may write them (a caller reading a flag the
        callee leaves alone keeps its own value)."""
        if not self.abi_flags:
            return ALL_STATE
        return ALL_REG_STATE | self.flag_clobber.get(a, C.ALL_FLAGS)

    def flag_contract(self):
        """Where this executable could break the flag convention the analysis assumes. Each entry is a site and
        the flags concerned:
        readers: address-taken functions that read flags at entry (an indirect caller passes none);
        indirect_results: indirect calls followed by a read of the callee's flags;
        indirect_returns: returns of functions an indirect jump with live flags may reach (fenced: they write those
        flags back), where a flag comes from a call whose callee does not publish it;
        partial: direct calls followed by a read of a flag the callee writes on some paths only, while the value
        before the call is not where the reader finds it.
        Also, not a violation: indirect_tails, the indirect jumps whose callers read flags (the fence)."""
        out = {'readers': [], 'indirect_results': [], 'indirect_returns': [], 'partial': [], 'indirect_tails': []}
        for a in sorted(self.address_taken):
            if self.live_in[a] & C.ALL_FLAGS:
                out['readers'].append((a, self.live_in[a] & C.ALL_FLAGS))
        for a in sorted(self.functions):
            fn = self.functions[a]
            for i in sorted(fn.reachable):
                after = fn.live_out[i] & C.ALL_FLAGS
                for x in fn.exits[i]:
                    extra = fn.indirect_return_use(x)
                    if extra:
                        mask = 0
                        for f in C.FLAGS:
                            if extra & C.FLAG_BIT[f] and any(q != fn.ENTRY and not self.provided(fn, q, f)
                                                             for q in fn.reaching(i, f)):
                                mask |= C.FLAG_BIT[f]
                        if mask:
                            out['indirect_returns'].append((fn.instructions[i].address, mask))
                    if x.kind == 'dyncall' and after:
                        out['indirect_results'].append((fn.instructions[i].address, after))
                    elif x.kind == 'dyntail' and self.ret_live[a] & C.ALL_FLAGS:
                        out['indirect_tails'].append((fn.instructions[i].address, self.ret_live[a] & C.ALL_FLAGS))
                    elif x.kind == 'call':
                        partial = after & self.flag_clobber.get(x.target, C.ALL_FLAGS) & \
                            ~self.flag_must.get(x.target, 0)
                        mask = 0
                        for f in C.FLAGS:
                            if partial & C.FLAG_BIT[f] and any(not self.provided(fn, q, f) for q in fn.reaching(i, f)):
                                mask |= C.FLAG_BIT[f]
                        if mask:
                            out['partial'].append((fn.instructions[i].address, mask))
        return out

    def analyze(self, log=print):
        fns = list(self.functions.values())
        for fn in fns:
            fn.classify()
            fn.record()
        self.compute_may_def(fns)
        self.stack = stack.analyze(self, fns)
        fpu = fpustack.analyze(self, fns)
        self.fpu_deltas = dict((a, info.delta) for a, info in fpu.items())
        for fn in fns:
            fn.fpu = fpu[fn.entry_address]
        self.compute_may_def(fns, self.stack)
        if self.setjmp in self.functions:
            self.stack[self.setjmp] = stack.StackInfo()
            self.may_def[self.setjmp] = ALL_STATE
        if self.abi_flags:
            self.compute_flag_summaries(fns)
        for fn in fns:
            self.live_in[fn.entry_address] = 0
            self.ret_live[fn.entry_address] = 0
        # Host code (threads, callbacks, window procedures) may read any register after an indirect call.
        host_ret = ALL_REG_STATE
        iteration = 0
        while True:
            iteration += 1
            changed = False
            for fn in fns:
                v = fn.liveness()
                if fn.entry_address in self.flags_preserved:
                    v &= ~C.ALL_FLAGS
                if v != self.live_in[fn.entry_address]:
                    self.live_in[fn.entry_address] = v
                    changed = True
            indirect_live_in = ALL_REG_STATE
            for a in self.address_taken:
                indirect_live_in |= self.live_in[a] & self.indirect_mask
            ret_live = dict((a, 0) for a in self.functions)
            indirect_ret = host_ret
            tail_flags = 0
            tail_targets = set()
            for fn in fns:
                for i in fn.reachable:
                    for x in fn.exits[i]:
                        if x.kind == 'call':
                            if x.target in ret_live:
                                ret_live[x.target] |= fn.live_out[i] & self.result_mask(x.target) & \
                                    ~self.kept(x.target)
                        elif x.kind == 'dyncall':
                            indirect_ret |= fn.live_out[i] & self.indirect_mask
                        elif x.kind == 'tail':
                            if x.target in ret_live:
                                ret_live[x.target] |= self.ret_live[fn.entry_address] & self.result_mask(x.target)
                        elif x.kind == 'dyntail':
                            indirect_ret |= self.ret_live[fn.entry_address] & self.indirect_mask
                            flags = self.ret_live[fn.entry_address] & ~self.indirect_mask
                            if flags:
                                tail_flags |= flags
                                site = fn.instructions[i].address
                                tail_targets |= set(self.jump_targets.get(site, self.address_taken))
            for a in self.address_taken:
                ret_live[a] |= indirect_ret & self.result_mask(a)
            if indirect_live_in != self.indirect_live_in or ret_live != self.ret_live:
                changed = True
            self.indirect_live_in = indirect_live_in
            self.ret_live = ret_live
            self.indirect_tail_flags = tail_flags
            if not changed:
                break
        self.indirect_returners = self.tail_closure(tail_targets & set(self.functions))
        log('liveness: %d iterations; %d address-taken functions; indirect live-in %s; indirect return %s'
            % (iteration, len(self.address_taken), state_names(self.indirect_live_in), state_names(indirect_ret)))
        for fn in fns:
            fn.decide()


def state_names(mask):
    names = [f for f in C.FLAGS if mask & C.FLAG_BIT[f]]
    names += [r for r in C.REGS if mask & (C.REG_BIT[r] << REG_SHIFT)]
    return ','.join(names) or '-'
