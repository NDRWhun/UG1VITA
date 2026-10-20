"""x87 stack depth relative to function entry.

When every x87 instruction of a function runs at a statically known depth, the registers it pushes live in C++
locals (fp1, fp2, ...; fpN is the N-th value above the entry stack) and the entry stack stays in cpu.fpu. Calls
need the callee's net push count (fpu delta); a function whose depth is not known everywhere keeps the in-memory
stack (cpu.fpu.push/pop/st).

Indirect calls (virtual methods, callbacks) have no static callee. Calls through the import table reach host shims,
which never change the depth. A guest callee has the standard convention (empty x87 stack at the call, nothing or
one float in st0 at the return), so its delta is 0 unless the caller's code shows the float: an x87 instruction
right after the call reads below what was pushed since, or a delta of 1 removes violations of the convention
(reads below the entry stack, joins or returns at different depths) without adding any. After such a call the
generated code checks the depth and sets the one it expects (x86::fpuCallMismatch logs it). Where the code does not
tell (the result could only reach the return) or the depth there is unknown, the function keeps its stack in
cpu.fpu, which follows whatever the callee left.
"""

from capstone import x86 as X

from .codegen import ctx as C

PUSH = {'fld', 'fild', 'fld1', 'fldz', 'fldpi', 'fldl2e', 'fldln2', 'fptan', 'fsincos'}
POP = {'fstp', 'fistp', 'faddp', 'fsubp', 'fsubrp', 'fmulp', 'fdivp', 'fdivrp', 'fcomp', 'fpatan', 'fyl2x'}
POP2 = {'fcompp'}
RESET = {'fninit', 'finit', 'frstor', 'fnsave', 'fsave', 'fldenv', 'fnstenv', 'fstenv', 'ffree', 'fdecstp',
         'fincstp'}
# x87 instructions that neither read nor move the register stack.
CONTROL = {'fnstcw', 'fstcw', 'fldcw', 'fnstsw', 'fstsw', 'fnclex', 'fclex', 'fwait', 'wait', 'fnop'}
# Without operands these read st(1) as well.
TWO_IMPLICIT = {'fcom', 'fcomp', 'fucom', 'fucomp', 'fcompp', 'fucompp', 'fxch', 'faddp', 'fsubp', 'fsubrp',
                'fmulp', 'fdivp', 'fdivrp', 'fpatan', 'fyl2x', 'fyl2xp1', 'fprem', 'fprem1', 'fscale'}
# Largest net push an indirect callee is inferred to make (a float result).
MAX_INDIRECT = 1


NEVER = 'never'     # delta of a function that never returns


def effect(mnemonic):
    if mnemonic in PUSH:
        return 1
    if mnemonic in POP:
        return -1
    if mnemonic in POP2:
        return -2
    return 0


def is_x87(mnemonic):
    return mnemonic.startswith('f') and mnemonic not in ('fwait',) or mnemonic == 'wait'


def needs(ins):
    """Registers an x87 instruction reads from the stack (pushes from memory read none)."""
    mnemonic = ins.mnemonic.split()[-1]
    if not is_x87(mnemonic) or mnemonic in CONTROL or mnemonic in RESET:
        return 0
    regs = []
    for op in ins.operands:
        if op.type == X.X86_OP_REG:
            name = ins.reg_name(op.reg)
            if name.startswith('st'):
                regs.append(int(name[-2] if name.endswith(')') else name[-1]))
    if mnemonic in PUSH:
        return max(regs) + 1 if regs else 0
    implicit = 2 if not ins.operands and mnemonic in TWO_IMPLICIT else 1
    return max([implicit] + [r + 1 for r in regs])


class Depths:
    def __init__(self):
        self.depth = None       # per instruction, depth before it (None: unknown)
        self.local = False      # the function keeps pushed values in locals
        self.delta = None       # net pushes from entry to return, when every path agrees
        self.sites = {}         # indirect call index -> inferred net pushes of its callee (absent: 0)
        self.evidence = {}      # indirect call index -> how its delta is known (see evidence)


