"""Generates the C++ recompilation of Need for Speed Underground (Speed.exe) into one directory.

Usage: generate.py --binaries <dir> --out <dir> [--analysis <file.json>]

<dir> must hold Speed.exe. Its SHA-256 is checked: the tables below fit only this file.

Files in the output directory are replaced only when their content changes; generated files that are no longer
produced are deleted, other files are left alone.
"""

import argparse
import collections
import filecmp
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disasm'))
import bootstrap  # noqa: E402
bootstrap.ensure_capstone()
import capstone  # noqa: E402
from disasm import analysis, codegen, discover, fpustack, image as pe_image, writer  # noqa: E402

EXE_NAME = 'Speed.exe'
EXE_SHA256 = 'c0fad450912952f53809a54690d235aa8b4e25b9f46650693c7cbee1b4c074b7'
MODULE = 'nfsu'

# MSVC CRT setjmp (_setjmp3) and the indirect jump in longjmp that resumes after it. Calls to setjmp save a host
# jump buffer; the longjmp jump restores it (include/recomp.h). Speed.exe links _setjmp3 (0x6844ac) and longjmp
# (0x684430, resuming at 0x6844a5 through edx), but no code or data refers to either.
SETJMP = None
LONGJMP_JUMPS = {}                      # jump address -> register holding the guest jmp_buf

# Functions that never return although their code has a path to ret: the CRT exits leave through doexit and a
# function pointer. Code after calls to them is padding.
NO_RETURN = [
    0x6720ea,   # exit
    0x6720fb,   # _exit
    0x670c6c,   # _amsg_exit
]

# Guest functions replaced by hand-written native code (native::sub_<address>, src/nfsu/native.cpp); the translated
# body stays as sub_<address>_guest.
NATIVE_OVERRIDES = [
    0x672fc8,   # _ftol: st0 to a 64-bit integer, truncating (sets and restores the x87 control word)
    0x674898,   # _ftol2: the same result without a control word change
    0x6734a0,   # _chkstk / _alloca_probe: page-probing stack allocation
    0x647cb0,   # movie YUV420 -> ARGB converter: the guest has only an MMX path
    # D3DX (include/winapi/d3dx9.h): effects without the guest HLSL compiler; the math entries are jumps through
    # the PSGP dispatch table.
    0x5af9e5,   # D3DXCreateEffectPool
    0x5afb2f,   # D3DXCreateEffect
    0x5ac69c,   # D3DXMatrixMultiply
    0x5abdaf,   # D3DXVec3Transform
    0x5ac443,   # D3DXVec4Transform
    0x5ac8e5,   # D3DXMatrixTranspose
    # EA SND mixer (src/nfsu/audio_mixer.cpp): the voice render without the execution lock, its x87 kernels.
    0x656d70,   # render frames into the DirectSound buffer
    0x65a880,   # dst += src * gain
    0x65a9a0,   # 16-bit -> float
    0x661f60,   # linear interpolation resampler
    0x65fad0,   # EA-XA decoder
    0x65b3f0,   # float -> 16-bit output module
    # Scenery culling (src/nfsu/cull.cpp): the per-frame scenery culler, its visibility of each (view, section) is
    # computed on worker threads; the box against frustum test is native for every caller.
    0x446620,   # scenery culler: every view, every visible section
    0x40db20,   # bounding box against the view frustum: 0 outside, 1 partial, 2 inside
    # Track topology queries (src/nfsu/coll.cpp): ground height and polygon tests for wheels, cameras, AI and
    # effects, and the polygon cache rebuild around a point.
    0x440a40,   # ground height under a point from a topology coordinate's polygon cache
    0x4400e0,   # distance of a point outside a track polygon
    0x4407b0,   # interest box and polygon cache of a topology coordinate
    # Math helpers (src/nfsu/mathfn.cpp): trigonometry by 16-bit angle, lookups, vectors and boxes.
    0x564b10,   # sine of a 16-bit angle
    0x564bb0,   # sine of a float in radians
    0x564bd0,   # sine and cosine of a 16-bit angle
    0x564cd0,   # arcsine as a 16-bit angle
    0x564db0,   # angle of a 2D vector
    0x4387e0,   # float truncated to an integer, as a float
    0x583ea0,   # table value, linear between entries
    0x583f20,   # graph value, linear between points
    0x567370,   # normalise a vector
    0x567450,   # normalise a vector with the w term
    0x567310,   # normalise a 2D vector to a length
    0x540830,   # length with the w term
    0x5677d0,   # grow a box by a point and radius
    0x567740,   # grow a box by a point
    0x567710,   # box of one point
    # Render preparation (src/nfsu/prep.cpp): per-instance light-context fills on the workers.
    0x446730,   # StuffScenery: fill light contexts and submit the view's instances
    0x40a580,   # frame render and present
    # Resource loading (src/nfsu/load.cpp): the JDLZ decompressor, the 8-bit palette texture expansion and the car
    # skin compositor with its pixel blend.
    0x58d190,   # JDLZ decompressor
    0x407db0,   # texture creation: palette textures expanded to A8R8G8B8, levels copied into the D3D texture
    0x5a8a50,   # CompositeSkin32: car skin layers blended into a 32-bit texture
    0x5a8640,   # weighted sum of up to n pixels
    # Light flares (src/nfsu/flares.cpp): the world pass with per-flare visibility on the workers, one flare, and
    # one flare quad.
    0x544f30,   # world light flares of a view
    0x5447c0,   # one light flare: intensity, visibility and its quads
    0x5443f0,   # one flare quad and its submission
    # Draw queue flush setup functions (src/nfsu/draw.cpp): transforms, states, effect constants and slot-pool
    # releases of each packet and node, with direct device and effect calls (include/winapi/d3d9_native.h).
    0x410400,   # world, view and projection transforms of a packet
    0x410550,   # blend enable, source and destination
    0x410650,   # alpha test enable and reference
    0x410700,   # packets of a node back to the packet pool
    0x40ed10,   # a node's packets back to the pool
    0x40e6d0,   # effect group switch
    0x4102f0,   # bone palette of a skinned mesh
    0x40f3d0,   # model texture, alpha, blend, cull and z-write states, texture offset
    0x40f7c0,   # light context: fixed-function lights or the harmonic coefficients
    0x4100a0,   # reflection matrices
    0x40fae0,   # material: fixed-function material or the diffuse, specular and envmap terms
]

