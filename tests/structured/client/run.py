#!/usr/bin/env python3
"""Build a fake Device and verify the JS v3 client against native C++ bytes.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
Generated inputs, requests, results and logs stay in --build-dir.
"""
import argparse
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EXPECTED_JS_CHECKS = 8892  # +3: one tightening check per v3.1 group-tree ceiling


def client_report(text):
    """Accept the complete Check.mjs result, including its required count."""
    def unique(pairs):
        result = dict(pairs)
        if len(result) != len(pairs):
            raise ValueError('Duplicate client result fields')
        return result

    report = json.loads(text, object_pairs_hook=unique)
    if (not isinstance(report, dict) or set(report) != {'checks', 'failures', 'cppInterop'} or
            type(report['checks']) is not int or report['checks'] != EXPECTED_JS_CHECKS or
            type(report['failures']) is not int or report['failures'] != 0 or
            report['cppInterop'] is not True):
        raise ValueError('Wrong client result/count/status: ' + repr(report))
    return report


def report_controls():
    """A lost condition or malformed success report must fail the runner."""
    valid = {'checks': EXPECTED_JS_CHECKS, 'failures': 0, 'cppInterop': True}
    client_report(json.dumps(valid))
    cases = [json.dumps({key: value for key, value in valid.items() if key != missing})
             for missing in valid]
    for key, value in (('checks', EXPECTED_JS_CHECKS - 1), ('checks', True),
                       ('checks', float(EXPECTED_JS_CHECKS)), ('checks', str(EXPECTED_JS_CHECKS)),
                       ('failures', 1), ('failures', False), ('failures', 0.0),
                       ('cppInterop', False), ('cppInterop', 1), ('extra', 0)):
        cases.append(json.dumps({**valid, key: value}))
    cases += [f'{{"checks":{EXPECTED_JS_CHECKS},"checks":{EXPECTED_JS_CHECKS},'
              '"failures":0,"cppInterop":true}',
              f'[{EXPECTED_JS_CHECKS},0,true]', json.dumps(valid) + ' trailing']
    for case in cases:
        try:
            client_report(case)
        except ValueError:
            continue
        raise AssertionError('Accepted invalid client result: ' + case)
    return {'positive_reports': 1, 'rejected_mutations': len(cases),
            'scope': 'result parser controls; no interoperability checks generated'}


def browser_migration_controls():
    """The retired UI switch refuses before output or compiler/Node execution."""
    with tempfile.TemporaryDirectory(prefix='telemetry-browser-migration-') as temporary:
        output = Path(temporary) / 'must-not-be-created'
        result = subprocess.run(
            [os.sys.executable, '-B', str(Path(__file__).resolve()), '--browser',
             '--build-dir', str(output), '--cxx', '__telemetry_never_run_cxx__',
             '--node', '__telemetry_never_run_node__'],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        if (result.returncode != 2 or output.exists() or
                'Browser qualification moved to Telemetry UX:' not in result.stdout):
            raise AssertionError('Retired browser switch did not fail before output/tools: ' +
                                 repr((result.returncode, output.exists(), result.stdout)))
    return {'rejected_browser_requests': 1, 'diagnostic_exit': 2,
            'no_output_created': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--node', default=os.environ.get('NODE', 'node'))
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--null-checks', action='store_true')
    parser.add_argument('--browser', action='store_true',
                        help='Retired switch: browser qualification moved to Telemetry UX')
    args = parser.parse_args()
    if args.browser:
        parser.error('Browser qualification moved to Telemetry UX: https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/tests/structured_client/README.md')
    controls = report_controls()
    migration_controls = browser_migration_controls()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    compiler = Path(shutil.which(args.cxx) or args.cxx).resolve()
    # Windows MinGW's runtime DLLs must match the compiler, not a different kit.
    env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
    commands = 0

    def run(command, label):
        nonlocal commands
        result = subprocess.run(list(map(str, command)), cwd=ROOT, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=180)
        (out / (label + '.log')).write_text(result.stdout, encoding='utf-8')
        if result.returncode:
            raise RuntimeError(f'{label}: {result.stdout}')
        commands += 1
        return result.stdout

    flags = [args.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    if args.null_checks:
        flags += ['-fno-delete-null-pointer-checks']
    flags += ['-I' + str(ROOT / name) for name in ('lib', 'lib/boost_pfr/include', 'lib/magic_enum')]
    sources = [ROOT / 'lib/telemetry/abi/StructuredAbi.cpp',
               ROOT / 'lib/telemetry/model/Adapter.cpp',
               ROOT / 'lib/integration/telemetry/v3/detail/Values.cpp']
    suffix = '.exe' if os.name == 'nt' else ''
    for source, name in (('Device.cpp', 'client-device'), ('Resources.cpp', 'client-resources')):
        program = out / (name + suffix)
        run(flags + [HERE / source, *sources, '-o', program], 'build-' + name)
        run([program, out], 'generate-' + name)
    oracle = runpy.run_path(str(HERE.parent / 'descriptor/parser.py'))['parse']
    for name in ('descriptor.bin', 'resources-descriptor.bin'):
        oracle((out / name).read_bytes())
    report = client_report(run([args.node, HERE / 'Check.mjs', out,
                                out / ('client-device' + suffix)], 'js-check'))
    report['successful_commands'] = commands
    report['independent_descriptors'] = 2
    report['result_controls'] = controls
    report['browser_migration_controls'] = migration_controls
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Structured client checks passed:', report, flush=True)


if __name__ == '__main__':
    main()
