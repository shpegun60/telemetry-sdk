#!/usr/bin/env python3
"""Relocated CMake, packed ESM and installed-wheel SDK consumers.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs():
    paths = [ROOT / name for name in ('CMakeLists.txt', 'VERSION', 'LICENSE', 'package.json',
                                      'pyproject.toml', 'setup.py', 'MANIFEST.in',
                                      'examples/structured_client/Device.hpp')]
    for directory in ('lib', 'cmake', 'tools/telemetry_codegen', 'web', 'tests/sdk_packages',
                      'tests/codegen'):
        paths += [p for p in (ROOT / directory).rglob('*') if p.is_file()
                  and '__pycache__' not in p.parts and p.suffix in
                  ('.hpp', '.cpp', '.py', '.js', '.ts', '.json', '.in', '.cmake', '.txt')]
    return {p.relative_to(ROOT).as_posix(): digest(p) for p in sorted(set(paths))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--node', required=True)
    parser.add_argument('--tsc', required=True)
    parser.add_argument('--npm-cli', help='Optional bundled npm-cli.js path')
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', default='ninja')
    args = parser.parse_args()
    selected_node = shutil.which(args.node)
    if selected_node is None:
        parser.error('Node must name an executable on PATH or an explicit executable path')
    args.node = str(Path(selected_node).resolve())
    out = args.build_dir.resolve()
    if out == ROOT or ROOT in out.parents or out in ROOT.parents:
        parser.error('Use a fresh external output directory')
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        parser.error('Output must be fresh and empty; nothing is deleted')
    out.mkdir(parents=True, exist_ok=True)
    (out / 'package.json').write_text('{"private":true,"type":"module"}\n')
    report = dict(passed=False, hardware=False, started_utc=datetime.now(timezone.utc).isoformat(),
                  source_sha256=inputs(), commands=[], checks=0)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1',
               PIP_DISABLE_PIP_VERSION_CHECK='1', PIP_CACHE_DIR=str(out / 'pip-cache'),
               npm_config_cache=str(out / 'npm-cache'))
    env.pop('PYTHONPATH', None)
    env['PATH'] = str(Path(args.node).resolve().parent) + os.pathsep + env.get('PATH', '')

    def check(condition, label):
        report['checks'] += 1
        if not condition:
            raise RuntimeError(label)

    def run(name, command, cwd=out, success=True):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=600)
        log = out / (name + '.log')
        log.write_text(result.stdout, encoding='utf8')
        report['commands'].append(dict(name=name, command=command, returncode=result.returncode,
                                       log=log.name, sha256=digest(log)))
        check((result.returncode == 0) == success, name + ':\n' + result.stdout)
        print(name + ': exit=' + str(result.returncode), flush=True)
        return result.stdout

    try:
        # All builders operate on a prepared external copy, never on repository outputs.
        source = out / 'prepared-source'
        source.mkdir()
        for name in ('CMakeLists.txt', 'VERSION', 'LICENSE', 'README.md', 'package.json',
                     'pyproject.toml', 'setup.py', 'MANIFEST.in'):
            shutil.copy2(ROOT / name, source / name)
        for name in ('lib', 'cmake', 'tools/telemetry_codegen', 'web', 'tests/codegen'):
            shutil.copytree(ROOT / name, source / name,
                            ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        fixture = source / 'examples/structured_client/Device.hpp'
        fixture.parent.mkdir(parents=True)
        shutil.copy2(ROOT / 'examples/structured_client/Device.hpp', fixture)
        version = (source / 'VERSION').read_text().strip()
        check(json.loads((source / 'package.json').read_text())['version'] == version,
              'ESM/CMake version alignment')
        configure = [args.cmake, '-S', source, '-B', out / 'cmake-build', '-G', 'Ninja',
                     '-DCMAKE_MAKE_PROGRAM=' + str(shutil.which(args.ninja) or args.ninja),
                     '-DCMAKE_CXX_COMPILER=' + str(shutil.which(args.cxx) or args.cxx),
                     '-DTELEMETRY_COMPONENTS=client;encoded_wire',
                     '-DCMAKE_INSTALL_PREFIX=' + str(out / 'prefix')]
        run('cmake-configure', configure)
        run('cmake-install', [args.cmake, '--build', out / 'cmake-build', '--target', 'install'])
        prefix = out / 'relocated-prefix'
        (out / 'prefix').rename(prefix)
        info = json.loads((prefix / 'share/telemetry/SDKMetadata.json').read_text())
        check(info['sdkVersion'] == version and info['sourceRevision'] == '', 'Installed SDK identity')
        check(info['descriptorWireVersion'] == [3, 0] and info['encodedWireVersion'] == 1,
              'Package and wire versions are separate')
        consumer = out / 'cmake-consumer'
        consumer.mkdir()
        (consumer / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(Consumer LANGUAGES CXX)
if(SIMULATED_POINTER_BYTES)
    set(CMAKE_SIZEOF_VOID_P ${SIMULATED_POINTER_BYTES})
endif()
find_package(Telemetry ${REQUIRED_SDK} CONFIG REQUIRED COMPONENTS client encoded_wire)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE Telemetry::client Telemetry::encoded_wire)
''')
        (consumer / 'main.cpp').write_text('''#include <client/ClientResult.hpp>
#include <wire/BatchWire.hpp>
int main() {
    static_assert(telemetry::protocol::encoded::batch::maxItems == 32);
    return telemetry::protocol::encoded::batch::valid({1, 7}) ? 0 : 1;
}
''')
        run('cmake-consumer-configure', [args.cmake, '-S', consumer, '-B', out / 'consumer-build',
             '-G', 'Ninja', '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DREQUIRED_SDK=' + version])
        run('cmake-consumer-build', [args.cmake, '--build', out / 'consumer-build'])
        suffix = '.exe' if os.name == 'nt' else ''
        run('cmake-consumer-run', [out / ('consumer-build/consumer' + suffix)])
        run('cmake-header-only-cross-size', [args.cmake, '-S', consumer, '-B', out / 'header-cross-size',
             '-G', 'Ninja', '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DREQUIRED_SDK=' + version,
             '-DSIMULATED_POINTER_BYTES=4'])
        run('cmake-compiled-configure', [args.cmake, '-S', source, '-B', out / 'compiled-build',
             '-G', 'Ninja', '-DTELEMETRY_COMPONENTS=resource_protocol',
             '-DCMAKE_INSTALL_PREFIX=' + str(out / 'compiled-prefix')])
        run('cmake-compiled-install', [args.cmake, '--build', out / 'compiled-build', '--target', 'install'])
        compiled_consumer = out / 'compiled-consumer'
        compiled_consumer.mkdir()
        (compiled_consumer / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(CompiledConsumer LANGUAGES CXX)
set(CMAKE_SIZEOF_VOID_P 4)
find_package(Telemetry 0.1 CONFIG REQUIRED COMPONENTS resource_protocol)
''')
        run('cmake-compiled-cross-size-refusal', [args.cmake, '-S', compiled_consumer,
             '-B', out / 'compiled-cross-size', '-G', 'Ninja',
             '-DCMAKE_PREFIX_PATH=' + str(out / 'compiled-prefix')], success=False)
        # Semantic versioning: a consumer asks for the minor it was written for and
        # takes any patch of it; in 0.x the next minor may break and is refused.
        major, minor = (int(part) for part in version.split('.')[:2])
        run('cmake-same-minor', [args.cmake, '-S', consumer, '-B', out / 'same-minor',
             '-G', 'Ninja', '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DREQUIRED_SDK=%d.%d' % (major, minor)])
        run('cmake-next-minor-refusal', [args.cmake, '-S', consumer, '-B', out / 'next-minor',
             '-G', 'Ninja', '-DCMAKE_PREFIX_PATH=' + str(prefix),
             '-DREQUIRED_SDK=%d.%d' % (major, minor + 1)], success=False)
        run('cmake-version-refusal', [args.cmake, '-S', consumer, '-B', out / 'wrong-version',
             '-G', 'Ninja', '-DCMAKE_PREFIX_PATH=' + str(prefix), '-DREQUIRED_SDK=9.9.9'], success=False)
        run('cmake-revision-refusal', [args.cmake, '-S', source, '-B', out / 'bad-revision',
             '-G', 'Ninja', '-DTELEMETRY_SOURCE_REVISION=invalid'], success=False)

        node_home = Path(args.node).resolve().parent
        candidates = (node_home.parent / 'lib/node_modules/npm/bin/npm-cli.js',
                      node_home / 'node_modules/npm/bin/npm-cli.js')
        npm = Path(args.npm_cli).resolve() if args.npm_cli else next(
            (path for path in candidates if path.is_file()), None)
        check(npm is not None and npm.is_file(), 'Supply bundled npm-cli.js with --npm-cli')
        dist = out / 'dist'
        dist.mkdir()
        packed = json.loads(run('npm-pack', [args.node, npm, 'pack', source, '--json',
                                '--pack-destination', dist]))[0]
        tarball = dist / packed['filename']
        report['npm_tarball_sha256'] = digest(tarball)
        with tarfile.open(tarball) as archive:
            names = set(archive.getnames())
        check('package/web/DynamicClient.js' in names and 'package/web/telemetry.d.ts' in names,
              'Packed dynamic and runtime declarations')
        check(not any('/app/' in n or '/examples/' in n or n.endswith('.qml') for n in names),
              'Headless npm package contains no GUI source')
        npm_consumer = out / 'npm-consumer'
        npm_consumer.mkdir()
        (npm_consumer / 'package.json').write_text('{"private":true,"type":"module"}\n')
        run('npm-install', [args.node, npm, 'install', '--offline', '--ignore-scripts',
                           '--no-audit', '--no-fund', tarball], cwd=npm_consumer)
        (npm_consumer / 'Consumer.ts').write_text('''import {DeviceClient, EncodedBackend, Batch, Status} from 'telemetry-sdk';
import {parseDescriptor} from 'telemetry-sdk/runtime';
import {DynamicClient} from 'telemetry-sdk/dynamic';
const backend = () => new EncodedBackend(() => {}, {batch:true, readMany:true});
const typed = new DeviceClient({backend:backend(),matchesHeader:()=>true});
const dynamic = new DynamicClient({backend:backend()});
void typed; void dynamic; void Batch.items; void parseDescriptor; void Status.Ok;
''')
        run('npm-types', [args.node, args.tsc, npm_consumer / 'Consumer.ts', '--strict',
             '--noUnusedLocals', '--target', 'ES2022', '--module', 'NodeNext',
             '--moduleResolution', 'NodeNext', '--noEmit'])
        run('npm-execute', [args.node, '--input-type=module', '-e',
             "import{DeviceClient,EncodedBackend,Batch}from'telemetry-sdk';"
             "import{parseDescriptor}from'telemetry-sdk/runtime';"
             "import{DynamicClient}from'telemetry-sdk/dynamic';"
             "if(![DeviceClient,EncodedBackend,parseDescriptor,DynamicClient].every(x=>typeof x==='function')"
             "||Batch.items().length!==0)throw Error('exports');"], cwd=npm_consumer)

        run('python-sdist', [sys.executable, 'setup.py', 'sdist', '--dist-dir', dist], cwd=source)
        sdist = next(dist.glob('*.tar.gz'))
        run('python-wheel', [sys.executable, '-m', 'pip', 'wheel', '--no-deps', '--no-index',
             '--no-build-isolation', '--wheel-dir', dist, sdist])
        wheel = next(dist.glob('*.whl'))
        report['python_wheel_sha256'] = digest(wheel)
        with zipfile.ZipFile(wheel) as archive:
            wheel_metadata = archive.read(next(n for n in archive.namelist()
                                               if n.endswith('.dist-info/METADATA'))).decode()
            check('Name: telemetry-codegen-core\n' in wheel_metadata,
                  'Headless generator distribution identity')
            for name in ('Client.ts', 'Client.js', 'Client.d.ts'):
                check(archive.read('tools/telemetry_codegen/_runtime/' + name) ==
                      (source / 'lib/client/web' / name).read_bytes(), 'Canonical wheel runtime ' + name)
        venv = out / 'venv'
        run('python-venv', [sys.executable, '-m', 'venv', venv])
        python = venv / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
        cli = venv / ('Scripts/telemetry-codegen.exe' if os.name == 'nt' else 'bin/telemetry-codegen')
        run('python-install', [python, '-m', 'pip', 'install', '--no-index', '--no-deps', wheel])
        run('python-installed-identity', [python, '-c',
             "import tools.telemetry_codegen as c;"
             "assert c.__version__=='" + version + "';"
             "assert 'site-packages' in c.__file__;print(c.__file__)"])
        native = out / 'native'
        native.mkdir()
        producer = out / ('producer' + suffix)
        run('producer-build', [args.cxx, '-std=c++20', '-O2', '-I' + str(source / 'lib'),
             '-I' + str(source / 'lib/boost_pfr/include'), '-I' + str(source / 'lib/magic_enum'),
             source / 'tests/codegen/Producer.cpp', source / 'lib/telemetry/abi/StructuredAbi.cpp',
             source / 'lib/telemetry/model/Adapter.cpp', '-o', producer])
        run('producer-run', [producer, native])
        # Rename the prepared source after installation; installed consumers have no source path.
        source.rename(out / 'unavailable-source')
        descriptor = native / 'descriptor.bin'
        generated = out / 'generated'
        command = [cli, 'generate', descriptor, '--targets', 'cpp,js,ts', '--client-runtime',
                   '--output', generated]
        run('python-installed-generate', command)
        run('python-installed-check', [*command, '--check'])
        run('python-installed-metadata', [cli, 'generate', descriptor, '--targets', 'cpp,js,ts',
             '--client-runtime', '--structural-metadata', '--output', out / 'generated-metadata'])
        check((out / 'generated-metadata/metadata/schema.json').is_file() and
              (out / 'generated-metadata/js/device.metadata.js').is_file(), 'Installed optional metadata emitter')
        probe = out / 'metadata-consumer.cpp'
        probe.write_text('''#include <device_api/Metadata.hpp>
static_assert(device_api::metadata::version == 1);
static_assert(device_api::metadata::descriptorHeader.size() == 64);
static_assert(!device_api::metadata::typeRecords.empty());
int main() { return 0; }
''')
        run('installed-metadata-cpp', [args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror',
             '-I' + str(out / 'generated-metadata/cpp'), probe, '-o', out / ('metadata-consumer' + suffix)])
        run('installed-metadata-cpp-run', [out / ('metadata-consumer' + suffix)])
        run('installed-metadata-types', [args.node, args.tsc,
             out / 'generated-metadata/ts/device.metadata.ts', '--strict', '--target', 'ES2022',
             '--module', 'ES2022', '--noEmit'])
        unavailable = run('python-installed-qml-extension-refusal', [cli, 'generate', descriptor,
             '--targets', 'qml', '--output', out / 'generated-qml'], success=False)
        check('telemetry-ux-codegen' in unavailable,
              'Absent optional QML extension gives its installation name')
        check(not (out / 'generated-qml').exists(),
              'Absent optional QML extension publishes no output')
        run('python-module-compatibility', [python, '-m', 'tools.telemetry_codegen', 'inspect', descriptor])
        for folder, extension in (('js', 'js'), ('js', 'd.ts'), ('ts', 'ts')):
            canonical = out / 'unavailable-source/lib/client/web' / ('Client.' + extension)
            check((generated / folder / ('client.' + extension)).read_text() == canonical.read_text(),
                  'Installed --client-runtime canonical ' + extension)
        runtime = Path(run('python-runtime-location', [python, '-c',
            "from importlib.resources import files;print(files('tools.telemetry_codegen').joinpath('_runtime'))"]).strip())
        check(runtime.is_relative_to(venv), 'Installed assets belong to the isolated environment')
        missing = runtime / 'Client.js'
        missing.rename(runtime / 'Client.js.hidden')
        try:
            output = run('python-incomplete-assets-refusal', [cli, 'generate', descriptor,
                '--targets', 'js', '--client-runtime', '--output', out / 'incomplete-assets'], success=False)
            check('runtime assets are incomplete' in output, 'Incomplete asset diagnostic')
            check(not (out / 'incomplete-assets').exists(), 'Failed generation publishes nothing')
        finally:
            (runtime / 'Client.js.hidden').rename(missing)
        hidden = runtime.with_name('_runtime-hidden')
        runtime.rename(hidden)
        try:
            # An ambient ancestor with similarly named files is not an installed SDK resource.
            ambient = runtime.parents[2] / 'lib/client/web'
            ambient.mkdir(parents=True)
            (ambient / 'Client.js').write_text('ambient wrong runtime')
            (ambient / 'Client.d.ts').write_text('ambient wrong runtime')
            output = run('python-missing-assets-refusal', [cli, 'generate', descriptor,
                '--targets', 'js', '--client-runtime', '--output', out / 'missing-assets'], success=False)
            check('runtime assets are missing' in output, 'Missing assets never use ambient ancestor')
            check(not (out / 'missing-assets').exists(), 'Missing resources publish nothing')
        finally:
            hidden.rename(runtime)
        with zipfile.ZipFile(wheel) as archive:
            metadata = archive.read(next(n for n in archive.namelist()
                                         if n.endswith('.dist-info/METADATA'))).decode()
        check(not any(line.startswith('Requires-Dist:') for line in metadata.splitlines()),
              'Codegen wheel adds no mandatory Qt or third-party runtime')
        run('generated-types', [args.node, args.tsc, generated / 'ts/device.ts',
             generated / 'ts/client.ts', '--strict', '--target', 'ES2022', '--module', 'ES2022',
             '--noEmit'])
        report['source_sha256_after'] = inputs()
        check(report['source_sha256'] == report['source_sha256_after'], 'Source inputs changed')
        report['passed'] = True
        print('SDK package checks=' + str(report['checks']) + ' failures=0', flush=True)
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