# Byte patches applied to the code before discovery: (address, original bytes, new bytes) in hex. Generation stops
# when the executable's bytes differ from the original ones.
PATCHES = [
]

# Busy-wait loops: the generated code releases the execution lock there so other guest threads can run.
THREAD_SEGMENTS = []

# Net x87 pushes of functions the depth analysis cannot follow (they leave through a computed jump).
FPU_DELTAS = {}

# Functions that save the caller's flags on entry (pushfd) and restore them before returning (popfd).
FLAGS_PRESERVED = []

# SIMD code exists only behind CPU feature tests (docs: cpu_threads.md 1.2); the runtime reports no MMX, SSE or
# 3DNow!, so these ranges trap if reached. A SIMD instruction outside them counts as unsupported.
SIMD_GATED = [
    (0x5b0000, 0x620000, 'D3DX: PSGP tables 0x6d4ac0, MMX blitter probe'),
    (0x624000, 0x627000, 'EA render command kernels (SSE/3DNow! opcodes chosen at 0x6212a0)'),
    (0x63f000, 0x649000, 'EA memcpy/memset dispatchers, EA MMX helpers, movie MMX kernels'),
    (0x64f000, 0x663000, 'audio library mixers (flags 0x713eeb, 0x7172f4)'),
    (0x673000, 0x680000, 'CRT libm SSE2 paths (0x792fb8)'),
    (0x682f00, 0x683100, 'D3DX MMX blitter (0x6ecd04)'),
]

# Names of the generated files: <module>.cpp/.h, <module>.<bucket>.cpp and the layout header.
GENERATED_FILE = re.compile(r'^%s(\.[0-9a-f]+)?\.(cpp|h)$|^%s_layout\.h$' % (MODULE, MODULE))


class GameModule:
    """The executable as the analyses and the writer see it."""
    def __init__(self, image, subroutines):
        self.application_name = MODULE
        self.image = image
        self.entry_point = image.entry_point
        self.exported_symbols = []
        self.subroutines = subroutines


def check_binary(directory):
    path = os.path.join(directory, EXE_NAME)
    if not os.path.isfile(path):
        sys.exit('missing %s' % path)
    # Checked before parsing: other files are refused whatever they contain.
    with open(path, 'rb') as f:
        sha256 = hashlib.sha256(f.read()).hexdigest()
    if sha256 != EXE_SHA256:
        sys.exit('%s: SHA-256 %s, expected %s' % (path, sha256, EXE_SHA256))
    return pe_image.Image(path)


def apply_patches(image):
    for address, original, patched in PATCHES:
        original, patched = bytes.fromhex(original), bytes.fromhex(patched)
        s = image.section_of(address)
        if s is None or len(original) != len(patched):
            sys.exit('patch 0x%x: no section or lengths differ' % address)
        offset = address - s.address
        if s.data[offset:offset + len(original)] != original:
            sys.exit('patch 0x%x: bytes %s, expected %s'
                     % (address, s.data[offset:offset + len(original)].hex(), original.hex()))
        s.data = s.data[:offset] + patched + s.data[offset + len(patched):]


def simd_range(address):
    return next((r for r in SIMD_GATED if r[0] <= address < r[1]), None)


