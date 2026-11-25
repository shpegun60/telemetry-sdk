#!/usr/bin/env python3
"""Strict TS, emitted JS/TS, shared C++ traces and actual Encoded/RPC server checks.

All compiler/test outputs are external. No downloads or hardware access.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
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

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def snapshot():
    roots = ('lib/telemetry', 'lib/resource', 'lib/client', 'lib/common', 'lib/wire', 'lib/integration', 'tests/client_web', 'tests/async_client',
             'tests/codegen', 'tools/telemetry_codegen', 'examples/structured_client',
             'examples/client_web', 'examples/codegen_web', 'lib/delegate',
             'lib/boost_pfr/include', 'lib/magic_enum')
    extensions = {'.ts', '.mts', '.mjs', '.js', '.py', '.hpp', '.cpp', '.json'}
    return {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for root in roots for path in sorted((ROOT / root).rglob('*'))
            if path.is_file() and path.suffix in extensions and '__pycache__' not in path.parts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--node', default='node')
    parser.add_argument('--tsc', default='tsc')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    args = parser.parse_args()
    build = Path(args.build_dir).resolve()
    if build == ROOT or ROOT in build.parents:
        parser.error('use an external build directory')
    build.mkdir(parents=True, exist_ok=True)
    (build / 'package.json').write_text('{"type":"module"}\n')
    report = dict(passed=False, started_at_utc=datetime.now(timezone.utc).isoformat(),
                  source_sha256=snapshot(), commands=[], executions=[], traces=[])
    tools = [Path(shutil.which(value) or value).resolve()
             for value in (args.node, args.tsc, args.cxx, sys.executable)]
    tsc_lib = Path(args.tsc).resolve().parent.parent / 'lib'
    tools += list(tsc_lib.glob('*.d.ts'))
    tools += [tsc_lib / '_tsc.js', tsc_lib / 'tsc.js']
    report['tool_sha256'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                             for path in sorted(set(tools)) if path.is_file()}
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')

    def run(command, name, *, input=None):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=ROOT, env=env, input=input, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
        (build / (name + '.log')).write_text(result.stdout, encoding='utf8')
        report['commands'].append(dict(name=name, command=command, returncode=result.returncode))
        if result.returncode:
            raise RuntimeError(name + ' failed:\n' + result.stdout)
        print(name + ': ' + result.stdout.strip().split('\n')[-1], flush=True)
        return result.stdout

    def counted(command, name, floor):
        output = run(command, name)
        matches = re.findall(r'checks=(\d+)(?: packets=\d+)? failures=0', output)
        if len(matches) != 1 or int(matches[0]) < floor:
            raise RuntimeError('missing positive check count: ' + name)
        report['executions'].append(dict(name=name, checks=int(matches[0])))

    suffix = '.exe' if os.name == 'nt' else ''
    flags = [args.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
             '-Ilib', '-Ilib/boost_pfr/include', '-Ilib/magic_enum']
    abi = ROOT / 'lib/telemetry/abi/StructuredAbi.cpp'
    adapter = ROOT / 'lib/telemetry/model/Adapter.cpp'
    tsc = [args.node, args.tsc] if Path(args.tsc).name in ('tsc', 'tsc.js') else [args.tsc]
    cli = [sys.executable, '-m', 'tools.telemetry_codegen']
    try:
        run([args.node, '--version'], 'node-version')
        run([*tsc, '--version'], 'tsc-version')
        run([args.cxx, '--version'], 'compiler-version')
        runtime = build / 'compiled'
        run([sys.executable, ROOT / 'lib/client/web/build.py', '--node', args.node,
             '--tsc', args.tsc, '--output', runtime], 'single-source-runtime')
        trace = build / ('trace' + suffix)
        run([*flags, ROOT / 'tests/async_client/Trace.cpp', '-o', trace], 'compile-cpp-traces')
        tests = json.loads((HERE / 'traces.json').read_text())
        expected = {}
        for test in tests:
            output = run([trace], 'cpp-trace-' + test['name'], input='\n'.join(test['actions']) + '\n')
            expected[test['name']] = output.strip().splitlines()
        for label, module in (('js', ROOT / 'lib/client/web/Client.js'),
                              ('ts', runtime / 'Client.js')):
            counted([args.node, HERE / 'Check.mjs', module], 'web-' + label, 332)
            counted([args.node, HERE / 'Lifetime.mjs', module], 'lifetime-' + label, 335)
            counted([args.node, HERE / 'Polling.mjs', module], 'polling-' + label, 90)
            counted([args.node, HERE / 'Retrying.mjs', module], 'retrying-' + label, 370)
            counted([args.node, HERE / 'UartBudget.mjs', module], 'uart-budget-' + label, 143)
            traces = run([args.node, HERE / 'Trace.mjs', module], 'traces-' + label)
            for line in traces.splitlines():
                test = json.loads(line)
                if test['events'] != expected[test['name']]:
                    raise RuntimeError(f"C++/{label} trace differs {test['name']}:\n"
                                       f"C++ {expected[test['name']]}\nweb {test['events']}")
                report['traces'].append(dict(runtime=label, name=test['name'], events=test['events']))
        bridge = build / ('encoded-bridge' + suffix)
        run([*flags, ROOT / 'tests/async_client/EncodedBridge.cpp',
             ROOT / 'lib/integration/encoded/Dispatcher.cpp', adapter, abi, '-o', bridge],
            'compile-encoded-bridge')
        descriptor = build / 'encoded-descriptor.bin'
        run([bridge, '--descriptor', descriptor], 'encoded-descriptor')
        encoded = build / 'encoded-sdk'
        run([*cli, 'generate', descriptor, '--targets', 'js,ts', '--client-runtime', '--output', encoded], 'generate-encoded')
        producer = build / ('producer' + suffix)
        run([*flags, ROOT / 'tests/codegen/Producer.cpp', abi, '-o', producer], 'compile-producer')
        native = build / 'native'
        run([producer, native], 'produce-native')
        rpc = build / 'rpc-sdk'
        run([*cli, 'generate', native / 'descriptor.bin', '--targets', 'js,ts', '--client-runtime', '--output', rpc], 'generate-rpc')
        rpc_bridge = build / ('rpc-bridge' + suffix)
        run([*flags, ROOT / 'tests/codegen/Bridge.cpp', abi, adapter,
             ROOT / 'lib/resource/protocol/Protocol.cpp', ROOT / 'lib/integration/telemetry/v3/RpcMailbox.cpp',
             ROOT / 'lib/client/rpc/RpcClient.cpp', '-o', rpc_bridge], 'compile-rpc-bridge')
        for mode, sdk, desc, executable in (('encoded', encoded, descriptor, bridge),
                ('rpc', rpc, native / 'descriptor.bin', rpc_bridge)):
            compiled = sdk / 'ts-compiled'
            run([*tsc, sdk / 'ts/device.ts', sdk / 'ts/client.ts', '--strict', '--target', 'ES2022',
                 '--module', 'ES2022', '--outDir', compiled], 'compile-generated-' + mode)
            # Generated .d.ts consumers and emitted TS must have the same typed binding API.
            consumer = sdk / 'Consumer.ts'
            consumer.write_text("import * as S from './js/device.js';\n"
                "import {DeviceClient, EncodedBackend, RetryingEncodedBackend, type EncodedRetryOptions, type Result} from './js/client.js';\n"
                "const C = new DeviceClient({backend: new EncodedBackend(() => {}), matchesHeader:S.matchesHeader});\n"
                "const options: EncodedRetryOptions = {replyTimeoutMs:100,maxRetries:3};\n"
                "new DeviceClient({backend: new RetryingEncodedBackend(() => {},options), matchesHeader:S.matchesHeader});\n"
                "const r: Promise<Result<S.device.types.Config>> = C.read(S.device.Config);\n"
                "const w = C.write(S.device.Config, {} as S.device.types.Config);\n"
                "Promise.all([r,w]);\n")
            run([*tsc, consumer, '--strict', '--target', 'ES2022', '--module', 'ES2022', '--noEmit'], 'consumer-' + mode)
            example = []
            if mode == 'rpc':
                example_root = build / 'example'
                example_root.mkdir(exist_ok=True)
                shutil.copy2(ROOT / 'examples/client_web/client.ts', example_root / 'client.ts')
                shutil.copytree(sdk / 'js', example_root / 'generated/js', dirs_exist_ok=True)
                run([*tsc, example_root / 'client.ts', '--strict', '--target', 'ES2022',
                     '--module', 'ES2022', '--outDir', example_root / 'compiled'], 'compile-async-example')
                shutil.copytree(sdk / 'js', example_root / 'compiled/generated/js', dirs_exist_ok=True)
                example = [example_root / 'compiled/client.js']
                legacy = build / 'legacy-example'
                legacy.mkdir(exist_ok=True)
                shutil.copy2(ROOT / 'examples/codegen_web/client.ts', legacy / 'client.ts')
                shutil.copytree(sdk / 'js', legacy / 'generated/js', dirs_exist_ok=True)
                (legacy / 'runtime').mkdir(exist_ok=True)
                for name in ('transport.mjs', 'transport.d.mts'):
                    text = (ROOT / 'examples/codegen_web' / name).read_text().replace(
                        '../../lib/client/web/Client.js', './runtime/Client.js')
                    (legacy / name).write_text(text)
                for name in ('Client.js', 'Client.d.ts'):
                    shutil.copy2(ROOT / 'lib/client/web' / name, legacy / 'runtime' / name)
                run([*tsc, legacy / 'client.ts', '--strict', '--target', 'ES2022',
                     '--module', 'NodeNext', '--moduleResolution', 'NodeNext', '--noEmit'],
                    'compile-legacy-example')
            for label, folder in (('js', sdk / 'js'), ('ts', compiled)):
                counted([args.node, HERE / 'Integration.mjs', folder / 'client.js', folder / 'device.js',
                         desc, executable, mode, *example], mode + '-' + label,
                        138 if mode == 'encoded' else 45)
        report['source_sha256_after'] = snapshot()
        if report['source_sha256'] != report['source_sha256_after']:
            raise RuntimeError('source inputs changed during qualification; repeat from frozen inputs')
        report['checks'] = sum(item['checks'] for item in report['executions'])
        report['passed'] = True
    finally:
        report['completed_at_utc'] = datetime.now(timezone.utc).isoformat()
        (build / 'summary.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print(f"async web total checks={report['checks']} shared_traces={len(report['traces'])} failures=0")


if __name__ == '__main__':
    main()