def analyze_function(fn, deltas, indirect_delta):
    """Depths of fn; indirect call deltas start at indirect_delta and are raised where the code shows a float
    result (reads_result, then infer_sites)."""
    sites = {}
    if indirect_delta is not None and indirect_delta < MAX_INDIRECT:
        sites = dict((i, MAX_INDIRECT) for i in reads_result(fn))
    while True:
        result = walk(fn, deltas, sites, indirect_delta)
        if not infer_sites(fn, deltas, sites, indirect_delta):
            result.sites = dict((i, v) for i, v in sites.items() if v != indirect_delta)
            return result


def depth_sets(fn, deltas, sites, indirect_delta):
    """Every depth each instruction is reached with (None: unknown), and the depths at the function's exits."""
    n = fn.n
    reach = [set() for _ in range(n)]
    reach[fn.entry].add(0)
    work = [fn.entry]
    returns = set()
    while work:
        i = work.pop()
        kind = fn.branch[i][0]
        if kind == 'call' and deltas.get(fn.branch[i][1]) == NEVER:
            continue
        if kind == 'call':
            delta = deltas.get(fn.branch[i][1])
        elif kind == 'dyncall':
            delta = sites.get(i, indirect_delta)
        else:
            delta = effect(fn.instructions[i].mnemonic.split()[-1])
        after = set(None if v is None or delta is None or abs(v + delta) > 16 else v + delta for v in reach[i])
        for x in fn.exits[i]:
            if x.edge == 'call':
                continue
            if x.kind == 'ret':
                returns |= after
            elif x.kind == 'tail':
                d = deltas.get(x.target)
                if d != NEVER:
                    returns |= set(None if v is None or d is None else v + d for v in after)
            else:
                returns |= set(None if v is None or indirect_delta is None else v + indirect_delta for v in after)
        for s in fn.succ[i]:
            if not after <= reach[s]:
                reach[s] |= after
                work.append(s)
    return reach, returns


def dyncalls(fn):
    return sorted(i for i in fn.reachable if fn.branch[i][0] == 'dyncall')


def import_calls(fn):
    """Indirect calls through the import address table: call [slot], or call reg with reg loaded from a slot on the
    straight path before it (callee-saved registers also across calls). Host shims never change the x87 depth."""
    cached = getattr(fn, 'fpu_import_calls', None)
    if cached is not None:
        return cached
    slots = fn.program.import_slots
    found = set()
    for i in dyncalls(fn):
        op = fn.instructions[i].operands[0]
        if op.type == X.X86_OP_MEM:
            if op.mem.base == 0 and op.mem.index == 0 and op.mem.disp & 0xffffffff in slots:
                found.add(i)
            continue
        if op.type != X.X86_OP_REG:
            continue
        reg = fn.instructions[i].reg_name(op.reg)
        kept = reg in ('ebx', 'ebp', 'esi', 'edi')
        k = i
        while len(fn.pred[k]) == 1 and fn.pred[k][0] == k - 1:
            k -= 1
            ins = fn.instructions[k]
            if ins.mnemonic == 'mov' and ins.operands[0].type == X.X86_OP_REG \
                    and ins.reg_name(ins.operands[0].reg) == reg and ins.operands[1].type == X.X86_OP_MEM:
                mem = ins.operands[1].mem
                if mem.base == 0 and mem.index == 0 and mem.disp & 0xffffffff in slots:
                    found.add(i)
                break
            if fn.effects[k].reg_def & C.REG_BIT[reg] or (fn.branch[k][0] in ('call', 'dyncall') and not kept):
                break
    fn.fpu_import_calls = found
    return found


def guest_calls(fn):
    """Indirect calls that may reach guest code."""
    imports = import_calls(fn)
    return [i for i in dyncalls(fn) if i not in imports]