def write_analysis(path, program, module, discovery):
    functions = []
    for sub in module.subroutines:
        a = sub.get_entry_point()
        info = program.stack.get(a)
        functions.append({'entry': a, 'origin': discovery.origin[a], 'ret_live': program.ret_live[a],
                          'live_in': program.live_in[a], 'preserved': info.preserved if info else 0,
                          'native': a in program.native_overrides, 'instructions': len(sub.instructions),
                          'no_return': a in discovery.no_return, 'address_taken': a in program.address_taken})
    sections = [{'name': s.name, 'address': s.address, 'length': s.virtual_size} for s in module.image.sections]
    tables = [{'address': t.address, 'targets': t.targets, 'bounded': t.bounded} for t in discovery.tables.values()]
    rejected = [{'address': a, 'origin': o, 'reason': r} for a, (o, r) in sorted(discovery.rejected.items())]
    sources = {'code_refs': {a: sorted(s) for a, s in discovery.code_refs.items()},
               'data_pointers': {a: sorted(s) for a, s in discovery.data_pointers.items()},
               'string_pointers': {a: sorted(s) for a, s in discovery.string_pointers.items()}}
    traps = [{'address': a, 'mnemonic': m, 'class': k} for a, m, k in sorted(set(codegen.simd_sites))]
    code_ranges = []
    for a in sorted(discovery.code):
        end = a + discovery.decoded[a].size
        if code_ranges and a <= code_ranges[-1][1]:
            code_ranges[-1][1] = max(code_ranges[-1][1], end)
        else:
            code_ranges.append([a, end])
    mnemonics = collections.Counter(discovery.decoded[a].mnemonic for a in discovery.code)
    with open(path, 'w') as f:
        json.dump({'functions': functions, 'sections': sections, 'tables': tables, 'gaps': discovery.gaps(),
                   'rejected': rejected, 'sources': sources, 'simd_traps': traps,
                   'unsupported': sorted(set(codegen.unhandled_sites)), 'rounds': discovery.rounds,
                   'index_tables': discovery.index_tables, 'code_ranges': code_ranges,
                   'mnemonics': dict(mnemonics), 'no_return': sorted(discovery.no_return)}, f)


def sync_output(stage, out_dir):
    """Copies the files generated in stage whose content changed (unchanged files keep their timestamps, so the build
    recompiles just the changed ones) and deletes generated files of earlier runs; other files are left alone."""
    names = set(os.listdir(stage))
    changed = removed = 0
    for name in sorted(names):
        assert GENERATED_FILE.match(name), name
        target = os.path.join(out_dir, name)
        if not os.path.isfile(target) or not filecmp.cmp(os.path.join(stage, name), target, shallow=False):
            shutil.copyfile(os.path.join(stage, name), target)
            changed += 1
    for name in os.listdir(out_dir):
        if name not in names and GENERATED_FILE.match(name) and os.path.isfile(os.path.join(out_dir, name)):
            os.remove(os.path.join(out_dir, name))
            removed += 1
    print('generated %d files into %s (%d changed, %d removed)' % (len(names), out_dir, changed, removed))


