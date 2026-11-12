#!/usr/bin/env python3
"""Check optional qmake source selection and include order without compiling. MIT."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


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
    out.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, compiled=False, hardware=False, cases={}, commands=[],
                  started_utc=datetime.now(timezone.utc).isoformat())

    def save():
        (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

    def run(name, command):
        command = list(map(str, command))
        start = time.monotonic()
        try:
            result = subprocess.run(command, cwd=out, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=60)
            output, code = result.stdout, result.returncode
        except subprocess.TimeoutExpired as error:
            output, code = error.stdout or b'', None
        log = out / (name + '.log')
        log.write_bytes(output)
        report['commands'].append(dict(name=name, command=command, exit=code,
                                       seconds=time.monotonic() - start,
                                       log=log.name, sha256=digest(log)))
        save()
        if code != 0:
            raise RuntimeError(name + ' failed: ' + output.decode(errors='replace')[-4000:])
        return output.decode('utf-8', errors='replace')

    def snapshot():
        inputs = sorted((ROOT / 'lib').rglob('*.pri')) + [HERE / 'qmake.py']
        return {p.relative_to(ROOT).as_posix(): digest(p) for p in inputs}

    def include(path):
        return 'include("' + (ROOT / path).as_posix() + '")'

    core = include('lib/telemetry/telemetry.pri')
    integration = include('lib/integration/integration.pri')
    base_sources = ['lib/telemetry/abi/StructuredAbi.cpp', 'lib/telemetry/model/Adapter.cpp',
                    'lib/integration/encoded/Dispatcher.cpp']
    batch_sources = base_sources + ['lib/telemetry/encoded/ReadMany.cpp',
                                    'lib/integration/encoded/ReadManyDispatcher.cpp']
    mixed_sources = ['lib/telemetry/encoded/Batch.cpp',
                     'lib/integration/encoded/BatchDispatcher.cpp']
    late = [core, 'CONFIG += telemetry_read_many_server', integration]
    late_encoded = ['CONFIG += resource_telemetry_encoded', integration,
                    'CONFIG += telemetry_read_many_server', integration]
    mixed_first = ['CONFIG += telemetry_batch_server', integration,
                   'CONFIG += telemetry_read_many_server', integration]
    read_many_first = ['CONFIG += telemetry_read_many_server', integration,
                       'CONFIG += telemetry_batch_server', integration]
    cases = {
        'legacy': (['CONFIG += resource_telemetry_encoded', integration], base_sources),
        'batch': (['CONFIG += telemetry_read_many_server', integration], batch_sources),
        'client': (['CONFIG += telemetry_read_many_client telemetry_encoded_backend', integration], []),
        'static-client': (['CONFIG += telemetry_read_many_static_client telemetry_encoded_backend', integration], []),
        'static-client-repeated': ([include('lib/integration/read_many/static_client.pri'),
                                    include('lib/integration/read_many/static_client.pri')], []),
        'workbench': (['CONFIG += telemetry_read_many_workbench telemetry_encoded_backend', integration], []),
        'workbench-repeated': ([include('lib/integration/read_many/read_many_workbench.pri'),
                                include('lib/integration/read_many/read_many_workbench.pri')], []),
        'late': (late, batch_sources),
        'repeated': (late + [core, core, integration, integration], batch_sources),
        'late-encoded': (late_encoded, batch_sources),
        'late-encoded-repeated': (late_encoded + [integration, core, integration], batch_sources),
        'mixed-first': (mixed_first, batch_sources + mixed_sources),
        'read-many-first': (read_many_first, batch_sources + mixed_sources),
        'mixed-repeated': (mixed_first + [integration, core, integration], batch_sources + mixed_sources),
    }
    try:
        report['source_sha256_before'] = snapshot()
        qmake = Path(shutil.which(args.qmake) or args.qmake).resolve()
        if not qmake.is_file():
            raise RuntimeError('qmake executable not found: ' + str(qmake))
        report['qmake'] = str(qmake)
        report['qmake_sha256'] = digest(qmake)
        report['qmake_version'] = run('qmake-version', [qmake, '-v'])
        for name, (selection, selected_sources) in cases.items():
            case = out / name
            source, build = case / 'source', case / 'build'
            source.mkdir(parents=True)
            build.mkdir()
            main_cpp = source / 'main.cpp'
            main_cpp.write_text('int main() { return 0; }\n', encoding='utf-8')
            project = source / 'Probe.pro'
            project.write_text('\n'.join([
                'TEMPLATE = app', 'TARGET = read_many_source_inventory',
                'CONFIG += console c++20', 'CONFIG -= qt debug_and_release app_bundle',
                'SOURCES += "$$PWD/main.cpp"', *selection,
                'write_file("$$OUT_PWD/sources.txt", SOURCES)',
                'write_file("$$OUT_PWD/headers.txt", HEADERS)', '']), encoding='utf-8')
            run(name, [qmake, '-o', build / 'Makefile', project])
            inventory_file = build / 'sources.txt'
            actual = [Path(line).resolve() for line in
                      inventory_file.read_text(encoding='utf-8').splitlines() if line]
            expected = [main_cpp.resolve(), *[(ROOT / path).resolve() for path in selected_sources]]
            if Counter(actual) != Counter(expected) or not all(path.is_file() for path in actual):
                raise RuntimeError(name + ': wrong source inventory: ' + repr(actual))
            headers = [Path(line).resolve() for line in (build / 'headers.txt').read_text(encoding='utf-8').splitlines() if line]
            bridge = (ROOT / 'lib/integration/read_many/Workbench.hpp').resolve()
            if headers.count(bridge) != (1 if name.startswith('workbench') else 0):
                raise RuntimeError(name + ': optional Workbench header leaked or was duplicated')
            static_client = (ROOT / 'lib/integration/read_many/StaticClient.hpp').resolve()
            if headers.count(static_client) != (1 if name.startswith('static-client') else 0):
                raise RuntimeError(name + ': optional Static ReadMany header leaked or was duplicated')
            if name.startswith('static-client') and any('/telemetry/runtime/' in path.as_posix()
                                                       or '/integration/dynamic/' in path.as_posix()
                                                       for path in headers):
                raise RuntimeError(name + ': Static ReadMany imported runtime metadata')
            report['cases'][name] = dict(sources=[str(path) for path in actual],
                                         count=len(actual), inventory_sha256=digest(inventory_file))
            save()
        report['source_sha256_after'] = snapshot()
        if report['source_sha256_before'] != report['source_sha256_after']:
            raise RuntimeError('qmake inputs changed during qualification')
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        save()
    print(f'ReadMany qmake source inventories: {len(cases)} cases passed; no compilation or hardware')


if __name__ == '__main__':
    main()
