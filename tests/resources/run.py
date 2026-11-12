#!/usr/bin/env python3
"""Independent C++20 resource core/protocol checks; telemetry has its own suite.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
No PFR, telemetry headers or optional adapter is needed by this runner.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from stack_check import check_usage, self_test

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'tests/structured/qualification'))
from gates import forbidden_symbols  # noqa: E402


def counted_conditions(text, expected):
    """A shortened or duplicated positive report cannot qualify the suite."""
    rows = re.findall(r'^.*?(\d+) (?:checks|controls) passed\s*$', text, re.M)
    if len(rows) != 1 or int(rows[0]) != expected:
        raise RuntimeError(f'Expected exactly {expected} counted conditions')
    return expected


def condition_controls():
    for text in ('Resource: 34 controls passed\n', '34 checks passed\n'):
        counted_conditions(text, 34)
    for text in ('', '0 checks passed', '1 checks passed', '33 checks passed',
                 '35 checks passed', '34 checks passed\n34 checks passed\n'):
        try:
            counted_conditions(text, 34)
        except RuntimeError:
            continue
        raise AssertionError('Incomplete or duplicated condition report accepted')
    return {'positive': 2, 'refused': 6}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--null-checks', action='store_true')
    parser.add_argument('--arm', action='store_true')
    parser.add_argument('--objdump')
    parser.add_argument('--size')
    args = parser.parse_args()
    self_test()
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # clang++ can be a symlink to clang; retain the C++ driver spelling so
    # host links select the C++ runtime rather than the C driver defaults.
    compiler = Path(shutil.which(args.cxx) or args.cxx).absolute()
    environment = os.environ.copy()
    environment['PATH'] = str(compiler.parent) + os.pathsep + environment.get('PATH', '')
    environment.setdefault('ASAN_OPTIONS', 'detect_leaks=1:detect_stack_use_after_return=1')
    environment.setdefault('UBSAN_OPTIONS', 'halt_on_error=1')
    report = {'commands': 0, 'rejections': 0, 'conditions': 0, 'headers': [],
              'execution': 'compile/link only' if args.arm else 'host'}
    report['condition_controls'] = condition_controls()

    def run(command, label, *, diagnostic=None, input_text=None):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=ROOT, env=environment, input=input_text,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                encoding='utf-8', errors='replace', timeout=180)
        (output / (label + '.log')).write_text(
            f'COMMAND {command!r}\nEXIT {result.returncode}\n{result.stdout}', encoding='utf-8')
        report['commands'] += 1
        if diagnostic:
            if result.returncode == 0 or not re.search(diagnostic, result.stdout, re.I):
                raise RuntimeError(label + ': intended refusal missing\n' + result.stdout)
            report['rejections'] += 1
        elif result.returncode:
            raise RuntimeError(label + '\n' + result.stdout)
        print(label + ': pass', flush=True)
        return result.stdout

    run([compiler, '--version'], 'compiler')
    flags = [compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic-errors',
             '-fdiagnostics-color=never', '-Ilib']
    if args.null_checks:
        flags += ['-fno-delete-null-pointer-checks']
    if args.sanitize:
        flags += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined,float-cast-overflow',
                  '-fsanitize-address-use-after-scope', '-fno-sanitize-recover=all']
    if args.arm:
        flags += ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard',
                  '-fno-exceptions', '-fno-rtti', '-ffunction-sections', '-fdata-sections']
    headers = sorted((ROOT / 'lib/resource').glob('*.hpp'))
    headers += sorted((ROOT / 'lib/resource/protocol').glob('*.hpp'))
    if {path.relative_to(ROOT / 'lib').as_posix() for path in headers} != {
            'resource/Types.hpp', 'resource/File.hpp', 'resource/FileSystem.hpp',
            'resource/FileView.hpp', 'resource/BytesFile.hpp', 'resource/Resource.hpp',
            'resource/ChunkWriter.hpp', 'resource/StagedFile.hpp', 'resource/protocol/Protocol.hpp',
            'resource/protocol/Wire.hpp', 'resource/protocol/Packets.hpp'}:
        raise RuntimeError('Generic resource header coverage changed')
    for index, header in enumerate(headers):
        relative = header.relative_to(ROOT / 'lib').as_posix()
        run(flags + ['-x', 'c++', '-fsyntax-only', '-'], 'header-' + str(index),
            input_text='#include <' + relative + '>\n')
        report['headers'].append(relative)
    deleted = r"call to deleted function ['\u2018]file|use of deleted function [^\n]*resource::file"
    for case in range(39):
        if 35 <= case <= 37:
            member = ('object', 'ops', 'path')[case - 35]
            diagnostic = rf'(?:\b{member}\b[^\n]*private|private[^\n]*\b{member}\b)'
        elif case == 38:
            diagnostic = r'(?:FileEntry[^\n]*private|private[^\n]*FileEntry)'
        else:
            diagnostic = (deleted if case in (1, 8, 13, 14) or case >= 15 else
                          r'deleted' if case == 7 else
                          r'invalidDefinition' if case in (2, 3, 4, 5, 6, 12) else
                          r'satisfaction of .Provider|does not satisfy .Provider' if case in (9, 10, 11) else None)
        run(flags + [f'-DCASE={case}', '-fsyntax-only', HERE / 'Negative.cpp'],
            f'negative-{case}', diagnostic=diagnostic)
    for case in range(17):
        run(flags + [f'-DCASE={case}', '-fsyntax-only', HERE / 'BytesFileNegative.cpp'],
            f'bytes-negative-{case}',
            diagnostic=r'(?:deleted|no matching|no viable).*BytesFile' if case else None)
    if not args.arm:
        run(flags + ['-DCASE=17', '-fsyntax-only', HERE / 'BytesFileNegative.cpp'],
            'bytes-oversized', diagnostic=r'bytesFileSizeExceeded')
    for case in range(14):
        diagnostic = (
            r"deleted[^\n]*(?:operator[\s'\u2018\u2019]*\[\]|begin|end)|use of deleted function[^\n]*FileSystem"
            if 1 <= case <= 6 else
            r'no match|constraints not satisfied' if case in (7, 8) else
            r'(?:cannot convert|no viable conversion).*FileView' if case == 9 else
            r'no member named .*object' if case == 10 else
            r'no member named .*ops' if case == 11 else
            r'(?:private.*FileView|FileView.*private)' if case == 12 else
            r'(?:private.*FileIterator|FileIterator.*private)' if case == 13 else None)
        run(flags + [f'-DCASE={case}', '-fsyntax-only', HERE / 'FileViewNegative.cpp'],
            f'file-view-negative-{case}', diagnostic=diagnostic)
    if args.arm:
        def tool(name):
            return compiler.with_name('arm-none-eabi-' + name + compiler.suffix)
        for opt in ('O2', 'Os'):
            run(flags + ['-' + opt, '-c', HERE / 'BoundaryCheck.cpp',
                         '-o', output / (opt + '-BoundaryCheck.o')], 'boundary-check-' + opt)
            run(flags + ['-' + opt, '-c', HERE / 'BytesFileCheck.cpp',
                         '-o', output / (opt + '-BytesFileCheck.o')], 'bytes-check-' + opt)
            run(flags + ['-' + opt, '-c', HERE / 'StagedFileCheck.cpp',
                         '-o', output / (opt + '-StagedFileCheck.o')], 'staged-check-' + opt)
            run(flags + ['-' + opt, '-c', HERE / 'FileViewCheck.cpp',
                         '-o', output / (opt + '-FileViewCheck.o')], 'file-view-check-' + opt)
            run(flags + ['-' + opt, '-c', ROOT / 'examples/resources/Files.cpp',
                         '-o', output / (opt + '-ResourceExample.o')], 'resource-example-' + opt)
            protocol = output / (opt + '-Protocol.o')
            probe = output / (opt + '-ArmProbe.o')
            for source, obj in ((ROOT / 'lib/resource/protocol/Protocol.cpp', protocol),
                                (HERE / 'ArmProbe.cpp', probe)):
                run(flags + ['-' + opt, '-fstack-usage', '-c', source, '-o', obj], obj.stem)
            usage = check_usage(protocol.with_suffix('.su').read_text(encoding='utf-8'), required='process')
            report.setdefault('stack', {})[opt] = usage
            assembly = run([args.objdump or tool('objdump'), '-drC', probe], 'assembly-' + opt)
            symbols = run([args.objdump or tool('objdump'), '-t', probe], 'storage-' + opt)
            if not re.search(r'\.rodata\S*\s+\S+\s+resource_probe_files', symbols):
                raise RuntimeError('File table left constant storage')
            body = re.search(r'<resource_probe_known>:\n(.*?)(?=\n[0-9a-f]+ <|\Z)', assembly, re.S)
            if not body or re.search(r'\bblx\b|\bbx\s+r[0-9]+', body[1]):
                raise RuntimeError('Known provider gained indirect dispatch')
            image = output / (opt + '-resources.elf')
            run(flags + ['-' + opt, probe, protocol, '--specs=nano.specs', '--specs=nosys.specs',
                         '-Wl,--gc-sections', '-o', image], 'link-' + opt)
            linked = run([tool('nm'), '-C', image], 'symbols-' + opt)
            if forbidden_symbols(linked):
                raise RuntimeError('Allocation/formatting entered generic resource image')
            run([args.size or tool('size'), '-A', image], 'sections-' + opt)
    else:
        for stem, sources in (('CoreCheck', [ROOT / 'lib/resource/protocol/Protocol.cpp']),
                              ('BoundaryCheck', [ROOT / 'lib/resource/protocol/Protocol.cpp']),
                              ('NoHeapCheck', [ROOT / 'lib/resource/protocol/Protocol.cpp']),
                              ('BytesFileCheck', []),
                              ('StagedFileCheck', []),
                              ('FileViewCheck', []),
                              ('Negative', [])):
            program = output / (stem + ('.exe' if os.name == 'nt' else ''))
            run(flags + ['-O1' if args.sanitize else '-O2', '-DCASE=0', HERE / (stem + '.cpp'),
                         *sources, '-o', program], 'build-' + stem)
            result = run([program], 'execute-' + stem)
            expected = {'CoreCheck': 4198, 'BoundaryCheck': 306653, 'NoHeapCheck': 5000, 'BytesFileCheck': 122, 'StagedFileCheck': 36,
                        'FileViewCheck': 88, 'Negative': 34}[stem]
            report['conditions'] += counted_conditions(result, expected)
        example = output / ('ResourceExample' + ('.exe' if os.name == 'nt' else ''))
        run(flags + ['-O1' if args.sanitize else '-O2', ROOT / 'examples/resources/Files.cpp',
                     '-o', example], 'build-resource-example')
        result = run([example], 'execute-resource-example')
        report['conditions'] += counted_conditions(result, 23)
    (output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: report[key] for key in ('commands', 'rejections', 'conditions', 'execution')}))


if __name__ == '__main__':
    main()
