#!/usr/bin/env python3
"""V3 provider correctness, lifetime, independent byte oracle and offline ARM gates. MIT."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
from check_values import check

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
READER = ROOT / 'lib/integration/telemetry/v3/detail/Values.cpp'
PROTOCOL = ROOT / 'lib/resource/protocol/Protocol.cpp'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--arm', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--null-checks', action='store_true')
    parser.add_argument('--build-dir', required=True, type=Path)
    args = parser.parse_args()
    out = args.build_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    counts = dict(passed=0, rejected=0)

    def run(command, label, diagnostic=None):
        command = list(map(str, command))
        start = time.perf_counter()
        result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding='utf-8', errors='replace', timeout=180)
        log = f'COMMAND {command!r}\nEXIT {result.returncode}\n{result.stdout}'
        (out / (label + '.log')).write_text(log, encoding='utf-8')
        if diagnostic:
            if not result.returncode or not re.search(diagnostic, result.stdout, re.I | re.S):
                raise RuntimeError(f'{label}: expected rejection missing\n{log}')
            counts['rejected'] += 1
        elif result.returncode:
            raise RuntimeError(f'{label} failed\n{log}')
        else:
            counts['passed'] += 1
        print(f'{label}: pass ({time.perf_counter() - start:.2f}s)', flush=True)
        return result.stdout

    run([args.cxx, '--version'], 'compiler')
    flags = [args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic-errors',
             '-fdiagnostics-color=never', '-Ilib', '-Ilib/boost_pfr/include', '-Ilib/magic_enum']
    if args.null_checks: flags += ['-fno-delete-null-pointer-checks']
    if args.sanitize: flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if args.arm:
        flags += ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard',
                  '-fno-exceptions', '-fno-rtti', '-ffunction-sections', '-fdata-sections']

    # This owns the optional adapter's self-contained header coverage. The
    # Generic resource checks do not impose PFR on resource-only users.
    headers = sorted((ROOT / 'lib/integration/telemetry/v3').rglob('*.hpp'))
    if not headers:
        raise RuntimeError('Optional provider header coverage is empty')
    for i, header in enumerate(headers):
        source = out / f'header-{i}.cpp'
        source.write_text(f'#include <{header.relative_to(ROOT / "lib").as_posix()}>\n')
        run(flags + ['-fsyntax-only', source], 'header-' + str(i))

    if not args.arm:
        for stem in ('Check', 'Edges', 'NoHeap'):
            budgets = (0, 16, 32, 64) if stem == 'Check' else (32,)
            for budget in budgets:
                label = stem + '-' + str(budget)
                exe = out / (label + ('.exe' if os.name == 'nt' else ''))
                run(flags + ['-O2', f'-DTELEMETRY_STRUCTURED_LOCAL_BYTES={budget}', HERE / (stem + '.cpp'),
                             READER, PROTOCOL, '-o', exe], 'build-' + label)
                run([exe, out / 'descriptor.bin', out / 'values.bin'], 'execute-' + label)
                if stem == 'Check': counts['oracle'] = check(out)
        # The application exposes only the generic resource facade. Compile
        # that consumer separately to catch real provider composition errors.
        device = out / ('DeviceCheck' + ('.exe' if os.name == 'nt' else ''))
        run(flags + ['-O2', ROOT / 'tests/resources/DeviceCheck.cpp',
                     ROOT / 'tests/resources/fixtures/resources/DeviceResources.cpp',
                     ROOT / 'tests/resources/fixtures/demo/DemoCatalog.cpp',
                     ROOT / 'lib/telemetry/model/Adapter.cpp',
                     ROOT / 'lib/telemetry/abi/StructuredAbi.cpp', READER, PROTOCOL, '-o', device],
            'build-device-facade')
        run([device], 'execute-device-facade')
    else:
        folder = Path(shutil.which(args.cxx) or args.cxx).resolve().parent
        suffix = '.exe' if os.name == 'nt' else ''
        nm, objdump = [folder / ('arm-none-eabi-' + name + suffix) for name in ('nm', 'objdump')]
        evidence = {}
        for opt in ('O2', 'Os', 'Og'):
            objects = []
            for label, source in (('roots', HERE / 'Arm.cpp'), ('reader', READER)):
                obj = out / (opt + '-' + label + '.o'); objects.append(obj)
                run(flags + ['-' + opt, '-fstack-usage', '-c', source, '-o', obj], 'build-' + opt + '-' + label)
                sections = run([objdump, '-h', obj], 'sections-' + opt + '-' + label)
                if re.search(r'\.(?:init_array|preinit_array|ctors)\b', sections):
                    raise RuntimeError('Static providers require startup code')
            exe = out / (opt + '.elf')
            run(flags + [*objects, '-nostdlib', '-Wl,-e,resource_roots', '-Wl,--gc-sections', '-lc', '-lgcc', '-o', exe], 'link-' + opt)
            symbols = run([nm, '-C', exe], 'symbols-' + opt)
            if re.search(r'\b(?:malloc|calloc|realloc|free|operator new|operator delete|printf|to_chars|Scalar::)\b', symbols):
                raise RuntimeError('Unexpected allocation/formatting/legacy conversion retained')
            sections = run([objdump, '-h', exe], 'linked-sections-' + opt)
            run([objdump, '-drC', exe], 'assembly-' + opt)
            sizes = {n: int(s, 16) for n, s in re.findall(r'^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s', sections, re.M)}
            frames = {}
            for obj in objects:
                for line in obj.with_suffix('.su').read_text().splitlines():
                    name, size, kind = line.split('\t')
                    limit = 128 if opt == 'Og' else 104
                    if kind != 'static' or int(size) > limit:
                        raise RuntimeError(f'Provider frame exceeded {limit} B: ' + line)
                    frames[name] = int(size)
            assert any('readValues(' in n for n in frames)
            evidence[opt] = dict(frames=frames, sections={s: sizes.get(s, 0) for s in ('.text', '.rodata', '.data', '.bss')})
        (out / 'arm-summary.json').write_text(json.dumps(evidence, indent=2) + '\n')
        # Host correctness programs must also instantiate under the MCU compiler.
        for stem in ('Check', 'Edges'):
            run(flags + ['-O2', '-c', HERE / (stem + '.cpp'), '-o', out / (stem + '.o')], 'compile-' + stem)

    for case in range(1, 13):
        run(flags + ['-O2', f'-DCASE={case}', '-fsyntax-only', HERE / 'Negative.cpp'], 'negative-' + str(case),
            r'(?:no matching|no viable|constraints not satisfied|deleted).*?(?:DescriptorFile|ValuesFile|file\()')

    # A real call must retain the full reader ABI dependency under GC and LTO.
    # Changing local-storage policy in just the reader must fail at link time.
    for lto in (False, True):
        label = 'abi-lto' if lto else 'abi-gc'
        extra = ['-flto'] if lto else []
        if lto and 'clang' in Path(args.cxx).name:
            extra += ['-fuse-ld=lld', '-Wno-unused-command-line-argument']
        options = flags + ['-O2', '-ffunction-sections', '-fdata-sections', *extra]
        app = out / (label + '-app.o'); good = out / (label + '-good.o'); bad = out / (label + '-bad.o')
        run(options + ['-DABI_MAIN', '-c', HERE / 'Arm.cpp', '-o', app], label + '-app')
        run(options + ['-c', READER, '-o', good], label + '-good')
        run(options + ['-DTELEMETRY_STRUCTURED_LOCAL_BYTES=0', '-c', READER, '-o', bad], label + '-bad')
        linking = ['-nostdlib', '-Wl,-e,resource_roots', '-lc', '-lgcc'] if args.arm else []
        run(options + [app, good, '-Wl,--gc-sections', *linking, '-o', out / (label + '.elf')], label + '-match')
        run(options + [app, bad, '-Wl,--gc-sections', *linking, '-o', out / (label + '-mismatch.elf')],
            label + '-mismatch', r'undefined (?:reference|symbol).*readValues')

    (out / 'summary.json').write_text(json.dumps(counts, indent=2) + '\n')
    print('Stage 10 checks passed:', counts, flush=True)


if __name__ == '__main__':
    main()
