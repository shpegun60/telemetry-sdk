#!/usr/bin/env python3
"""Verify optional Mixed Batch source selection; does not compile or use hardware. MIT."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--qmake', default='qmake')
    args = parser.parse_args()
    out = args.build_dir.resolve()
    if out == ROOT or ROOT in out.parents or out in ROOT.parents:
        parser.error('Use a dedicated external output directory')
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        parser.error('Use a fresh empty output directory; nothing is deleted')
    tool = Path(shutil.which(args.qmake) or args.qmake).resolve()
    if not tool.is_file():
        parser.error('qmake not found: '+str(tool))
    out.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, compiled=False, hardware=False, cases={},
                  qmake=str(tool), qmake_sha256=digest(tool))

    def include(path):
        return 'include("'+(ROOT/path).as_posix()+'")'

    core = include('lib/telemetry/telemetry.pri')
    integration = include('lib/integration/integration.pri')
    standalone = include('lib/integration/batch/batch_server.pri')
    base = ['lib/telemetry/abi/StructuredAbi.cpp', 'lib/telemetry/model/Adapter.cpp',
            'lib/integration/encoded/Dispatcher.cpp']
    batch = base+['lib/telemetry/encoded/Batch.cpp', 'lib/integration/encoded/BatchDispatcher.cpp']
    late = ['CONFIG += resource_telemetry_encoded', integration,
            'CONFIG += telemetry_batch_server', integration]
    cases = {
        'legacy': (['CONFIG += resource_telemetry_encoded', integration], base),
        'server': (['CONFIG += telemetry_batch_server', integration], batch),
        'client': (['CONFIG += telemetry_batch_client telemetry_encoded_backend', integration], []),
        'static-client': (['CONFIG += telemetry_batch_static_client telemetry_encoded_backend', integration], []),
        'static-client-repeated': (['CONFIG += telemetry_batch_static_client', integration, integration,
                                     include('lib/integration/batch/static_client.pri')], []),
        'late': ([core, *late], batch),
        'repeated': ([*late, core, integration, standalone, standalone], batch),
        'standalone': ([standalone], batch),
        'read-many-and-batch': (['CONFIG += telemetry_batch_server telemetry_read_many_server', integration],
            batch+['lib/telemetry/encoded/ReadMany.cpp', 'lib/integration/encoded/ReadManyDispatcher.cpp']),
    }
    inputs = sorted((ROOT/'lib').rglob('*.pri'))+[Path(__file__).resolve()]
    before = {str(path.relative_to(ROOT)): digest(path) for path in inputs}
    try:
        for name, (selection, sources) in cases.items():
            case = out/name
            case.mkdir()
            main_cpp = case/'main.cpp'
            main_cpp.write_text('int main() { return 0; }\n', encoding='utf-8')
            project = case/'Probe.pro'
            project.write_text('\n'.join(['TEMPLATE = app', 'TARGET = batch_inventory',
                'CONFIG += console c++20', 'CONFIG -= qt debug_and_release app_bundle',
                'SOURCES += "$$PWD/main.cpp"', *selection,
                'write_file("$$OUT_PWD/sources.txt", SOURCES)',
                'write_file("$$OUT_PWD/headers.txt", HEADERS)', '']), encoding='utf-8')
            result = subprocess.run([str(tool), '-o', str(case/'Makefile'), str(project)],
                cwd=case, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            (case/'qmake.log').write_bytes(result.stdout)
            if result.returncode:
                raise RuntimeError(name+': qmake failed\n'+result.stdout.decode(errors='replace'))
            inventory = case/'sources.txt'
            actual = [Path(line).resolve() for line in inventory.read_text().splitlines() if line]
            expected = [main_cpp, *[ROOT/path for path in sources]]
            if Counter(actual) != Counter(expected) or not all(path.is_file() for path in actual):
                raise RuntimeError(name+': incorrect or repeated sources: '+repr(actual))
            if name.startswith('static-client'):
                headers = [Path(line).resolve() for line in (case/'headers.txt').read_text().splitlines() if line]
                if headers.count(ROOT/'lib/integration/batch/StaticClient.hpp') != 1:
                    raise RuntimeError(name+': missing or repeated static batch header')
                if any('/telemetry/runtime/' in path.as_posix() or '/integration/dynamic/' in path.as_posix()
                       for path in headers):
                    raise RuntimeError(name+': static client imported runtime metadata')
            report['cases'][name] = dict(sources=[str(path) for path in actual],
                                        inventory_sha256=digest(inventory))
        report['source_sha256_before'] = before
        report['source_sha256_after'] = {str(path.relative_to(ROOT)): digest(path) for path in inputs}
        if before != report['source_sha256_after']:
            raise RuntimeError('qmake inputs changed during qualification')
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        (out/'summary.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(f'Mixed Batch qmake: {len(cases)} source inventories passed; not compiled')


if __name__ == '__main__':
    main()
