"""x87 stack depth relative to function entry.

When every x87 instruction of a function runs at a statically known depth, the registers it pushes live in C++
locals (fp1, fp2, ...; fpN is the N-th value above the entry stack) and the entry stack stays in cpu.fpu. Calls
need the callee's net push count (fpu delta); a function whose depth is not known everywhere keeps the in-memory
stack (cpu.fpu.push/pop/st).

Indirect calls (virtual methods, callbacks) have no static callee. Their delta is 0 unless the code around the call
shows the callee returned a float in st0 (delta 1): the x87 code after the call reads st0 before pushing anything,
or the depth after the call must equal the one other paths bring to the same join or return.
"""

from capstone import x86 as X

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


def analyze_function(fn, deltas, indirect_delta):
    """Depths of fn; indirect call deltas start at indirect_delta and are raised where the code shows a float
    result (infer_sites)."""
    sites = {}
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


def infer_sites(fn, deltas, sites, indirect_delta):
    """Raises the delta of indirect calls whose callee must have returned a float: the first x87 instruction after
    the call reads more registers than the depth there, or the depth after the call differs from the one the other
    paths bring to a join or to the return. Only straight non-x87 code may lie between the call and that point, and
    every other path into it must agree. Returns whether a delta changed."""
    if indirect_delta is None or not any(fn.branch[i][0] == 'dyncall' for i in fn.reachable):
        return False
    if any(fn.instructions[i].mnemonic.split()[-1] in RESET and fn.effects[i].boundary != 'trap'
           for i in fn.reachable):
        return False
    reach, returns = depth_sets(fn, deltas, sites, indirect_delta)

    def single(values):
        return next(iter(values)) if len(values) == 1 and None not in values else None

    def region(starts, entry_value):
        """Ends of the backward region of plain instructions: (fixed depths, {call index: depth after it})."""
        fixed, calls = [], {}
        seen = set()
        work = list(starts)
        if entry_value:
            fixed.append(0)
        while work:
            k = work.pop()
            if k in seen:
                continue
            seen.add(k)
            ins = fn.instructions[k]
            mnemonic = ins.mnemonic.split()[-1]
            kind = fn.branch[k][0]
            if kind == 'dyncall':
                v = single(reach[k])
                if v is None:
                    return None
                calls[k] = v + sites.get(k, indirect_delta)
            elif kind == 'call' or (is_x87(mnemonic) and mnemonic not in CONTROL):
                v = single(reach[k])
                delta = deltas.get(fn.branch[k][1]) if kind == 'call' else effect(mnemonic)
                if v is None or delta is None or delta == NEVER or mnemonic in RESET:
                    return None
                fixed.append(v + delta)
            else:
                if k == fn.entry:
                    fixed.append(0)
                work.extend(fn.pred[k])
        return fixed, calls

    def raise_to(ends, target):
        if ends is None:
            return False
        fixed, calls = ends
        if any(v != target for v in fixed) or not calls or any(v > target for v in calls.values()):
            return False
        changed = False
        for k, v in calls.items():
            new = sites.get(k, indirect_delta) + target - v
            if new > MAX_INDIRECT:
                return False
        for k, v in calls.items():
            if v < target:
                sites[k] = sites.get(k, indirect_delta) + target - v
                changed = True
        return changed

    for j in sorted(fn.reachable):
        values = reach[j]
        if None in values or not values:
            continue
        need = needs(fn.instructions[j])
        if len(values) > 1 or min(values) < need:
            ends = region(fn.pred[j], j == fn.entry)
            if ends is not None:
                target = max(ends[0] + [need] + list(ends[1].values()))
                if raise_to(ends, target):
                    return True
    # Returns at different depths; only for functions that leave through ret alone.
    exits = [(i, x.kind) for i in fn.reachable for x in fn.exits[i] if x.edge != 'call']
    if len(returns) > 1 and None not in returns and all(kind == 'ret' for _, kind in exits):
        ends = region([i for i, _ in exits], False)
        if ends is not None and ends[0] and raise_to(ends, max(ends[0])):
            return True
    return False


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
    """Depths per function entry. An indirect call is taken to keep the depth (host shims never touch the x87
    stack) unless the caller's code shows a float result (Depths.sites). When some address-taken function does not
    provably return at its entry depth, the code after each indirect call checks the depth at run time
    (indirect_fpu_checked)."""
    infos, deltas = fixpoint(program, fns, 0)
    program.indirect_fpu_delta = 0
    program.indirect_fpu_checked = not all(deltas.get(a) in (0, NEVER) for a in program.address_taken)
    return infos