def report(discovery, module, program):
    """Statistics and every place the generated code traps. Returns the number of problems: unsupported
    instructions in reachable code and SIMD outside the gated ranges."""
    image = module.image
    origins = collections.Counter(discovery.origin[s.get_entry_point()] for s in module.subroutines)
    instructions = sum(len(s.instructions) for s in module.subroutines)
    code_bytes = sum(discovery.decoded[a].size for a in discovery.code)
    text = sum(s.virtual_size for s in image.code_sections)
    print('functions: %d (%s); %d no-return, %d address-taken; %d discovery rounds'
          % (len(module.subroutines), ', '.join('%s %d' % kv for kv in sorted(origins.items())),
             len(discovery.no_return), len(program.address_taken), discovery.rounds))
    print('instructions: %d emitted, %d distinct (%d of %d code-section bytes)'
          % (instructions, len(discovery.code), code_bytes, text))
    print('jump tables: %d (%d bounded by their switch), byte index tables: %d'
          % (len(discovery.tables), sum(t.bounded for t in discovery.tables.values()), len(discovery.index_tables)))
    reasons = collections.Counter(discovery.rejected.values())
    print('pointer candidates rejected: %d (%s); text dwords skipped: %d'
          % (len(discovery.rejected), ', '.join('%s/%s %d' % (o, r, n) for (o, r), n in sorted(reasons.items())),
             len(discovery.string_pointers)))
    gaps = discovery.gaps()
    print('code-section bytes outside functions and tables (not padding): %d in %d ranges'
          % (sum(b - a for a, b in gaps), len(gaps)))
    mnemonics = collections.Counter(discovery.decoded[a].mnemonic for a in discovery.code)
    print('mnemonics: %d distinct' % len(mnemonics))
    calls = fpustack.audit(program)
    print('indirect calls: %d; import table %d (host, no check); float result inferred %d, none inferred %d '
          '(checked: %s); open %d and no static x87 depth %d (stack in cpu.fpu, no check)'
          % (sum(calls.values()), calls.get(('import', 0), 0), calls.get(('inferred', 1), 0),
             calls.get(('inferred', 0), 0), 'yes' if program.indirect_fpu_checked else 'no',
             sum(n for (k, _), n in calls.items() if k == 'open'),
             sum(n for (k, _), n in calls.items() if k == 'unknown')))
    traps = collections.defaultdict(set)
    for fn in program.functions.values():
        for i in fn.reachable:
            ins = fn.instructions[i]
            if fn.effects[i].boundary == 'trap':
                traps[getattr(ins, 'reason', None) or ins.mnemonic].add(ins.address)
    simd = set(a for a, _, _ in codegen.simd_sites)
    for name, sites in sorted(traps.items()):
        if sites & simd:
            continue
        sites = sorted(sites)
        print('trap: %-28s %4d site(s): %s' % (name, len(sites), ' '.join('0x%x' % a for a in sites[:12])))
    problems = 0
    by_range = collections.Counter()
    for a, m, kind in sorted(set(codegen.simd_sites)):
        r = simd_range(a)
        if r is None:
            print('UNGATED SIMD instruction: 0x%x %s (%s)' % (a, m, kind))
            problems += 1
        else:
            by_range[(r[2], kind)] += 1
    for (where, kind), n in sorted(by_range.items()):
        print('gated SIMD trap: %-6s %5d in %s' % (kind, n, where))
    if codegen.unhandled_sites:
        unhandled = collections.Counter(m for _, m in set(codegen.unhandled_sites))
        print('UNSUPPORTED instructions (trap at run time): %s' % ', '.join('%s x%d' % kv for kv in unhandled.items()))
        problems += len(set(codegen.unhandled_sites))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--binaries', required=True, help='directory with %s' % EXE_NAME)
    parser.add_argument('--out', required=True, help='output directory for the generated sources')
    parser.add_argument('--analysis', help='also write the analysis summary (JSON) for test tools')
    parser.add_argument('--memory-x87', action='store_true',
                        help='keep the x87 stack in cpu.fpu instead of C++ locals (for comparisons)')
    parser.add_argument('--memory-registers', action='store_true',
                        help='keep x86 registers in cpu instead of C++ locals (for comparisons)')
    parser.add_argument('--fpu-check', action='store_true',
                        help='assert the x87 depth after every call whose depth is known (finds leaking callees)')
    args = parser.parse_args()
    started = time.time()
    image = check_binary(args.binaries)
    apply_patches(image)
    print('%s: SHA-256 ok, %d sections, %d relocations, %d imports, %d patches'
          % (EXE_NAME, len(image.sections), len(image.relocations), len(image.imports), len(PATCHES)))
    os.makedirs(args.out, exist_ok=True)

    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    cs.detail = True
    discovery = discover.Discovery(image, cs, no_return=NO_RETURN)
    discovery.discover(extra_entries=NATIVE_OVERRIDES + ([SETJMP] if SETJMP is not None else []))
    print('discovery took %.0f s' % (time.time() - started))
    subroutines = [discovery.subroutine(e) for e in sorted(discovery.entries)]
    module = GameModule(image, subroutines)
    program = analysis.Program([module], THREAD_SEGMENTS, [], {}, FLAGS_PRESERVED, (), NATIVE_OVERRIDES,
                               local_registers=not args.memory_registers, local_x87=not args.memory_x87,
                               setjmp=SETJMP, longjmp_jumps=LONGJMP_JUMPS, fpu_check=args.fpu_check,
                               fpu_deltas=FPU_DELTAS, preempt_loops=True, address_taken=discovery.address_taken)
    program.analyze()
    # Every direct call and tail call must reach a generated function.
    missing = sorted(set(x.target for fn in program.functions.values() for i in fn.reachable for x in fn.exits[i]
                         if x.kind in ('call', 'tail') and x.target not in program.functions))
    if missing:
        sys.exit('calls to addresses that are no function: %s' % ' '.join('0x%x' % a for a in missing[:20]))
    with tempfile.TemporaryDirectory(prefix='nfsugen-') as stage:
        writer.Writer(module, program, stage).write()
        sync_output(stage, args.out)
    if args.analysis:
        write_analysis(args.analysis, program, module, discovery)
    problems = report(discovery, module, program)
    print('generation took %.0f s' % (time.time() - started))
    if problems:
        sys.exit('%d unsupported or ungated instruction(s) in reachable code' % problems)


if __name__ == '__main__':
    main()
