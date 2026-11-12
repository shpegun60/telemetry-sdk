#!/usr/bin/env python3
"""Installed device-files consumers: FileReader against the real Resource
protocol handler without a network, and the files of a live ConnectionSession
over localhost TCP. No board. MIT."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--qt-prefix', default='')
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    out = args.build_dir.resolve()
    if out == ROOT or ROOT in out.parents or out in ROOT.parents:
        parser.error('Use a dedicated external output directory')
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        parser.error('Use a fresh empty output directory; nothing is deleted')
    out.mkdir(parents=True, exist_ok=True)
    compiler = Path(shutil.which(args.cxx) or args.cxx).absolute()
    cmake = Path(shutil.which(args.cmake) or args.cmake).absolute()
    ninja = Path(shutil.which(args.ninja) or args.ninja).absolute()
    for tool in (compiler, cmake, ninja):
        if not tool.is_file():
            parser.error('Missing tool: ' + str(tool))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
               ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    env['PATH'] = os.pathsep.join([str(compiler.parent),
        *([str(Path(args.qt_prefix) / 'bin')] if args.qt_prefix else []), env.get('PATH', '')])
    report = dict(passed=False, hardware=False, sockets='ephemeral localhost only', commands=[], executions={},
                  tools={p.name: digest(p) for p in (compiler, cmake, ninja)})

    def run(name, command, timeout=600):
        started = time.monotonic()
        result = subprocess.run(list(map(str, command)), env=env, cwd=out, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        log = out / (name + '.log')
        log.write_text(result.stdout, encoding='utf-8')
        report['commands'].append(dict(name=name, command=list(map(str, command)), returncode=result.returncode,
                                       seconds=time.monotonic() - started, log=log.name, sha256=digest(log)))
        if result.returncode:
            raise RuntimeError(name + ' failed:\n' + result.stdout[-14000:])
        print(name + ': passed', flush=True)
        return result.stdout

    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_COMPILER=' + str(compiler),
              '-DCMAKE_MAKE_PROGRAM=' + str(ninja), '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
    if args.qt_prefix:
        common += ['-DCMAKE_PREFIX_PATH=' + args.qt_prefix]
    if args.sanitize:
        common += ['-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer',
                   '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined']
    try:
        prefix = out / 'prefix'
        run('package-configure', [cmake, '-S', ROOT, '-B', out / 'package', *common,
            '-DTELEMETRY_COMPONENTS=files_client;resource_protocol;qt_connection;tcp_server;tcp_encoded_profile;resource_adapter;values_reader',
            '-DCMAKE_DISABLE_FIND_PACKAGE_Qt6SerialPort=ON', '-DCMAKE_INSTALL_PREFIX=' + str(prefix)])
        run('package-install', [cmake, '--build', out / 'package', '--target', 'install', '-j2'])
        relocated = out / 'relocated-prefix'
        prefix.rename(relocated)
        consumer = out / 'consumer'
        consumer.mkdir()
        for name in ('Reader.cpp', 'Values.cpp', 'Session.cpp', 'FilesOnly.cpp', 'CMakeLists.txt'):
            shutil.copy2(HERE / name, consumer / name)
        search = str(relocated) + (';' + args.qt_prefix if args.qt_prefix else '')
        run('consumer-configure', [cmake, '-S', consumer, '-B', out / 'consumer-build', *common,
                                   '-DCMAKE_PREFIX_PATH=' + search])
        run('consumer-build', [cmake, '--build', out / 'consumer-build', '-j2'])
        commands = (out / 'consumer-build/compile_commands.json').read_text(encoding='utf-8').replace('\\', '/')
        if str(ROOT / 'lib').replace('\\', '/') in commands:
            raise RuntimeError('Installed consumer leaked SDK source include paths')
        for target, pattern, floor in (('reader', r'Device files reader: (\d+) checks, 0 failures', 80),
                                       ('values', r'Values reader: (\d+) checks, 0 failures', 50),
                                       ('session', r'Device files session: (\d+) checks, 0 failures', 15),
                                       ('files_only', r'Files-only connection: (\d+) checks, 0 failures', 30)):
            executable = out / 'consumer-build' / (target + ('.exe' if os.name == 'nt' else ''))
            found = re.findall(pattern, run(target + '-execute', [executable], 120))
            if len(found) != 1 or int(found[0]) < floor:
                raise RuntimeError('Missing maintained nonzero check count: ' + target)
            report['executions'][target] = int(found[0])
        report['passed'] = True
    except BaseException as error:
        report['error'] = str(error)
        raise
    finally:
        (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