def reads_result(fn):
    """Indirect calls followed, before any branch, call or join, by an x87 instruction that reads below what the
    code pushed since the call. The x87 stack is empty at a call to a function with the standard convention, so
    that register is the callee's float result."""
    cached = getattr(fn, 'fpu_reads_result', None)
    if cached is not None:
        return cached
    found = set()
    for i in guest_calls(fn):
        pushed = 0
        k = i
        while fn.succ[k] == [k + 1] and len(fn.pred[k + 1]) == 1 and not fn.exits[k + 1]:
            k += 1
            ins = fn.instructions[k]
            mnemonic = ins.mnemonic.split()[-1]
            if fn.branch[k][0] != 'next' or mnemonic in RESET or fn.effects[k].boundary == 'trap':
                break
            if needs(ins) > pushed:
                found.add(i)
                break
            pushed += effect(mnemonic)
    fn.fpu_reads_result = found
    return found


def resets(fn):
    return any(fn.instructions[i].mnemonic.split()[-1] in RESET and fn.effects[i].boundary != 'trap'
               for i in fn.reachable)


def violations(fn, deltas, sites, indirect_delta):
    """Places where the depths break the x87 calling convention: paths joining at different depths, a register read
    below the entry stack, an indirect call with values left on the stack (its callee may use all eight registers),
    returns at different depths, or an address-taken function returning other than nothing or one float."""
    reach, returns = depth_sets(fn, deltas, sites, indirect_delta)
    bad = set()
    for j in fn.reachable:
        values = reach[j]
        if len(values) > 1:
            bad.add(('join', j))
        values = values - {None}
        if not values:
            continue
        if fn.branch[j][0] == 'dyncall':
            if values != {0}:
                bad.add(('call', j))
        elif min(values) < needs(fn.instructions[j]):
            bad.add(('read', j))
    known = set(v for v in returns if v is not None)
    if len(known) > 1:
        bad.add(('ret', None))
    if fn.entry_address in fn.program.address_taken and known - {0, 1}:
        bad.add(('abi', None))
    return bad


def infer_sites(fn, deltas, sites, indirect_delta):
    """Raises by one the delta of an indirect call whose callee must have returned a float: raising it removes
    violations (st0 read after the call, a join or the return expecting one more value) and adds none at a place
    without one. Of several, the one leaving the fewest, then the last. Returns whether a delta changed."""
    calls = guest_calls(fn)
    if indirect_delta is None or not calls or resets(fn):
        return False
    base = violations(fn, deltas, sites, indirect_delta)
    if not base:
        return False
    places = set(j for _, j in base)
    best = None
    for i in calls:
        value = sites.get(i, indirect_delta)
        if value >= MAX_INDIRECT:
            continue
        trial = dict(sites)
        trial[i] = value + 1
        v = violations(fn, deltas, trial, indirect_delta)
        if len(v) < len(base) and set(j for _, j in v) <= places and (best is None or len(v) <= best[0]):
            best = (len(v), i)
    if best is None:
        return False
    sites[best[1]] = sites.get(best[1], indirect_delta) + 1
    return True


def evidence(fn, info, deltas, indirect_delta):
    """Per indirect call of fn (depths info): 'import' (proven: a host shim), 'unknown' (no static depth there),
    'inferred' (the other delta, 0 or 1, breaks the convention somewhere the chosen one does not) or 'open' (the
    caller's code does not tell: the callee's result, if any, only reaches the return)."""
    out = dict((i, 'import') for i in import_calls(fn))
    calls = guest_calls(fn)
    if not calls:
        return out
    sites = dict(info.sites)
    base = None if indirect_delta is None or resets(fn) else violations(fn, deltas, sites, indirect_delta)
    for i in calls:
        if info.depth[i] is None:
            out[i] = 'unknown'
        elif base is None:
            out[i] = 'open'
        else:
            trial = dict(sites)
            trial[i] = 1 - sites.get(i, indirect_delta)
            out[i] = 'inferred' if violations(fn, deltas, trial, indirect_delta) - base else 'open'
    return out


