#!/usr/bin/env python3
"""Mixed endpoints, bindings, compiled adapters and offline ARM qualification.

Authors: Ruslan Kovtun (shpegun60), codexAi. MIT license.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
ADAPTER = ROOT / 'lib/telemetry/model/Adapter.cpp'
ABI = ROOT / 'lib/telemetry/abi/StructuredAbi.cpp'
NO_MATCH = r'no matching (?:member )?function'
DELETED = r'deleted (?:constructor|function|member)'
DIAGNOSTICS = {
    1: 'Field getter must take no arguments',
    2: 'Field getter must return an unqualified native value',
    3: 'Structured endpoints must be noexcept',
    4: 'Field setter must accept the exact getter type',
    5: 'Field setter must return telemetry::WriteResult',
    6: 'Request must be by value or const lvalue reference',
    7: 'Command request must be an aggregate struct or void',
    8: 'Command must return telemetry::CommandResult',
    9: 'Use one request structure',
    10: 'Request must be by value or const lvalue reference',
    11: 'Structured endpoints must be noexcept',
    **{i: NO_MATCH for i in (12, 13, 14, 15, 16)},
    17: 'Field position is outside this table',
    18: 'Field group is outside this catalog',
    19: 'Command position is outside this table',
    20: 'Command group is outside this catalog',
    21: NO_MATCH, 22: NO_MATCH, 23: DELETED, 24: DELETED,
    25: r'cannot bind.*lvalue reference|no matching function',
    26: 'Field catalog groups require FieldTable instances',
    27: 'Packed ID must be an integer',
    28: 'Structured field accepts name and bindings only',
    29: 'Structured field accepts name and bindings only',
    30: 'Structured command accepts name and one binding only',
    31: 'Endpoint target cannot be nullptr', 32: 'Endpoint target cannot be nullptr',
    33: NO_MATCH, 34: DELETED, 35: NO_MATCH, 36: NO_MATCH,
    37: NO_MATCH, 38: NO_MATCH,
    39: r'no matching (?:function|constructor)',
    40: r'no matching (?:function|constructor)',
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    p.add_argument('--arm', action='store_true')
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--null-checks', action='store_true')
    p.add_argument('--local-bytes', type=int)
    p.add_argument('--build-dir', type=Path, required=True)
    args = p.parse_args()
    if args.arm and args.sanitize:
        p.error('ARM runtime sanitizer is not provided by this runner')
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    counts = {'passed': 0, 'rejected': 0, 'payload_conditions': 0}

    def run(command, label, failure=''):
        start = time.perf_counter()
        result = subprocess.run([str(x) for x in command], cwd=ROOT,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            encoding='utf-8', errors='replace', timeout=180)
        log = f'COMMAND {command!r}\nEXIT {result.returncode}\n{result.stdout}'
        (output / (label + '.log')).write_text(log, encoding='utf-8')
        if failure:
            if not result.returncode or re.search(failure, result.stdout, re.I | re.S) is None:
                raise RuntimeError(f'{label}: intended diagnostic missing\n{log}')
            counts['rejected'] += 1
        elif result.returncode:
            raise RuntimeError(f'{label} failed\n{log}')
        else:
            counts['passed'] += 1
        print(f'{label}: pass ({time.perf_counter()-start:.2f}s)', flush=True)
        return result.stdout

    run([args.cxx, '--version'], 'compiler')
    flags = [args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic-errors',
        '-fdiagnostics-color=never', '-Ilib', '-Ilib/boost_pfr/include', '-Ilib/magic_enum']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if args.null_checks:
        flags += ['-fno-delete-null-pointer-checks']
    if args.local_bytes is not None:
        flags += ['-DTELEMETRY_STRUCTURED_LOCAL_BYTES='+str(args.local_bytes)]
    if args.arm:
        flags += ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard',
            '-fno-exceptions', '-fno-rtti', '-ffunction-sections', '-fdata-sections', '-fstack-usage']
    optimizations = ('O2', 'Os', 'Og') if args.arm else ('O2',)
    probes = ['MixedProbe', 'BindingProbe', 'LargeEndpoints', 'Edges', 'CodecParity', 'CompositeStorage', 'LeafStorage', 'ErasedBoundary', 'PayloadBoundary']
    if args.arm or os.name != 'nt':
        probes += ['WeakEndpoints']
    for opt in optimizations:
        for name in probes:
            sources = [HERE / (name + '.cpp')]
            if name in ('MixedProbe', 'PayloadBoundary'):
                sources += [ADAPTER, ABI]
            if args.arm:
                for source in sources:
                    run(flags + ['-'+opt, '-c', source, '-o', output / f'{opt}-{source.stem}.o'],
                        f'compile-{opt}-{source.stem}')
            else:
                exe = output / (name + ('.exe' if os.name == 'nt' else ''))
                run(flags + ['-'+opt, *sources, '-o', exe], 'compile-'+name)
                result = run([exe], 'execute-'+name)
                if name == 'PayloadBoundary':
                    conditions = re.findall(r'^Encoded boundaries: (\d+) checks passed$', result, re.M)
                    corpus = re.findall(r'^Encoded corpus: (\d+) packets; (\d+) canonical; (\d+) unknown enum codes$', result, re.M)
                    if conditions != ['427986'] or corpus != [('10742', '4479', '2903')]:
                        raise RuntimeError('Incomplete or changed encoded boundary corpus')
                    counts['payload_conditions'] = int(conditions[0])
        # Signature constraints do not change with optimizer choices.
        if opt == 'O2':
            for case, diagnostic in DIAGNOSTICS.items():
                run([flag for flag in flags if flag != '-fstack-usage'] +
                    ['-'+opt, f'-DCASE={case}', '-fsyntax-only', HERE/'Negative.cpp'],
                    f'negative-{case}', diagnostic)

    if args.arm:
        tools = Path(shutil.which(args.cxx) or args.cxx).resolve().parent
        suffix = '.exe' if os.name == 'nt' else ''
        objcopy = tools / ('arm-none-eabi-objcopy'+suffix)
        objdump = tools / ('arm-none-eabi-objdump'+suffix)
        results = {}
        for opt in optimizations:
            obj = output / (opt+'-ArmCodegen.o')
            run(flags + ['-'+opt, '-c', HERE/'ArmCodegen.cpp', '-o', obj], 'codegen-'+opt)
            asm = run([objdump, '-drC', obj], 'assembly-'+opt)
            if re.search(r'telemetry::(?:Scalar|Getter|Setter)', asm):
                raise RuntimeError('Legacy value erasure leaked into native codegen')
            layout = output/(opt+'-layout.bin')
            run([objcopy, '--dump-section', f'.rodata.endpoint_layout={layout}', obj], 'layout-'+opt)
            results[opt] = {'layout': list(struct.unpack('<13I', layout.read_bytes()))}
            if opt != 'Og' and not args.null_checks:
                for operation in ('read', 'write', 'command', 'boolean', 'u16', 'f32', 'f64',
                                  'enumeration', 'array', 'state', 'config'):
                    code = []
                    for route in ('direct', 'local', 'handle', 'path'):
                        symbol = operation+'_'+route
                        path = output / (opt+'-'+symbol+'.bin')
                        run([objcopy, '--dump-section', f'.text.{symbol}={path}', obj], 'bytes-'+opt+'-'+symbol)
                        code.append(path.read_bytes())
                    if not code[0] or any(code[0] != other for other in code[1:]):
                        raise RuntimeError(f'{opt}: {operation} direct/local/handle/path code differs')
                    results[opt][operation+'_identical_bytes'] = len(code[0])
            usage = (output/(opt+'-LargeEndpoints.su')).read_text()
            frames = re.findall(r':static [^\n]*(?:FieldTable|CommandTable)[^\n]*::(?:readOne|writeOne|invokeOne)\([^\n]*\t(\d+)\tstatic', usage)
            if len(frames) != 3 or any(int(frame) > 192 for frame in frames):
                raise RuntimeError(f'{opt}: unexpected large-object frames {frames}')
            results[opt]['large_frames'] = list(map(int, frames))
            print(opt, results[opt], flush=True)
        (output/'arm-results.json').write_text(json.dumps(results, indent=2)+'\n')
        sections = run([objdump, '-h', output/'O2-MixedProbe.o'], 'table-storage')
        for name in ('mixedFields', 'localCommands', 'localServices', 'model'):
            if re.search(r'\.rodata\.[^\s]*'+name+r'E\b', sections) is None:
                raise RuntimeError(name+' did not remain in read-only storage')
        if re.search(r'\.(?:init_array|preinit_array|ctors)\b', sections):
            raise RuntimeError('Tables gained startup constructors')
    # A retained real adapter reference rejects a mismatched caller.
    link = ['-nostdlib', '-Wl,-e,main', '-Wl,--gc-sections', '-lc', '-lgcc'] if args.arm else []
    for case, symbol in enumerate(('readFieldEncoded', 'writeFieldEncoded', 'executeCommandEncoded',
                                   'readFieldEncoded', 'writeFieldEncoded'), 1):
        run(flags + ['-O2', f'-DCASE={case}', HERE/'AbiMismatch.cpp', ADAPTER, ABI,
            *link, '-o', output/f'mismatch-{case}'], f'abi-mismatch-{case}',
            r'undefined (?:reference|symbol).*'+symbol)
    (output/'summary.json').write_text(json.dumps(counts, indent=2)+'\n')
    print('Stage 08 checks passed:', counts, flush=True)


if __name__ == '__main__':
    main()
