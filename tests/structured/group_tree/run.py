#!/usr/bin/env python3
"""Group-tree checks: tree.hex from the producer, refusals, path access, ARM sizes.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
Builds the two-branch fixture (AdcBranch.hpp, MetrologyBranch.hpp) with the
host compiler and every --cross compiler and compares the written descriptor
with tree.hex; with TELEMETRY_TYPE_NAMES=0 with the same tree without type
names. Compiles every case of Negative.cpp (refusals with their diagnostic,
controls that must build). With --arm-cxx: the ARM descriptor bytes and the
size of encoded dispatch, group tree against flat catalogs. Nothing touches a
device.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DESCRIPTOR = ROOT / 'tests/structured/descriptor'
sys.path.insert(0, str(DESCRIPTOR))
import golden  # noqa: E402
from parser import parse  # noqa: E402

_spec = importlib.util.spec_from_file_location('type_names_run', ROOT / 'tests/structured/type_names/run.py')
_type_names = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_type_names)
Compiler = _type_names.Compiler

# Refusals: the regular expression each diagnostic must match.
COMPILE_FAILURES = {
    1: r'invalidGroupTreeName', 2: r'invalidGroupTreeName', 3: r'duplicateGroupTreeName',
    4: r'duplicateGroupTreeName', 5: r'duplicateGroupTreeName',
    6: r'deeper than the profile.s maxGroupDepth', 7: r'groupTreeNameCeilingExceeded',
    8: r'groupTreeNameCeilingExceeded', 9: r'groupTreeNameCeilingExceeded',
    10: r'typeNameCeilingExceeded',
    11: r'No group has this path', 12: r'No endpoint has this path',
    13: r'names a group, not an endpoint', 14: r'another kind of endpoint',
    15: r'No member of this struct has this name', 16: r'member of a value that is not a struct',
    17: r'element past the end of the array', 18: r'take a whole endpoint',
    19: r'ExactFieldWriteArgumentUse_writeAs_for_conversion', 20: r'noEndpointAtPath',
    21: r'another kind of endpoint', 22: r'Malformed path',
}
COMPILE_CONTROLS = (101, 102, 103, 104, 105)
# What each diagnostic names besides its message: the path or the bad name.
NAMED_IN_TRACE = {1: 'adc/x', 2: 'Gain.x', 11: 'adc/channel3/Gain', 17: 'adc/Status.raw[2]',
                  19: 'adc/channel1/Gain', 20: 'adc/nope'}
ARM_FLAGS = ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard', '-fno-exceptions',
             '-fno-rtti', '-ffunction-sections', '-fdata-sections']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cross', action='append', default=[], metavar='LABEL=COMPILER',
                        help='another host compiler for the byte test and the diagnostics; '
                             'a wsl: prefix runs it through WSL')
    parser.add_argument('--arm-cxx', help='ARM GCC: descriptor bytes and dispatch size')
    args = parser.parse_args()
    out = args.build_dir.resolve()
    ignored = ROOT / 'build'
    if out in (ROOT, ignored) or out in ROOT.parents or (
            ROOT in out.parents and ignored not in out.parents):
        parser.error('--build-dir must be outside the checkout or under its ignored build/')
    out.mkdir(parents=True, exist_ok=True)
    counts = dict(passed=0, rejected=0)
    summary = dict(passed=False)
    environment = dict(os.environ)
    if args.sanitize:
        environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

    def run(command, label, diagnostic=None):
        command = [str(part) for part in command]
        result = subprocess.run(command, cwd=ROOT, env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                errors='replace', timeout=600)
        log = f'COMMAND {command!r}\nEXIT {result.returncode}\n{result.stdout}'
        (out / (label + '.log')).write_text(log, encoding='utf-8')
        if diagnostic is not None:
            if result.returncode == 0 or re.search(diagnostic, result.stdout, re.S) is None:
                raise RuntimeError(f'{label}: intended refusal missing\n{log}')
            counts['rejected'] += 1
        elif result.returncode:
            raise RuntimeError(f'{label} failed\n{log}')
        else:
            counts['passed'] += 1
        print(f'{label}: pass', flush=True)
        return result.stdout

    suffix = '.exe' if os.name == 'nt' else ''
    include = ['-Ilib', '-Ilib/boost_pfr/include', '-Ilib/magic_enum']
    strict = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic-errors', '-fdiagnostics-color=never']
    options = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-fno-omit-frame-pointer'] if args.sanitize else ['-O2']

    # 1. The golden and its independent builder agree.
    tree = bytes.fromhex((DESCRIPTOR / 'tree.hex').read_text())
    if golden.fixture('tree') != tree:
        raise RuntimeError('golden.py disagrees with tree.hex')
    model = parse(tree)
    if [g['path'] for g in model['groups']] != ['adc', 'adc/channel1', 'adc/channel2', 'metrology']:
        raise RuntimeError('tree.hex outline differs')
    print('golden: pass', flush=True)

    # 2. The producer on every host compiler, names on and off.
    host = Compiler('host', args.cxx)
    compilers = [host] + [Compiler(*item.split('=', 1)) for item in args.cross]
    produced = {}
    for compiler in compilers:
        for mode, extra in (('names', []), ('nonames', ['-DTELEMETRY_TYPE_NAMES=0'])):
            label = f'{compiler.label}-{mode}'
            program = out / f'Producer-{label}{suffix}'
            flags = strict + include + (options if compiler is host else ['-O2'])
            run(compiler.argv([*flags, *extra, HERE / 'Producer.cpp', '-o', program]), f'build-{label}')
            target = out / f'tree-{label}.bin'
            run(compiler.execute(program, target), f'execute-{label}')
            produced[label] = target.read_bytes()
        if produced[f'{compiler.label}-names'] != tree:
            raise RuntimeError(f'{compiler.label} does not produce tree.hex')
        if produced[f'{compiler.label}-nonames'] != golden.tree_fixture(names={}):
            raise RuntimeError(f'{compiler.label} without type names differs from the unnamed tree')
    summary['producers'] = sorted(produced)
    print('tree.hex from every compiler: ' + ', '.join(c.label for c in compilers), flush=True)

    # 3. Refusals and controls, with what each diagnostic names.
    traces = {}
    for compiler in compilers:
        for case, diagnostic in COMPILE_FAILURES.items():
            text = run(compiler.argv([*strict, *include, '-O2', f'-DCASE={case}', '-fsyntax-only',
                                      HERE / 'Negative.cpp']), f'{compiler.label}-negative-{case}', diagnostic)
            if case in NAMED_IN_TRACE:
                traces[f'{compiler.label}-{case}'] = NAMED_IN_TRACE[case] in text
        for case in COMPILE_CONTROLS:
            run(compiler.argv([*strict, *include, '-O2', f'-DCASE={case}', '-fsyntax-only',
                               HERE / 'Negative.cpp']), f'{compiler.label}-control-{case}')
    summary['named_in_trace'] = traces
    for key, shown in sorted(traces.items()):
        print(f'diagnostic {key} shows the path or name: {"yes" if shown else "no"}', flush=True)

    # 4. ARM: the descriptor bytes, and dispatch size against flat catalogs.
    if args.arm_cxx:
        arm = Compiler('arm', args.arm_cxx)
        run(arm.argv(['--version']), 'arm-version')
        arm_flags = strict + include + ARM_FLAGS
        obj = out / 'arm-tree.o'
        run(arm.argv([*arm_flags, '-O2', '-DGROUP_TREE=1', '-DDESCRIPTOR_SECTION=1', '-c',
                      HERE / 'DispatchSize.cpp', '-o', obj]), 'arm-descriptor-build')
        section = out / 'arm-tree.bin'
        run([arm.tool('objcopy'), '--dump-section', f'.descriptor.tree={section}', obj], 'arm-descriptor-extract')
        if section.read_bytes() != tree:
            raise RuntimeError('ARM does not produce tree.hex')
        sizes = {}
        for form, value in (('flat', 0), ('tree', 1)):
            for opt in ('O2', 'Os'):
                elf = out / f'dispatch-{form}-{opt}.elf'
                run(arm.argv([*arm_flags, '-' + opt, f'-DGROUP_TREE={value}', HERE / 'DispatchSize.cpp',
                              '-nostdlib', '-Wl,-e,dispatch_root', '-Wl,--gc-sections', '-lc', '-lgcc',
                              '-o', elf]), f'dispatch-{form}-{opt}')
                text = run([arm.tool('size'), '-A', elf], f'dispatch-size-{form}-{opt}')
                sections = {name: int(size) for name, size in re.findall(r'^(\.\w+)\s+(\d+)', text, re.M)}
                sizes[f'{form}-{opt}'] = {name: sections.get(name, 0) for name in ('.text', '.rodata', '.data')}
        summary['dispatch_arm_size'] = dict(compiler=arm.command, sections=sizes)
        print('ARM tree bytes equal tree.hex', flush=True)
        for opt in ('O2', 'Os'):
            flat, grown = sizes[f'flat-{opt}'], sizes[f'tree-{opt}']
            print(f'dispatch {opt}: ' + ', '.join(
                f'{name} {grown[name]} B (flat {flat[name]} B, {grown[name] - flat[name]:+d} B)'
                for name in ('.text', '.rodata', '.data')), flush=True)

    summary.update(passed=True, counts=counts, source_head=subprocess.check_output(
        ['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip())
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(f'Group-tree checks passed: {counts}', flush=True)


if __name__ == '__main__':
    main()