def walk(fn, deltas, sites, indirect_delta):
    n = fn.n
    depth = [None] * n
    known = [False] * n
    conflict = [False] * n
    depth[fn.entry] = 0
    known[fn.entry] = True
    work = [fn.entry]
    returns = set()
    reset = False
    while work:
        i = work.pop()
        d = None if conflict[i] else depth[i]
        mnemonic = fn.instructions[i].mnemonic.split()[-1]
        if mnemonic in RESET and fn.effects[i].boundary != 'trap':
            reset = True
        after = d
        kind = fn.branch[i][0]
        if kind == 'call' and deltas.get(fn.branch[i][1]) == NEVER:
            continue
        if d is not None:
            if kind == 'call':
                delta = deltas.get(fn.branch[i][1])
                after = d + delta if delta is not None else None
            elif kind == 'dyncall':
                delta = sites.get(i, indirect_delta)
                after = d + delta if delta is not None else None
            else:
                after = d + effect(mnemonic)
        for x in fn.exits[i]:
            if x.edge == 'call':
                continue
            if after is None:
                returns.add(None)
            elif x.kind == 'ret':
                returns.add(after)
            elif x.kind == 'tail':
                delta = deltas.get(x.target)
                if delta != NEVER:
                    returns.add(after + delta if delta is not None else None)
            else:
                returns.add(after + indirect_delta if indirect_delta is not None else None)
        for s in fn.succ[i]:
            if not known[s]:
                known[s] = True
                depth[s] = after
                work.append(s)
            elif not conflict[s] and depth[s] != after:
                conflict[s] = True
                depth[s] = None
                work.append(s)
    result = Depths()
    result.depth = [None if conflict[i] or not known[i] else depth[i] for i in range(n)]
    if len(returns) == 1:
        (result.delta,) = returns
    elif not returns:
        result.delta = NEVER
    if reset:
        # fninit and the like set an absolute depth: nothing is relative to the entry any more.
        result.delta = None
        result.depth = [None] * n
        return result
    # Paths reaching one instruction at different depths (data-dependent pops): no single mapping of st(i) to
    # locals, and cpu.fpu.count could not be written at the boundaries after it.
    if any(conflict[i] for i in fn.reachable):
        return result
    uses = False
    for i in fn.reachable:
        mnemonic = fn.instructions[i].mnemonic.split()[-1]
        if fn.branch[i][0] == 'skip' or not is_x87(mnemonic):
            continue
        uses = True
        if result.depth[i] is None:
            return result
    if uses and fn.program.local_x87:
        result.local = True
    return result


def fixpoint(program, fns, indirect):
    # Least fixpoint: every function starts as never returning; deltas only move up to a value, then unknown.
    declared = program.declared_fpu_deltas
    deltas = dict((fn.entry_address, declared.get(fn.entry_address, NEVER)) for fn in fns)
    infos = {}
    for _ in range(500):
        changed = False
        for fn in fns:
            info = analyze_function(fn, deltas, indirect)
            if fn.entry_address in declared:
                info.delta = declared[fn.entry_address]
            infos[fn.entry_address] = info
            if deltas.get(fn.entry_address) != info.delta:
                deltas[fn.entry_address] = info.delta
                changed = True
        if not changed:
            return infos, deltas
    # The optimistic start (never returning) is only sound once the deltas are stable.
    raise AssertionError('x87 depth analysis did not converge')


def analyze(program, fns):
    """Depths per function entry. An indirect call is taken to keep the depth unless the caller's code shows a float
    result (Depths.sites). When some address-taken function does not provably return at its entry depth, the code
    after each indirect call into guest code checks the depth at run time (indirect_fpu_checked)."""
    infos, deltas = fixpoint(program, fns, 0)
    program.indirect_fpu_delta = 0
    program.indirect_fpu_checked = not all(deltas.get(a) in (0, NEVER) for a in program.address_taken)
    for fn in fns:
        info = infos[fn.entry_address]
        info.evidence = evidence(fn, info, deltas, 0)
        # Where the code does not tell, the x87 stack stays in cpu.fpu: it follows whatever the callee returns.
        if any(kind in ('open', 'unknown') for kind in info.evidence.values()):
            info.local = False
    return infos


def audit(program):
    """Indirect call sites by evidence (see evidence) and delta, in reachable code."""
    counts = {}
    for fn in program.functions.values():
        for i, kind in fn.fpu.evidence.items():
            key = (kind, fn.fpu.sites.get(i, program.indirect_fpu_delta))
            counts[key] = counts.get(key, 0) + 1
    return counts
