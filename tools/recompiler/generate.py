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
from disasm import analysis, codegen, discover, image as pe_image, writer  # noqa: E402

EXE_NAME = 'Speed.exe'
EXE_SHA256 = 'c0fad450912952f53809a54690d235aa8b4e25b9f46650693c7cbee1b4c074b7'
MODULE = 'nfsu'

# MSVC CRT setjmp (_setjmp3) and the indirect jump in longjmp that resumes after it. Calls to setjmp save a host
# jump buffer; the longjmp jump restores it (include/recomp.h). None until located in Speed.exe.
SETJMP = None
LONGJMP_JUMPS = {}                      # jump address -> register holding the guest jmp_buf

# Guest functions replaced by hand-written native code (native::sub_<address>, src/nfsu/native.cpp); the translated
# body stays as sub_<address>_guest.
NATIVE_OVERRIDES = []

# Busy-wait loops: the generated code releases the execution lock there so other guest threads can run.
THREAD_SEGMENTS = []

# Net x87 pushes of functions the depth analysis cannot follow (they leave through a computed jump).
FPU_DELTAS = {}

# Functions that save the caller's flags on entry (pushfd) and restore them before returning (popfd).
FLAGS_PRESERVED = []

# Names of the generated files: <module>.cpp/.h, <module>.<n>.cpp and the layout header.
GENERATED_FILE = re.compile(r'^%s(\.\d+)?\.(cpp|h)$|^%s_layout\.h$' % (MODULE, MODULE))


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


def write_analysis(path, program, module, discovery):
    functions = []
    for sub in module.subroutines:
        a = sub.get_entry_point()
        info = program.stack.get(a)
        functions.append({'entry': a, 'origin': discovery.origin[a], 'ret_live': program.ret_live[a],
                          'live_in': program.live_in[a], 'preserved': info.preserved if info else 0,
                          'native': a in program.native_overrides, 'instructions': len(sub.instructions)})
    sections = [{'name': s.name, 'address': s.address, 'length': s.virtual_size} for s in module.image.sections]
    tables = [{'address': t.address, 'targets': t.targets, 'bounded': t.bounded} for t in discovery.tables.values()]
    with open(path, 'w') as f:
        json.dump({'functions': functions, 'sections': sections, 'tables': tables,
                   'gaps': discovery.gaps()}, f)


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
    """Statistics and every place the generated code traps."""
    image = module.image
    origins = collections.Counter(discovery.origin[s.get_entry_point()] for s in module.subroutines)
    instructions = sum(len(s.instructions) for s in module.subroutines)
    code_bytes = sum(discovery.decoded[a].size for a in discovery.code)
    text = sum(s.virtual_size for s in image.code_sections)
    print('functions: %d (%s)' % (len(module.subroutines), ', '.join('%s %d' % kv for kv in sorted(origins.items()))))
    print('instructions: %d emitted, %d distinct (%d of %d code-section bytes)'
          % (instructions, len(discovery.code), code_bytes, text))
    print('jump tables: %d (%d bounded by their switch), byte index tables: %d'
          % (len(discovery.tables), sum(t.bounded for t in discovery.tables.values()), len(discovery.index_tables)))
    if discovery.rejected:
        print('candidates rejected as data: %d (%s)' % (len(discovery.rejected), ', '.join(
            '0x%x' % a for a in sorted(discovery.rejected))))
    gaps = discovery.gaps()
    print('code-section bytes outside functions and tables (not padding): %d in %d ranges'
          % (sum(b - a for a, b in gaps), len(gaps)))
    mnemonics = collections.Counter(discovery.decoded[a].mnemonic for a in discovery.code)
    print('mnemonics: %d distinct' % len(mnemonics))
    traps = collections.defaultdict(list)
    for fn in program.functions.values():
        for i in fn.reachable:
            ins = fn.instructions[i]
            if fn.effects[i].boundary == 'trap':
                traps[ins.mnemonic + (' (undecodable)' if not ins.size else '')].append(ins.address)
    for mnemonic, sites in sorted(traps.items()):
        sites = sorted(set(sites))
        print('trap: %-28s %3d site(s): %s' % (mnemonic, len(sites), ' '.join('0x%x' % a for a in sites[:12])))
    if codegen.unhandled_sites:
        unhandled = collections.Counter(m for _, m in codegen.unhandled_sites)
        print('UNSUPPORTED instructions (trap at run time): %s' % ', '.join('%s x%d' % kv for kv in unhandled.items()))


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
    print('%s: SHA-256 ok, %d sections, %d relocations, %d imports'
          % (EXE_NAME, len(image.sections), len(image.relocations), len(image.imports)))
    os.makedirs(args.out, exist_ok=True)

    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    cs.detail = True
    discovery = discover.Discovery(image, cs)
    discovery.discover(extra_entries=NATIVE_OVERRIDES + ([SETJMP] if SETJMP is not None else []))
    subroutines = [discovery.subroutine(e) for e in sorted(discovery.entries)]
    module = GameModule(image, subroutines)
    program = analysis.Program([module], THREAD_SEGMENTS, [], {}, FLAGS_PRESERVED, (), NATIVE_OVERRIDES,
                               local_registers=not args.memory_registers, local_x87=not args.memory_x87,
                               setjmp=SETJMP, longjmp_jumps=LONGJMP_JUMPS, fpu_check=args.fpu_check,
                               fpu_deltas=FPU_DELTAS, preempt_loops=True)
    program.analyze()
    with tempfile.TemporaryDirectory(prefix='nfsugen-') as stage:
        writer.Writer(module, program, stage).write()
        sync_output(stage, args.out)
    if args.analysis:
        write_analysis(args.analysis, program, module, discovery)
    report(discovery, module, program)
    print('generation took %.0f s' % (time.time() - started))


if __name__ == '__main__':
    main()
