#!/usr/bin/env python3
"""Validate stable registry roots against a separate canonical wire baseline.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
Build inputs may come from a pristine external source archive; generated images,
logs and canonical byte files must stay outside either checkout. No device access.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = Path(__file__).resolve().parent
ARTIFACTS = ('descriptor.bin', 'values.bin', 'type-ids.bin')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--source-root', type=Path, default=ROOT,
                        help='Library source checkout/archive; fixture stays identical')
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--before-dir', type=Path,
                        help='Compare canonical outputs with a completed baseline run')
    parser.add_argument('--expect-new', action='store_true',
                        help='Require unique RegistryRootTypes local/catalog aliases')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--optimization', choices=('O2', 'Os', 'Og'), default='O2')
    args = parser.parse_args()
    source = args.source_root.resolve()
    out = args.build_dir.resolve()
    if any(out == root or root in out.parents for root in (ROOT, source)):
        parser.error('Generated files must remain outside the source checkout/archive')
    if out.exists() and any(out.iterdir()):
        parser.error('Use a fresh output directory to preserve previous evidence')
    required = ('lib/telemetry/Telemetry.hpp', 'lib/telemetry/model/Adapter.cpp')
    if not all((source / name).is_file() for name in required):
        parser.error('Source root must contain the telemetry library')
    layouts = (
        ('modular', 'lib/integration/telemetry/v3/detail/Values.cpp',
         'lib/integration/telemetry/v3/ValuesFile.hpp',
         'lib/telemetry/descriptor/Descriptor.hpp'),
        ('historical', 'lib/resource/telemetry/v3/detail/Values.cpp',
         'lib/resource/telemetry/v3/ValuesFile.hpp',
         'lib/resource/telemetry/v3/Descriptor.hpp'),
    )
    layout = next((entry for entry in layouts
                   if all((source / name).is_file() for name in entry[1:])), None)
    if layout is None:
        parser.error('Source root lacks a complete modular or historical descriptor/values layout')
    layout_name, values_source, _, _ = layout
    before = args.before_dir.resolve() if args.before_dir else None
    if before:
        if not all((before / name).is_file() for name in (*ARTIFACTS, 'summary.json')):
            parser.error('Before directory lacks completed canonical evidence')
        previous = json.loads((before / 'summary.json').read_text(encoding='utf-8'))
        if not previous.get('completed') or previous.get('expect_new'):
            parser.error('Before directory must be a completed old-alias baseline run')
    out.mkdir(parents=True, exist_ok=True)
    compiler = Path(shutil.which(args.cxx) or args.cxx).absolute()
    environment = os.environ.copy()
    environment['PATH'] = str(compiler.parent) + os.pathsep + environment.get('PATH', '')
    environment.setdefault('ASAN_OPTIONS', 'detect_leaks=1:detect_stack_use_after_return=1')
    environment.setdefault('UBSAN_OPTIONS', 'halt_on_error=1')
    inputs = [*sorted((source / 'lib').rglob('*.hpp')),
              *sorted((source / 'lib').rglob('*.h')),
              *sorted((source / 'lib').rglob('*.cpp'))]
    library_hashes = {str(path.relative_to(source)): digest(path) for path in inputs}
    fixture_hashes = {path.name: digest(path) for path in (FIXTURE / 'Fixture.hpp', FIXTURE / 'Check.cpp')}
    report = dict(completed=False, source_root=str(source), expect_new=args.expect_new,
                  source_layout=layout_name, values_source=values_source,
                  sanitize=args.sanitize, optimization=args.optimization,
                  library_hashes=library_hashes, fixture_hashes=fixture_hashes,
                  runner_sha256=digest(Path(__file__)), compiler_sha256=digest(compiler),
                  scope='Host correctness and exact canonical parity; no MCU cycles')

    def write_report():
        (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    def run(command, label):
        command = list(map(str, command))
        start = time.perf_counter()
        process = subprocess.Popen(command, cwd=out, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   encoding='utf-8', errors='replace',
                                   start_new_session=os.name == 'posix')
        try:
            output = process.communicate(timeout=180)[0]
            code = process.returncode
        except subprocess.TimeoutExpired:
            if os.name == 'posix':
                os.killpg(process.pid, signal.SIGKILL)
            else:
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                process.kill()
            output = process.communicate()[0]
            code = 124
        (out / (label + '.log')).write_text(
            f'COMMAND {command!r}\nEXIT {code}\nSECONDS {time.perf_counter() - start:.6f}\n{output}',
            encoding='utf-8')
        if code:
            report['failure'] = dict(step=label, code=code)
            write_report()
            raise RuntimeError(f'{label} failed: {output}')
        return output

    write_report()
    report['compiler_version'] = run([compiler, '--version'], 'version').splitlines()[0]
    flags = ['-std=c++20', '-' + args.optimization, '-Wall', '-Wextra', '-Werror',
             '-pedantic-errors', '-fdiagnostics-color=never',
             '-DTELEMETRY_TEST_REGISTRY_ROOTS=' + str(int(args.expect_new)),
             '-DTELEMETRY_TEST_MODULAR_LAYOUT=' + str(int(layout_name == 'modular')),
             # The canonical descriptor must equal the v3.0 baseline's bytes, so this
             # program keeps type names off; a pre-v3.1 source root ignores the switch.
             '-DTELEMETRY_TYPE_NAMES=0',
             '-I' + str(source / 'lib'), '-I' + str(source / 'lib/boost_pfr/include'),
             '-I' + str(source / 'lib/magic_enum'), '-I' + str(source / 'lib/delegate')]
    if args.sanitize:
        flags += ['-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                  '-fno-sanitize-recover=all']
    report['flags'] = flags
    image = out / ('root-types.exe' if os.name == 'nt' else 'root-types')
    run([compiler, *flags, FIXTURE / 'Check.cpp', source / 'lib/telemetry/model/Adapter.cpp',
         source / values_source, '-o', image], 'compile')
    result = json.loads(run([image], 'execute'))
    expected = dict(checks=4995, value_read_cases=680, fields=8, commands=6, services=7,
                    type_count=19, values_bytes=67)
    if any(result.get(key) != value for key, value in expected.items()):
        raise RuntimeError('Fixture population or actual counted coverage changed')
    report['result'] = result
    report['image_sha256'] = digest(image)
    report['artifacts'] = {name: dict(bytes=(out / name).stat().st_size, sha256=digest(out / name))
                           for name in ARTIFACTS}
    if before:
        if previous['fixture_hashes'] != fixture_hashes:
            raise RuntimeError('Before/after fixture sources differ')
        if previous['result'] != result:
            raise RuntimeError('Before/after type IDs, fingerprint or correctness counts differ')
        for name in ARTIFACTS:
            if (before / name).read_bytes() != (out / name).read_bytes():
                raise RuntimeError('Canonical before/after bytes differ: ' + name)
        report['before_directory'] = str(before)
        report['canonical_parity'] = True
    if library_hashes != {str(path.relative_to(source)): digest(path) for path in inputs}:
        raise RuntimeError('Library inputs changed while the fixture was building')
    if fixture_hashes != {path.name: digest(path) for path in (FIXTURE / 'Fixture.hpp', FIXTURE / 'Check.cpp')}:
        raise RuntimeError('Fixture inputs changed during validation')
    report['completed'] = True
    write_report()
    print(json.dumps(report['result'], sort_keys=True))
    print('root-types: pass' + ('; exact baseline parity' if before else ''))


if __name__ == '__main__':
    main()
