#!/usr/bin/env python3
"""Qualify native TCP stream framing without sockets or hardware (MIT)."""
import argparse
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


def tool(name):
    path = Path(shutil.which(name) or name).absolute()
    if not path.is_file():
        raise ValueError('Tool not found: ' + str(path))
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--arm-cxx')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', default='ninja')
    args = parser.parse_args()
    out = args.build_dir.resolve()
    if out == ROOT or ROOT in out.parents or out in ROOT.parents:
        parser.error('Use a dedicated external output directory')
    if out.exists() and any(out.iterdir()):
        parser.error('Use a fresh empty output directory')
    out.mkdir(parents=True, exist_ok=True)
    compiler = tool(args.cxx)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', PYTHONDONTWRITEBYTECODE='1')
    report = dict(passed=False, hardware=False, sockets=False, sanitized=args.sanitize,
                  commands=[], executions={}, compiler_sha256=digest(compiler),
                  source_head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip())
    inputs = [HERE / 'main.cpp', HERE / 'run.py', HERE / 'README.md', ROOT / 'cmake/Modules.cmake']
    inputs += [path for path in (ROOT / 'lib').rglob('*') if path.is_file() and path.suffix in ('.hpp', '.h', '.cpp')]

    def snapshot():
        return {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(inputs)}

    def run(label, command, expected=0):
        started = time.monotonic()
        command = list(map(str, command))
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=600)
        log = out / (label + '.log')
        log.write_text(result.stdout, encoding='utf-8')
        report['commands'].append(dict(name=label, command=command, exit=result.returncode,
                                      seconds=time.monotonic() - started, log=log.name,
                                      sha256=digest(log)))
        if result.returncode != expected:
            raise RuntimeError(label + ' failed\n' + result.stdout)
        print(label + ': passed', flush=True)
        return result.stdout

    def count(label, text):
        found = re.search(r'Native TCP stream: (\d+) checks, 0 failures', text)
        if not found or int(found[1]) < 1000:
            raise RuntimeError(label + ': check floor not reached')
        report['executions'][label] = int(found[1])

    base = [compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic-errors',
            '-Ilib', '-Ilib/boost_pfr/include', '-Ilib/magic_enum']
    profile = (['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-fno-omit-frame-pointer'] if args.sanitize else ['-O2'])
    implementation = ['lib/telemetry/model/Adapter.cpp', 'lib/telemetry/abi/StructuredAbi.cpp',
                      'lib/integration/encoded/Dispatcher.cpp', 'lib/resource/protocol/Protocol.cpp']
    suffix = '.exe' if os.name == 'nt' else ''
    try:
        report['source_sha256_before'] = snapshot()
        run('compiler', [compiler, '--version'])
        # Every public TCP header must compile when it is the only include, so a
        # documented include order can never be required by accident.
        headers = sorted((ROOT / 'lib/integration/tcp').glob('*.hpp'))
        if len(headers) < 4:
            raise RuntimeError('Public TCP headers not found')
        alone = out / 'standalone-headers'
        alone.mkdir()
        for header in headers:
            unit = alone / (header.stem + '.cpp')
            unit.write_text('#include <integration/tcp/' + header.name + '>\n', encoding='utf-8')
            run('standalone-' + header.stem, base + ['-fsyntax-only', unit])
        report['standalone_headers'] = [header.name for header in headers]
        host = out / ('host' + suffix)
        run('build-host', base + profile + [HERE / 'main.cpp', *implementation, '-o', host])
        count('host', run('execute-host', [host]))
        noheap = out / ('noheap' + suffix)
        run('build-noheap', base + ['-O2', '-DTCP_STREAM_NO_HEAP=1', '-DTINY_DELEGATE_ENABLE_HEAP_FALLBACK=0',
                                  HERE / 'main.cpp', *implementation, '-o', noheap])
        count('noheap', run('execute-noheap', [noheap]))
        control = out / ('noheap-control' + suffix)
        run('build-noheap-control', base + ['-O2', '-Wno-unused-function', '-DTCP_STREAM_NO_HEAP=1',
            '-DTCP_STREAM_HEAP_CONTROL=1', HERE / 'main.cpp', *implementation, '-o', control])
        control_output = run('execute-noheap-control', [control], expected=86)
        if 'native TCP allocation refused' not in control_output:
            raise RuntimeError('Allocation negative control did not reach its trap')
        report['noheap_negative_exit'] = 86

        cmake, ninja = tool(args.cmake), tool(args.ninja)
        package, prefix, consumer = out / 'package', out / 'prefix', out / 'consumer'
        consumer.mkdir()
        (consumer / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(TcpConsumer LANGUAGES CXX)\n'
            'find_package(Telemetry REQUIRED COMPONENTS tcp_server resource_adapter)\n'
            'add_executable(consumer "' + (HERE / 'main.cpp').as_posix() + '")\n'
            'target_link_libraries(consumer PRIVATE Telemetry::tcp_server Telemetry::resource_adapter)\n', encoding='utf-8')
        common = ['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + str(ninja),
                  '-DCMAKE_CXX_COMPILER=' + str(compiler), '-DCMAKE_BUILD_TYPE=Release']
        run('configure-package', [cmake, '-S', ROOT, '-B', package, *common,
                                 '-DTELEMETRY_COMPONENTS=tcp_server;resource_adapter',
                                 '-DCMAKE_INSTALL_PREFIX=' + str(prefix)])
        run('build-package', [cmake, '--build', package, '-j2'])
        run('install-package', [cmake, '--install', package])
        run('configure-consumer', [cmake, '-S', consumer, '-B', consumer / 'build', *common,
                                  '-DCMAKE_PREFIX_PATH=' + str(prefix)])
        run('build-consumer', [cmake, '--build', consumer / 'build', '-j2'])
        count('installed-consumer', run('execute-consumer', [consumer / 'build' / ('consumer' + suffix)]))
        report['cmake_sha256'], report['ninja_sha256'] = digest(cmake), digest(ninja)

        if args.arm_cxx:
            arm = tool(args.arm_cxx)
            image = out / 'native-tcp-arm.o'
            run('arm-compiler', [arm, '--version'])
            run('offline-arm-compile', [arm, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                '-pedantic-errors', '-Ilib', '-Ilib/boost_pfr/include', '-Ilib/magic_enum',
                '-mcpu=cortex-m7', '-mthumb', '-fno-exceptions', '-fno-rtti', '-fstack-usage',
                '-DTINY_DELEGATE_ENABLE_HEAP_FALLBACK=0', '-c', HERE / 'main.cpp', '-o', image])
            nm = tool(str(arm).replace('g++', 'nm'))
            symbols = run('offline-arm-symbols', [nm, '-uC', image])
            if re.search(r'operator new|operator delete|\b(?:malloc|calloc|realloc|free)\b', symbols):
                raise RuntimeError('ARM probe references allocation')
            report['offline_arm'] = dict(compiled=True, executed=False, allocation_symbols=0,
                                         stack_usage={path.name: path.read_text() for path in out.glob('*.su')})
        report['source_sha256_after'] = snapshot()
        if report['source_sha256_before'] != report['source_sha256_after']:
            raise RuntimeError('Source inputs changed during qualification')
        report['outputs'] = {path.name: digest(path) for path in out.iterdir() if path.is_file() and path.name != 'summary.json'}
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('Native TCP stream qualification passed:', report['executions'], flush=True)


if __name__ == '__main__':
    main()
