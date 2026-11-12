#!/usr/bin/env python3
"""Qualify additive native access, traversal and resource client APIs.

Host mode executes assertions; ARM mode records sections, individual frames and
manual/helper comparisons without accessing a board. Generated files live only
under the explicitly selected output directory.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'tests/ergonomics'
# The resource server's code as last changed on purpose - SAVE, operation 5
# (d24adef); any other change to it fails the ARM codegen comparison.
BASELINE = 'd24adef'
ABI = ROOT / 'lib/telemetry/abi/StructuredAbi.cpp'
PROTOCOL = ROOT / 'lib/resource/protocol/Protocol.cpp'
FIELD_NEGATIVES = {
    **{n: r'(?:use of deleted function|call to deleted (?:member )?function)' for n in (1, 2, 3, 4, 20, 23, 24)},
    **{n: r'no matching (?:member )?function' for n in (5, 6, 7, 8, 9, 13, 14, 15, 18, 19)},
    10: 'Field readAsResult permits numeric conversion or the exact structural type',
    11: 'Field readBorrowed requires the exact borrowed value type',
    12: 'Field readBorrowed requires the exact borrowed value type',
    16: 'FieldReadResult requires an unqualified object type',
    17: 'FieldReadResult factory must return the exact value type',
    21: 'Field position is outside this table',
    22: 'Packed ID is outside the 32-bit range',
}
TRAVERSAL_NEGATIVES = {
    **{n: 'forEachWhile visitor must return exactly bool'
       for n in (*range(1, 13), *range(28, 36), 38, 39, 41)},
    **{n: r'(?:use of deleted function|call to deleted (?:member )?function)' for n in range(13, 25)},
    **{n: 'forEachEntryWhile visitor must return exactly bool' for n in (25, 26, 27, 36, 40)},
    37: 'ExactFieldWriteArgumentUse_writeAs_for_conversion',
}
NATIVE_NEGATIVES = {
    **{n: r'no matching (?:member )?function' for n in (*range(1, 7), 11, 12, 13)},
    **{n: r'(?:use of deleted function|call to deleted (?:member )?function)' for n in range(7, 11)},
    14: 'NativeCallResult factory must return the exact result type',
}
RESULT_ACCESSOR_NEGATIVES = {
    **{n: r"(?:use of deleted function[^\n]*valueOrNull|call to deleted member function 'valueOrNull')"
       for n in range(1, 7)},
    **{n: r"(?:use of deleted function[^\n]*operator->|call to deleted member function 'operator->')"
       for n in (7, 8)},
    **{n: r"(?:use of deleted function[^\n]*operator->|overload resolution selected deleted operator '->')"
       for n in (9, 10)},
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--arm', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--null-checks', action='store_true')
    args = parser.parse_args()
    if args.arm and args.sanitize:
        parser.error('Sanitizers require host execution')
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # A failed rerun must not retain successful evidence from an earlier run.
    (output / 'summary.json').write_text('{"completed": false}\n', encoding='utf-8')
    compiler = Path(shutil.which(args.cxx) or args.cxx).absolute()
    environment = os.environ.copy()
    environment['PATH'] = str(compiler.parent) + os.pathsep + environment.get('PATH', '')
    environment.setdefault('ASAN_OPTIONS', 'detect_leaks=1:detect_stack_use_after_return=1')
    environment.setdefault('UBSAN_OPTIONS', 'halt_on_error=1')
    report = {'compiler': str(compiler), 'compiler_sha256': digest(compiler),
              'source_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'arm': args.arm, 'sanitize': args.sanitize, 'null_checks': args.null_checks,
              'commands': 0, 'rejections': 0, 'executions': {}, 'sections': {}, 'frames': {},
              'input_hashes': {}, 'completed': False}
    captured = sorted([*SOURCE.glob('*.cpp'), SOURCE / 'run.py', SOURCE / 'Runner.py',
                       ROOT / 'tests/docs/run.py', ROOT / 'tests/codegen/run.py', ABI, PROTOCOL,
                       *ROOT.joinpath('lib/telemetry').rglob('*.hpp'),
                       *ROOT.joinpath('lib/telemetry').rglob('*.h'),
                       *ROOT.joinpath('lib/resource').rglob('*.hpp')])
    before = {path.relative_to(ROOT).as_posix(): digest(path) for path in captured}
    report['source_dirty'] = bool(subprocess.check_output(
        ['git', 'status', '--porcelain=v1'], cwd=ROOT, text=True))

    def run(command, label, diagnostic=None, input_text=None):
        command = list(map(str, command))
        start = time.monotonic()
        result = subprocess.run(command, cwd=ROOT, env=environment, input=input_text,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding='utf-8', errors='replace', timeout=300)
        elapsed = time.monotonic() - start
        (output / f'{label}.log').write_text(
            f'COMMAND {command!r}\nEXIT {result.returncode}\nSECONDS {elapsed:.6f}\n{result.stdout}',
            encoding='utf-8')
        report['commands'] += 1
        if diagnostic is not None:
            if result.returncode == 0 or not re.search(diagnostic, result.stdout, re.I | re.S):
                raise RuntimeError(f'{label}: intended rejection missing\n{result.stdout}')
            report['rejections'] += 1
        elif result.returncode:
            raise RuntimeError(f'{label}: failed; see {output / (label + ".log")}\n{result.stdout[-3000:]}')
        print(f'{label}: pass ({elapsed:.2f}s)', flush=True)
        return result.stdout

    run([compiler, '--version'], 'compiler')
    flags = [str(compiler), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic-errors',
             '-UNDEBUG', '-fdiagnostics-color=never', '-Ilib', '-Ilib/boost_pfr/include',
             '-Ilib/magic_enum']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if args.null_checks:
        flags += ['-fno-delete-null-pointer-checks']
    if args.arm:
        flags += ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard',
                  '-fno-exceptions', '-fno-rtti', '-ffunction-sections', '-fdata-sections',
                  '-fstack-usage']
    report['flags'] = flags
    new_headers = ['telemetry/result/NativeCallResult.hpp', 'telemetry/detail/NativeCall.hpp',
                   'telemetry/result/FieldReadResult.hpp', 'resource/protocol/Wire.hpp',
                   'resource/protocol/Packets.hpp']
    for number, header in enumerate(new_headers):
        run(flags + ['-x', 'c++', '-fsyntax-only', '-'], f'header-{number}',
            input_text=f'#include <{header}>\n')
    # CI may keep its build tree inside the checkout; the regression's generated
    # source copies must still live outside it. Preserve their logs on failure.
    reuse = output / 'runner-reuse'
    reuse.mkdir(exist_ok=True)
    (reuse / 'summary.json').write_text('{"completed": false}\n', encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='telemetry-runner-reuse-') as temporary:
        try:
            run([sys.executable, SOURCE / 'Runner.py', '--build-dir', temporary], 'runner-reuse')
        finally:
            shutil.copytree(temporary, reuse, dirs_exist_ok=True)
    opts = ('O2', 'Os', 'Og') if args.arm else ('O2',)
    positives = ['Basics', 'Traversal', 'Fields', 'NativeCalls', 'ResourceClient', 'ResultAccessors']
    for opt in opts:
        for name in positives:
            support = [PROTOCOL] if name == 'ResourceClient' else [ABI]
            if args.arm:
                for source in [SOURCE / f'{name}.cpp', *support]:
                    run(flags + [f'-{opt}', '-c', source, '-o', output / f'{opt}-{name}-{source.stem}.o'],
                        f'build-{opt}-{name}-{source.stem}')
            else:
                executable = output / (f'{name}' + ('.exe' if os.name == 'nt' else ''))
                run(flags + [f'-{opt}', SOURCE / f'{name}.cpp', *support, '-o', executable], f'build-{name}')
                stdout = run([executable], f'execute-{name}')
                report['executions'][name] = stdout.strip()
    for filename, diagnostics in [('FieldsNegative.cpp', FIELD_NEGATIVES),
                                  ('TraversalNegative.cpp', TRAVERSAL_NEGATIVES),
                                  ('NativeNegative.cpp', NATIVE_NEGATIVES),
                                  ('ResultAccessorsNegative.cpp', RESULT_ACCESSOR_NEGATIVES)]:
        for case, diagnostic in sorted(diagnostics.items()):
            run(flags + ['-O2', f'-DCASE={case}', '-fsyntax-only', SOURCE / filename],
                f'negative-{filename[:-4]}-{case}', diagnostic)
    for targets in (128, 256):
        if args.arm:
            for opt in ('O2', 'Os'):
                obj = output / f'{opt}-NativeScale-{targets}.o'
                run(flags + [f'-{opt}', f'-DTARGETS={targets}', '-c', SOURCE / 'NativeScale.cpp', '-o', obj],
                    f'scale-{opt}-{targets}')
        else:
            executable = output / (f'NativeScale-{targets}' + ('.exe' if os.name == 'nt' else ''))
            run(flags + ['-O2', f'-DTARGETS={targets}', SOURCE / 'NativeScale.cpp', ABI, '-o', executable],
                f'scale-build-{targets}')
            report['executions'][f'NativeScale-{targets}'] = run([executable], f'scale-execute-{targets}').strip()

    if args.arm:
        tools = compiler.parent
        suffix = '.exe' if os.name == 'nt' else ''
        objdump = tools / ('arm-none-eabi-objdump' + suffix)
        objcopy = tools / ('arm-none-eabi-objcopy' + suffix)

        def sections(obj, label):
            run([objdump, '-h', obj], f'sections-{label}')
            if label.startswith('scale-'):
                for function in ('manual_command', 'convenience_command', 'manual_service', 'convenience_service'):
                    run([objdump, '-dr', '--disassemble=' + function, obj], f'disassembly-{label}-{function}')
            else:
                run([objdump, '-dr', obj], f'disassembly-{label}')
            # These are toolchain-generated ELF32 little-endian objects, not
            # input packets. Read sections in one pass instead of launching
            # objcopy once per thunk in a 256-target table.
            raw = obj.read_bytes()
            if raw[:6] != b'\x7fELF\x01\x01':
                raise RuntimeError('Expected an ARM ELF32 little-endian object')
            header = struct.unpack_from('<16sHHIIIIIHHHHHH', raw)
            table = [struct.unpack_from('<10I', raw, header[6] + n * header[11])
                     for n in range(header[12])]
            names = table[header[13]]
            strings = raw[names[4]:names[4] + names[5]]
            values = {}
            for entry in table:
                name = strings[entry[0]:].split(b'\0', 1)[0].decode('utf-8')
                if not name.startswith(('.text', '.rodata')) or entry[5] == 0:
                    continue
                payload = raw[entry[4]:entry[4] + entry[5]]
                values[name] = {'bytes': entry[5], 'sha256': hashlib.sha256(payload).hexdigest()}
            report['sections'][label] = values
            return values

        def usage(obj, label):
            frames = {}
            for line in obj.with_suffix('.su').read_text(encoding='utf-8').splitlines():
                parts = line.rsplit('\t', 2)
                if len(parts) == 3:
                    signature = re.sub(r'^.*?:\d+:\d+:', '', parts[0])
                    frames[signature] = {'bytes': int(parts[1]), 'kind': parts[2]}
            report['frames'][label] = frames
            return frames

        for targets in (128, 256):
            for opt in ('O2', 'Os'):
                obj = output / f'{opt}-NativeScale-{targets}.o'
                sections(obj, f'scale-{opt}-{targets}')
                usage(obj, f'scale-{opt}-{targets}')

        baseline = output / 'ProtocolBaseline.cpp'
        baseline.write_bytes(subprocess.check_output(['git', 'show', f'{BASELINE}:lib/resource/protocol/Protocol.cpp'], cwd=ROOT))
        for opt in opts:
            large = output / f'{opt}-LargeNativeCodegen.o'
            run(flags + [f'-{opt}', '-c', SOURCE / 'LargeNativeCodegen.cpp', '-o', large], f'large-{opt}')
            frames = usage(large, f'large-{opt}')
            if not frames or any(item['bytes'] > 256 for item in frames.values()):
                raise RuntimeError(f'{opt}: large native response hides a stack object: {frames}')
            sections(large, f'large-{opt}')
        for opt in ('O2', 'Os'):
            comparisons = []
            for reference in (True, False):
                label = f'client-{opt}-' + ('reference' if reference else 'helper')
                obj = output / (label + '.o')
                run(flags + [f'-{opt}', *(['-DRESOURCE_CLIENT_REFERENCE'] if reference else []),
                             '-c', SOURCE / 'ResourceClientCodegen.cpp', '-o', obj], label)
                comparisons.append((sections(obj, label), usage(obj, label)))
            if comparisons[0] != comparisons[1]:
                raise RuntimeError(f'{opt}: bounded READ client differs from its checked manual reference')
            protocol_sections = []
            for name, source in [('baseline', baseline), ('current', PROTOCOL)]:
                label = f'protocol-{opt}-{name}'
                obj = output / (label + '.o')
                run(flags + [f'-{opt}', '-Ilib/resource/protocol', '-c', source, '-o', obj], label)
                protocol_sections.append((sections(obj, label), usage(obj, label)))
            if protocol_sections[0] != protocol_sections[1]:
                raise RuntimeError(f'{opt}: existing resource server codegen changed')

    after = {path.relative_to(ROOT).as_posix(): digest(path) for path in captured}
    if before != after:
        raise RuntimeError('Captured library/test inputs changed during qualification')
    report['input_hashes'] = before
    report['completed'] = True
    (output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"Ergonomics: {report['commands']} commands, {report['rejections']} intended rejections; complete", flush=True)


if __name__ == '__main__':
    main()
