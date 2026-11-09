#!/usr/bin/env python3
"""Build the optional serial application and qualify it on host PTYs without hardware (MIT)."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources(cobs, ux, sdk):
    result = {}
    # The GUI consumes the installed SDK. Only the maintained board peer and
    # its private serial profile remain SDK source fixtures in this gate.
    for base, label, directories in ((ROOT, 'sdk-fixture/', (HERE,)),
            (ux, 'ux/', (ux / 'examples/inspector',)),
            (cobs, 'COBS_ROOT/', (cobs / 'src',)),
            (sdk, 'installed-sdk/', (sdk,))):
        for directory in directories:
            for parent, names, files in os.walk(directory):
                names[:] = [name for name in names if name not in ('.git', '__pycache__', 'build')
                            and (not label or name != 'tests')]
                for name in files:
                    path = Path(parent) / name
                    if label == 'installed-sdk/' or path.suffix in ('.h', '.hpp', '.cpp', '.py', '.txt', '.cmake', '.qml'):
                        result[label + path.relative_to(base).as_posix()] = digest(path)
    for path in (ROOT / 'examples/nucleo_h7s3l8/Protocol.hpp',
                 ROOT / 'lib/integration/serial/Profile.hpp'):
        result['sdk-fixture/' + path.relative_to(ROOT).as_posix()] = digest(path)
    return dict(sorted(result.items()))


def sdk_identity(prefix):
    configs = sorted(prefix.rglob('TelemetryConfig.cmake'))
    metadata_files = sorted(prefix.rglob('SDKMetadata.json'))
    if len(configs) != 1 or len(metadata_files) != 1:
        raise ValueError('--sdk-prefix must contain one installed Telemetry package and SDKMetadata.json')
    metadata = metadata_files[0]
    value = json.loads(metadata.read_text(encoding='utf-8'))
    if (not isinstance(value, dict) or value.get('sdkVersion') != '0.1.0' or
            value.get('descriptorWireVersion') != [3, 0] or
            any(type(n) is not int for n in value['descriptorWireVersion']) or
            type(value.get('encodedWireVersion')) is not int or value['encodedWireVersion'] != 1 or
            not isinstance(value.get('sourceRevision'), str) or
            (value['sourceRevision'] and not re.fullmatch('[0-9a-fA-F]{40}', value['sourceRevision']))):
        raise ValueError('Installed SDK metadata is incompatible with this serial gate')
    return configs[0].parent, dict(prefix=str(prefix), metadata=value,
        metadata_path=metadata.relative_to(prefix).as_posix(), metadata_sha256=digest(metadata))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--qt-prefix', type=Path, required=True)
    parser.add_argument('--cobs-root', type=Path, required=True)
    parser.add_argument('--ux-root', type=Path, required=True,
                        help='External telemetry-ux checkout containing examples/inspector')
    parser.add_argument('--sdk-prefix', type=Path, required=True,
                        help='Installed Telemetry SDK; the GUI never builds SDK sources')
    parser.add_argument('--serial-prefix', type=Path)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--jobs', default='2')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    output, cobs = args.build_dir.resolve(), args.cobs_root.resolve()
    ux, sdk = args.ux_root.resolve(), args.sdk_prefix.resolve()
    if not (ux / 'examples/inspector/CMakeLists.txt').is_file() or ux == ROOT:
        parser.error('--ux-root must contain the external UX Inspector application')
    if not sdk.is_dir() or sdk == ROOT or ROOT in sdk.parents or sdk == ux or ux in sdk.parents:
        parser.error('--sdk-prefix must be an installed SDK outside both source repositories')
    if any(output == path or path in output.parents or output in path.parents for path in (ROOT, ux, sdk, cobs)):
        parser.error('--build-dir must be a dedicated external directory')
    if output.exists() and any(output.iterdir()):
        parser.error('--build-dir must be fresh and empty')
    if not (cobs / 'src/cobs/Cobs.h').is_file():
        parser.error('--cobs-root must contain the actual external COBS sources')
    if args.sanitize and os.name == 'nt':
        parser.error('--sanitize requires a Linux sanitizer toolchain')
    output.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, hardware=False, sanitize=args.sanitize, commands=[],
                  started_utc=datetime.now(timezone.utc).isoformat(),
                  source_sha256_before=sources(cobs, ux, sdk), external_cobs=str(cobs),
                  ux_root=str(ux), sdk_source_fixture_root=str(ROOT), sdk_source_build=False)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', QT_QPA_PLATFORM='offscreen',
               QT_QUICK_BACKEND='software', QT_FORCE_STDERR_LOGGING='1')
    if args.sanitize:
        env['ASAN_OPTIONS'] = 'detect_leaks=0:halt_on_error=1'
        env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        report['sanitizers'], report['leak_checking'] = ['address', 'undefined'], False

    def run(name, command):
        command = list(map(str, command))
        started = time.monotonic()
        try:
            result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
            text, code = result.stdout, result.returncode
        except subprocess.TimeoutExpired as error:
            text = error.stdout or ''
            if isinstance(text, bytes):
                text = text.decode(errors='replace')
            code = None
        log = output / (name + '.log')
        log.write_text(text, encoding='utf-8')
        report['commands'].append(dict(command=command, name=name, exit=code,
            seconds=time.monotonic()-started, log=log.name, sha256=digest(log)))
        print(name + ': exit ' + str(code), flush=True)
        if code != 0:
            raise RuntimeError(name + ' failed\n' + text[-16000:])
        return text

    try:
        sdk_config, report['installed_sdk'] = sdk_identity(sdk)
        tools = {name: Path(shutil.which(getattr(args, name)) or getattr(args, name)).absolute()
                 for name in ('cxx', 'cmake', 'ninja')}
        for path in tools.values():
            if not path.is_file():
                raise ValueError('Missing required tool: ' + str(path))
        qt = args.qt_prefix.resolve()
        prefixes = [sdk, qt] + ([args.serial_prefix.resolve()] if args.serial_prefix else [])
        env['PATH'] = os.pathsep.join([str(tools['cxx'].parent)] +
            [str(path / 'bin') for path in prefixes] + [env.get('PATH', '')])
        report['tool_sha256'] = {str(path): digest(path) for path in tools.values()}
        report['source_head'] = run('source-head', ['git', 'rev-parse', 'HEAD']).strip()
        report['ux_head'] = run('ux-head', ['git', '-C', ux, 'rev-parse', 'HEAD']).strip()
        report['cobs_head'] = run('cobs-head', ['git', '-C', cobs, 'rev-parse', 'HEAD']).strip()
        build = output / 'build'
        flags = '-Wall -Wextra -Werror'
        configure = [tools['cmake'], '-S', HERE, '-B', build, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_COMPILER=' + str(tools['cxx']),
            '-DCMAKE_MAKE_PROGRAM=' + str(tools['ninja']),
            '-DCMAKE_PREFIX_PATH=' + ';'.join(map(str, prefixes)), '-DCOBS_ROOT=' + str(cobs),
            '-DTelemetry_DIR=' + str(sdk_config), '-DTELEMETRY_UX_ROOT=' + str(ux),
            '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF', '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
        if args.serial_prefix:
            configure += ['-DQt6SerialPort_DIR=' +
                          str(args.serial_prefix.resolve() / 'lib/cmake/Qt6SerialPort')]
        if args.sanitize:
            flags += ' -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
            configure += ['-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG',
                          '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined']
        run('configure', configure + ['-DCMAKE_CXX_FLAGS=' + flags])
        run('build', [tools['cmake'], '--build', build, '-j', args.jobs])
        exe = build / ('telemetry_inspector_serial_check.exe' if os.name == 'nt'
                       else 'telemetry_inspector_serial_check')
        gui = build / 'example' / ('telemetry-inspector-qml' + ('.exe' if os.name == 'nt' else ''))
        receipts = output / 'device-check'
        runtime = run('runtime', [exe, gui, receipts])
        counted = re.search(r'Inspector Serial checks=(\d+) reads=(\d+) admissions=(\d+) pty=(\d) Qt=(\S+)', runtime)
        if not counted:
            raise RuntimeError('Serial test executable did not publish its result')
        checks, reads, admissions, pty = map(int, counted.groups()[:4])
        if os.name != 'nt' and (not pty or checks < 1367 or reads < 9 or admissions < 6):
            raise RuntimeError('The maintained real QSerialPort/PTY scenarios did not all execute')
        if os.name == 'nt' and (pty or checks < 12):
            raise RuntimeError('Native build-only serial checks did not execute')
        report['executions'] = dict(checks=checks, reads=reads, admissions=admissions, pty=bool(pty))
        report['qt_version'] = counted[5]
        watch = build / ('telemetry_inspector_serial_watchlist_check' + ('.exe' if os.name == 'nt' else ''))
        watched = run('watchlist', [watch])
        marker = re.search(r'Inspector Serial Watchlist checks=(\d+) pty=1 batches=4 sessions=2 malformed=1 Qt=(\S+)', watched)
        if os.name != 'nt' and (not marker or int(marker[1]) < 100 or marker[2] != counted[5]):
            raise RuntimeError('Actual Watchlist UART discovery, batch and recovery checks did not execute')
        report['executions']['watchlist_pty'] = bool(marker)
        report['binary_sha256'] = {str(path.relative_to(output)): digest(path) for path in (exe, gui, watch)}
        if os.name != 'nt':
            phase = re.search(r'Inspector Serial pre-handoff timeout: busy=(\d+) connected=(\d+) '
                              r'recovery=(\d+) fieldReads=(\d+)', runtime)
            if not phase or phase.groups() != ('0', '1', '0', '0'):
                raise RuntimeError('The controlled pre-handoff timeout case did not execute')
            paced = re.search(r'Inspector Serial low-baud: bootstrap=1 field=1 explicit=2 '
                              r'bootstrapMs=(\d+) fieldMs=(\d+) explicitMs=(\d+)', runtime)
            if not paced:
                raise RuntimeError('The paced low-baud default/explicit deadline cases did not execute')
            bootstrap_ms, field_ms, explicit_ms = map(int, paced.groups())
            if bootstrap_ms <= 5000 or field_ms <= 3000 or explicit_ms >= 1500:
                raise RuntimeError('Low-baud timing did not distinguish automatic and exact deadlines')
            report['low_baud'] = dict(baud=1200, bootstrap_ms=bootstrap_ms, field_ms=field_ms,
                                     explicit_override_ms=80, explicit_cases=2,
                                     explicit_field_phase='before_handoff',
                                     explicit_completion_ms=explicit_ms)
            report['device_check'] = []
            for index in range(4):
                path = receipts / ('device-check-' + str(index) + '.json')
                observed = json.loads(path.read_text(encoding='utf-8'))
                if (observed.get('passed') is not (index == 0) or
                        observed.get('voltage') is not (index == 0) or
                        not observed.get('probe') or not observed.get('exactCounter')):
                    raise RuntimeError('Headless checker acceptance/negative case did not execute')
                report['device_check'].append(dict(file=str(path.relative_to(output)),
                                                  sha256=digest(path), passed=observed['passed']))
        report['serial_runtime_sha256'] = {str(path): digest(path) for prefix in prefixes
            for path in (prefix / 'bin').glob('Qt6SerialPort.dll')}
        report['compile_commands_sha256'] = digest(build / 'compile_commands.json')
        report['source_sha256_after'] = sources(cobs, ux, sdk)
        if report['source_sha256_before'] != report['source_sha256_after']:
            raise RuntimeError('Source inputs changed during qualification')
        report['passed'] = True
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
        print(report['error'], flush=True)
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    (output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
