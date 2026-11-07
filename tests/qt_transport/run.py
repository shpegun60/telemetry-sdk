#!/usr/bin/env python3
"""Installed Qt transport boundary, relocation and dependency selection. MIT."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--cobs-root', type=Path, required=True)
    parser.add_argument('--qt-prefix', default='')
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', default='ninja')
    args = parser.parse_args()
    out = args.build_dir.resolve()
    cobs = args.cobs_root.resolve()
    for source in (ROOT, cobs):
        if out == source or source in out.parents or out in source.parents:
            parser.error('Use a fresh external build directory')
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        parser.error('Output must be fresh and empty; nothing is deleted')
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    if args.qt_prefix:
        env['PATH'] = str(Path(args.qt_prefix)/'bin') + os.pathsep + env.get('PATH', '')
    files = [ROOT/name for name in ('CMakeLists.txt', 'VERSION', 'LICENSE')]
    for name in ('lib', 'cmake', 'tests/qt_transport', 'examples/read_many_uart'):
        files += [p for p in (ROOT/name).rglob('*') if p.is_file() and '__pycache__' not in p.parts]
    cobs_files = [cobs/'LICENSE']
    for name in ('cobs', 'wire', 'crc'):
        for directory, subdirs, names in os.walk(cobs/'src'/name):
            subdirs[:] = [d for d in subdirs if d not in ('tests', 'build', '__pycache__')]
            cobs_files += [Path(directory)/n for n in names
                           if Path(n).suffix in ('.h', '.hpp', '.cpp')]
    cobs_files.append(cobs/'src/adapters/qt/SerialAdapter.h')

    def inventory():
        return {**{'sdk/'+p.relative_to(ROOT).as_posix(): digest(p) for p in sorted(set(files))},
                **{'cobs/'+p.relative_to(cobs).as_posix(): digest(p) for p in sorted(set(cobs_files))}}

    report = dict(passed=False, hardware=False, started_utc=datetime.now(timezone.utc).isoformat(),
                  source_sha256_before=inventory(), checks=0, commands=[])

    def save():
        (out/'summary.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf8')

    def check(condition, label):
        report['checks'] += 1
        if not condition:
            raise RuntimeError(label)

    def run(name, command, success=True):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=out, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=600)
        log = out/(name+'.log')
        log.write_text(result.stdout, encoding='utf8')
        report['commands'].append(dict(name=name, command=command, returncode=result.returncode,
                                       log=log.name, sha256=digest(log)))
        save()
        check((result.returncode == 0) == success, name+':\n'+result.stdout[-12000:])
        print(name+': exit='+str(result.returncode), flush=True)
        return result.stdout

    compiler = shutil.which(args.cxx) or args.cxx
    ninja = shutil.which(args.ninja) or args.ninja
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_CXX_COMPILER='+compiler,
              '-DCMAKE_MAKE_PROGRAM='+ninja, '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
    if args.qt_prefix:
        common += ['-DCMAKE_PREFIX_PATH='+args.qt_prefix]
    suffix = '.exe' if os.name == 'nt' else ''
    try:
        source = out/'prepared-sdk'
        source.mkdir()
        for name in ('CMakeLists.txt', 'VERSION', 'LICENSE'):
            shutil.copy2(ROOT/name, source/name)
        for name in ('lib', 'cmake'):
            shutil.copytree(ROOT/name, source/name, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        private_cobs = out/'prepared-cobs'
        for path in cobs_files:
            target = private_cobs/path.relative_to(cobs)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        example = out/'cli-source'
        shutil.copytree(ROOT/'examples/read_many_uart', example)
        run('default-without-qt', [args.cmake, '-S', source, '-B', out/'default-noqt', *common,
                                 '-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON'])
        for name, components, extras in (
                ('base', 'qt_transport', ['-DCMAKE_DISABLE_FIND_PACKAGE_Qt6SerialPort=ON']),
                ('connection', 'qt_connection', ['-DCMAKE_DISABLE_FIND_PACKAGE_Qt6SerialPort=ON']),
                ('serial', 'qt_serial_transport;read_many_client;encoded_backend',
                 ['-DCOBS_ROOT='+str(private_cobs)])):
            run(name+'-configure', [args.cmake, '-S', source, '-B', out/(name+'-build'), *common,
                '-DTELEMETRY_COMPONENTS='+components, '-DCMAKE_INSTALL_PREFIX='+str(out/(name+'-prefix')),
                *extras])
            run(name+'-install', [args.cmake, '--build', out/(name+'-build'), '--target', 'install', '-j2'])
            (out/(name+'-prefix')).rename(out/(name+'-relocated'))
        refusal = run('missing-cobs', [args.cmake, '-S', source, '-B', out/'missing-cobs', *common,
                      '-DTELEMETRY_COMPONENTS=qt_serial_transport', '-DCOBS_ROOT='], success=False)
        check('COBS_ROOT' in refusal, 'Missing COBS refused for the intended reason')
        for prefix in (out/'base-relocated', out/'serial-relocated', out/'connection-relocated'):
            for path in prefix.rglob('*.cmake'):
                text = path.read_text().replace('\\', '/')
                check(str(private_cobs).replace('\\', '/') not in text, 'COBS build path not exported')
            for path in (prefix/'include').rglob('*.hpp'):
                check(not re.search(r'#\s*include\s*[<"](?:cobs|crc)/', path.read_text()),
                      'Installed public headers have no COBS/CRC includes')
        source.rename(out/'unavailable-sdk')
        private_cobs.rename(out/'unavailable-cobs')

        def consumer(name, prefix, mode, options=(), serial=False, optional=False, connection=False):
            name += '-consumer'
            directory = out/(name+'-source')
            directory.mkdir()
            if mode == 'base':
                shutil.copy2(HERE/'Consumer.cpp', directory/'Consumer.cpp')
                body = 'set(CMAKE_AUTOMOC ON)\nadd_executable(consumer Consumer.cpp)\n'
                target = 'qt_connection' if connection else ('qt_serial_transport' if serial else 'qt_transport')
                body += 'target_link_libraries(consumer PRIVATE Telemetry::'+target+')\n'
                if connection:
                    body += 'target_compile_definitions(consumer PRIVATE TEST_CONNECTION)\n'
                if serial:
                    body += 'target_compile_definitions(consumer PRIVATE TEST_SERIAL)\n'
            else:
                (directory/'Consumer.cpp').write_text('#include <client/ClientResult.hpp>\nint main() {}\n')
                body = 'add_executable(consumer Consumer.cpp)\ntarget_link_libraries(consumer PRIVATE Telemetry::client)\n'
            request = mode if mode != 'none' else ''
            if mode == 'base':
                request = 'qt_connection' if connection else ('qt_serial_transport' if serial else 'qt_transport')
            find = ('COMPONENTS '+request) if request else ''
            if optional:
                find += ' OPTIONAL_COMPONENTS qt_serial_transport'
            assertions = ''
            if optional:
                assertions = 'if(Telemetry_qt_serial_transport_FOUND)\nmessage(FATAL_ERROR "Unavailable optional serial reported found")\nendif()\n'
            (directory/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.21)\n'
                'project(InstalledConsumer LANGUAGES CXX)\n'
                'find_package(Telemetry 0.1.0 EXACT CONFIG REQUIRED '+find+')\n'+assertions+body)
            build = out/(name+'-build')
            search = str(prefix)+(';' + args.qt_prefix if args.qt_prefix else '')
            run(name+'-configure', [args.cmake, '-S', directory, '-B', build, *common,
                '-DCMAKE_PREFIX_PATH='+search, *options])
            run(name+'-build', [args.cmake, '--build', build, '-j2'])
            output = run(name+'-run', [build/('consumer'+suffix)])
            commands = (build/'compile_commands.json').read_text().replace('\\', '/')
            deps = run(name+'-dependencies', [ninja, '-C', build, '-t', 'deps']).replace('\\', '/')
            for hidden in (source, private_cobs):
                check(str(hidden).replace('\\', '/') not in commands+deps,
                      'Installed consumer does not reach hidden source')
            if mode == 'base':
                match = re.search(r'Installed Qt transport checks=(\d+)', output)
                check(match and int(match[1]) >= (19 if serial else 7), 'Actual QObject runtime checks')

        mixed = out/'serial-relocated'
        noqt = ['-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON']
        noserial = ['-DCMAKE_DISABLE_FIND_PACKAGE_Qt6SerialPort=ON']
        consumer('client-noqt', mixed, 'client', noqt)
        consumer('discovery-noqt', mixed, 'none', noqt)
        consumer('optional-noqt', mixed, 'client', noqt, optional=True)
        consumer('base-optional-serial', mixed, 'base', noserial, optional=True)
        consumer('base-only', out/'base-relocated', 'base', noserial)
        consumer('serial', mixed, 'base', serial=True)
        consumer('connection', out/'connection-relocated', 'base', connection=True)
        consumer('connection-client-noqt', out/'connection-relocated', 'client', noqt)
        build = out/'cli-build'
        search = str(mixed)+(';' + args.qt_prefix if args.qt_prefix else '')
        run('actual-cli-configure', [args.cmake, '-S', example, '-B', build, *common,
            '-DCMAKE_PREFIX_PATH='+search, '-DCOBS_ROOT='+str(out/'deliberately-missing-cobs')])
        run('actual-cli-build', [args.cmake, '--build', build, '-j2'])
        report_path = out/'invalid-cli-arguments.json'
        run('actual-cli-invalid-arguments', [build/('telemetry-read-many-uart'+suffix),
            '--report', report_path], success=False)
        invalid = json.loads(report_path.read_text())
        check(invalid['passed'] is False and not invalid['port'] and 'Usage:' in invalid['error'],
              'CLI rejects missing port before constructing an exchange')
        report['source_sha256_after'] = inventory()
        check(report['source_sha256_before'] == report['source_sha256_after'], 'Source inputs changed')
        report['passed'] = True
        print('Qt installed transport checks='+str(report['checks'])+' failures=0', flush=True)
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()


if __name__ == '__main__':
    main()
