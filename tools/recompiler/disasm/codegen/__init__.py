"""C++ code generators, one per x86 mnemonic: cg_<mnemonic>(ctx, *operands) -> list of statements."""

from capstone import x86 as X

from . import control, fpu, integer, mmx, string
from .ctx import CC_ALIASES

_MODULES = (integer, control, string, fpu, mmx)
_STRING = {'movs': string.cg_movs, 'stos': string.cg_stos, 'scas': string.cg_scas, 'cmps': string.cg_cmps,
           'lods': string.cg_lods}
_PREFIXES = ('rep', 'repe', 'repz', 'repne', 'repnz')


def _lookup(mnemonic):
    for module in _MODULES:
        cg = getattr(module, 'cg_' + mnemonic, None)
        if cg:
            return cg
    return None


unhandled_sites = []    # (address, mnemonic) of instructions the generator cannot translate (record pass)
simd_sites = []         # (address, mnemonic, class) of SIMD instructions that trap (record pass)

_SIMD_GROUPS = ((X.X86_GRP_3DNOW, '3DNow!'), (X.X86_GRP_SSE1, 'SSE'), (X.X86_GRP_SSE2, 'SSE2'),
                (X.X86_GRP_SSE3, 'SSE3'), (X.X86_GRP_MMX, 'MMX'))


def simd_class(ins):
    """'MMX', 'SSE', 'SSE2', 'SSE3' or '3DNow!' for a SIMD instruction, else None."""
    groups = getattr(ins, 'groups', ())
    for group, name in _SIMD_GROUPS:
        if group in groups:
            return name
    for op in getattr(ins, 'operands', []):
        if op.type == X.X86_OP_REG:
            name = ins.reg_name(op.reg)
            if name.startswith('xmm'):
                return 'SSE'
            if name.startswith('mm'):
                return 'MMX'
    return None


def _unhandled(ctx, ins):
    ctx.boundary('trap')
    if ctx.record:
        unhandled_sites.append((ins.address, ins.mnemonic))
    return ['x86::guestTrap(0x%x, "unsupported instruction %s");' % (ins.address, ins.mnemonic)]


def _simd_trap(ctx, ins, kind):
    """SIMD code runs only behind CPU feature tests that the runtime's CPU profile fails: reaching it traps."""
    ctx.boundary('trap')
    if ctx.record:
        simd_sites.append((ins.address, ins.mnemonic, kind))
    return ['x86::guestTrap(0x%x, "%s instruction %s on a CPU-feature-gated path");' % (ins.address, kind, ins.mnemonic)]


def generate(ctx, ins):
    reason = getattr(ins, 'reason', None)
    if reason is not None:
        ctx.boundary('trap')
        return ['x86::guestTrap(0x%x, "%s");' % (ins.address, reason)]
    kind = simd_class(ins)
    if kind is not None:
        mnemonic = ins.mnemonic.split()[-1]
        if kind != 'MMX' or getattr(mmx, 'cg_' + mnemonic, None) is None:
            return _simd_trap(ctx, ins, kind)
    parts = ins.mnemonic.split()
    prefix = parts[0] if len(parts) > 1 else None
    mnemonic = parts[-1]
    operands = list(getattr(ins, 'operands', []))
    if prefix is None and hasattr(ins, 'prefix') and ins.bytes[0] not in tuple(ins.prefix) + (ins.opcode[0],):
        # Capstone drops some F2/F3 prefixes on string instructions; both repeat them on real CPUs.
        if ctx.record:
            print('Warning: Capstone did not recognize instruction prefix for instruction at 0x%08x: %s %s'
                  % (ins.address, ins.mnemonic, ins.op_str))
        prefix = 'rep'
    if prefix in ('repz',):
        prefix = 'repe'
    if prefix in ('repnz',):
        prefix = 'repne'
    if prefix == 'lock':
        # Guest code runs under one execution lock, so a locked instruction is atomic without the prefix.
        prefix = None

    if mnemonic[:4] in _STRING and mnemonic not in ('movsx', 'movzx'):
        if prefix in ('repe', 'repne') and mnemonic[:4] in ('movs', 'stos'):
            prefix = 'rep'
        return _STRING[mnemonic[:4]](ctx, *operands, prefix=prefix)
    if prefix is not None:
        return _unhandled(ctx, ins)
    if mnemonic.startswith('j') and mnemonic[1:] in CC_ALIASES:
        return control.cg_jcc(ctx, mnemonic[1:])
    if mnemonic.startswith('set') and mnemonic[3:] in CC_ALIASES:
        return integer.cg_setcc(ctx, operands[0], mnemonic[3:])
    if mnemonic.startswith('cmov') and mnemonic[4:] in CC_ALIASES:
        return integer.cg_cmovcc(ctx, operands[0], operands[1], mnemonic[4:])
    cg = _lookup(mnemonic)
    if cg is None:
        return _unhandled(ctx, ins)
    return cg(ctx, *operands)
