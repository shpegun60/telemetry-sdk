#!/usr/bin/env python3
"""Build sealed modules, relocate their installation and exercise downstream consumers. MIT."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
MODULES = ('core', 'runtime', 'dynamic_client', 'read_many_client', 'read_many_static_client', 'batch_client', 'batch_static_client', 'inspector', 'workbench', 'read_many_workbench', 'resource', 'resource_protocol', 'client', 'resource_adapter',
           'encoded_server', 'batch_server', 'rpc_server', 'rpc_sessions', 'encoded_backend', 'rpc_client', 'rpc_backend', 'tcp_server',
           'link', 'tcp_link')
FORBIDDEN = {
    'batch_static_client': ('telemetry/runtime/RuntimeSchema.hpp',
                            'integration/dynamic/DynamicClient.hpp', 'telemetry/Telemetry.hpp',
                            'telemetry/codec/Codec.hpp', 'resource/Resource.hpp', 'boost/pfr.hpp',
                            'magic_enum.hpp', 'wire/RpcWire.hpp',
                            'integration/encoded/Backend.hpp', 'integration/rpc/Backend.hpp',
                            'integration/inspector/InspectorModel.hpp'),
    'read_many_static_client': ('telemetry/runtime/RuntimeSchema.hpp',
                               'integration/dynamic/DynamicClient.hpp', 'telemetry/Telemetry.hpp',
                               'telemetry/codec/Codec.hpp', 'resource/Resource.hpp', 'boost/pfr.hpp',
                               'magic_enum.hpp', 'wire/RpcWire.hpp',
                               'integration/encoded/Backend.hpp', 'integration/rpc/Backend.hpp',
                               'integration/inspector/InspectorModel.hpp'),
    'batch_client': ('telemetry/Telemetry.hpp', 'telemetry/type/Traits.hpp',
                     'telemetry/codec/Codec.hpp', 'resource/Resource.hpp', 'boost/pfr.hpp',
                     'magic_enum.hpp', 'wire/RpcWire.hpp', 'integration/encoded/Backend.hpp',
                     'integration/rpc/Backend.hpp', 'integration/inspector/InspectorModel.hpp'),
    'batch_server': ('resource/Resource.hpp', 'client/DeviceClient.hpp',
                     'telemetry/runtime/RuntimeSchema.hpp', 'integration/dynamic/DynamicClient.hpp'),
    'read_many_workbench': ('telemetry/Telemetry.hpp', 'telemetry/type/Traits.hpp',
                            'telemetry/codec/Codec.hpp', 'resource/Resource.hpp', 'boost/pfr.hpp',
                            'magic_enum.hpp', 'wire/RpcWire.hpp',
                            'integration/encoded/Backend.hpp', 'integration/rpc/Backend.hpp'),
    'read_many_client': ('telemetry/Telemetry.hpp', 'telemetry/type/Traits.hpp',
                         'telemetry/codec/Codec.hpp', 'resource/Resource.hpp', 'boost/pfr.hpp',
                         'magic_enum.hpp', 'wire/RpcWire.hpp', 'integration/encoded/Backend.hpp',
                         'integration/rpc/Backend.hpp', 'integration/inspector/InspectorModel.hpp'),
    'workbench': ('telemetry/Telemetry.hpp', 'telemetry/type/Traits.hpp',
                  'telemetry/codec/Codec.hpp', 'resource/Resource.hpp', 'boost/pfr.hpp',
                  'magic_enum.hpp', 'wire/EncodedWire.hpp', 'wire/RpcWire.hpp',
                  'integration/encoded/Backend.hpp', 'integration/rpc/Backend.hpp'),
    'inspector': ('telemetry/Telemetry.hpp', 'telemetry/type/Traits.hpp',
                  'telemetry/codec/Codec.hpp', 'resource/Resource.hpp', 'boost/pfr.hpp',
                  'magic_enum.hpp', 'wire/EncodedWire.hpp', 'wire/RpcWire.hpp',
                  'integration/encoded/Backend.hpp', 'integration/rpc/Backend.hpp'),
    'dynamic_client': ('telemetry/Telemetry.hpp', 'telemetry/type/Traits.hpp',
                       'telemetry/codec/Codec.hpp', 'resource/Resource.hpp', 'boost/pfr.hpp',
                       'magic_enum.hpp', 'wire/EncodedWire.hpp', 'wire/RpcWire.hpp',
                       'integration/encoded/Backend.hpp', 'integration/rpc/Backend.hpp',
                       'integration/inspector/InspectorModel.hpp'),
    'runtime': ('telemetry/Telemetry.hpp', 'telemetry/type/Traits.hpp', 'telemetry/codec/Codec.hpp',
                'resource/Resource.hpp', 'client/DeviceClient.hpp', 'boost/pfr.hpp', 'magic_enum.hpp',
                'integration/encoded/Backend.hpp', 'wire/EncodedWire.hpp', 'wire/RpcWire.hpp',
                'integration/inspector/InspectorModel.hpp'),
    'core': ('resource/Resource.hpp', 'client/DeviceClient.hpp', 'wire/EncodedWire.hpp', 'wire/RpcWire.hpp',
             'integration/inspector/InspectorModel.hpp'),
    'resource': ('telemetry/Telemetry.hpp', 'client/DeviceClient.hpp'),
    'resource_protocol': ('telemetry/Telemetry.hpp', 'client/DeviceClient.hpp'),
    'client': ('telemetry/runtime/RuntimeSchema.hpp', 'integration/dynamic/DynamicClient.hpp', 'telemetry/Telemetry.hpp', 'resource/Resource.hpp', 'wire/EncodedWire.hpp', 'wire/RpcWire.hpp'),
    'encoded_server': ('resource/Resource.hpp', 'client/DeviceClient.hpp'),
    'encoded_backend': ('telemetry/Telemetry.hpp', 'resource/Resource.hpp'),
    'rpc_client': ('telemetry/model/Model.hpp', 'integration/telemetry/v3/RpcMailbox.hpp'),
    'rpc_backend': ('telemetry/model/Model.hpp', 'integration/telemetry/v3/RpcMailbox.hpp'),
    'rpc_server': ('client/DeviceClient.hpp',),
    'resource_adapter': ('client/DeviceClient.hpp',),
    'rpc_sessions': ('client/DeviceClient.hpp',),
    'tcp_server': ('client/DeviceClient.hpp', 'client/rpc/RpcClient.hpp',
                   'integration/encoded/Backend.hpp', 'integration/rpc/Backend.hpp',
                   'integration/telemetry/v3/RpcMailbox.hpp',
                   'integration/telemetry/v3/DescriptorFile.hpp',
                   'integration/inspector/InspectorModel.hpp',
                   'integration/dynamic/DynamicClient.hpp', 'telemetry/runtime/RuntimeSchema.hpp'),
    # A framer is medium framing only: no client, Model, server or Encoded wire.
    'link': ('client/DeviceClient.hpp', 'telemetry/Telemetry.hpp', 'wire/EncodedWire.hpp'),
    'tcp_link': ('client/DeviceClient.hpp', 'telemetry/Telemetry.hpp', 'integration/encoded/Dispatcher.hpp'),
}
for _module in MODULES:
    FORBIDDEN[_module] += ('integration/qt/DeviceTransport.hpp',
                            'integration/qt/DescriptorBootstrap.hpp',
                            'integration/qt/SerialTransport.hpp',
                            'integration/serial/Profile.hpp')
    if _module != 'batch_static_client':
        FORBIDDEN[_module] += ('integration/batch/StaticClient.hpp',)
    if _module != 'read_many_static_client':
        FORBIDDEN[_module] += ('integration/read_many/StaticClient.hpp',)
    if _module != 'batch_client':
        FORBIDDEN[_module] += ('integration/batch/Client.hpp',)
    if _module != 'batch_server':
        FORBIDDEN[_module] += ('telemetry/encoded/Batch.hpp', 'integration/encoded/BatchDispatcher.hpp')
    if _module not in ('read_many_client', 'read_many_workbench'):
        FORBIDDEN[_module] += ('integration/read_many/Client.hpp',)
    if _module not in ('workbench', 'read_many_workbench'):
        FORBIDDEN[_module] += ('integration/workbench/WorkbenchModel.hpp',
                              'integration/workbench/RequestEditor.hpp')
    if _module != 'read_many_workbench':
        FORBIDDEN[_module] += ('integration/read_many/Workbench.hpp',)
    if _module not in ('tcp_server', 'tcp_link'):
        FORBIDDEN[_module] += ('integration/tcp/Framing.hpp',)
    if _module != 'tcp_server':
        FORBIDDEN[_module] += ('integration/tcp/Server.hpp',)


def check_install_paths(args, out, root, run, extra_config, symlinks):
    """Cleanup and file(INSTALL) must use identical normalized destinations."""
    result = {}

    def configure(name, prefix, *options):
        case = root / name
        case.mkdir(exist_ok=True)
        build = case / 'build'
        run([args.cmake, '-S', ROOT, '-B', build, '-G', 'Ninja',
             '-DTELEMETRY_COMPONENTS=client', '-DCMAKE_CXX_COMPILER=' + args.cxx,
             '-DCMAKE_INSTALL_PREFIX=' + str(prefix), *extra_config, *options],
            'install-path-' + name + '-configure')
        return build

    suffix = Path('include/telemetry-client/client/DeviceClient.hpp')
    name = 'relative-prefix'
    prefix = root / name / 'installed'
    build = configure(name, root / name / 'configured')
    run([args.cmake, '--install', build, '--prefix', prefix.relative_to(out).as_posix()],
        'install-path-relative-prefix')
    if not (prefix / suffix).is_file():
        raise RuntimeError('Relative runtime prefix did not resolve against the install invocation directory')
    result[name] = True

    # Exercise the generated normalizer without performing any root installation.
    script = (build / 'cmake_install.cmake').read_text(encoding='utf-8')
    start = script.index('# Telemetry install-path normalization begin.')
    end = script.index('# Telemetry install-path normalization end.', start)
    normalizer = script[start:end]
    values = [(out.drive, out.drive)] if os.name == 'nt' else [('', ''), ('/', '')]
    for index, (value, expected) in enumerate(values):
        probe = root / ('prefix-root-' + str(index) + '.cmake')
        probe.write_text('set(CMAKE_INSTALL_PREFIX "' + value + '")\n' + normalizer +
                         '\nif(NOT CMAKE_INSTALL_PREFIX STREQUAL "' + expected + '")\n'
                         'message(FATAL_ERROR "Root prefix became ${CMAKE_INSTALL_PREFIX}")\nendif()\n',
                         encoding='utf-8')
        run([args.cmake, '-P', probe], 'install-path-root-script-' + str(index))
    result['root_normalizer_script'] = True

    if os.name == 'nt':
        name = 'native-separators'
        prefix = root / name / 'installed'
        build = configure(name, prefix, '-DCMAKE_INSTALL_INCLUDEDIR=nested\\..\\include',
                          '-DCMAKE_INSTALL_LIBDIR=nested\\..\\lib', '-DCMAKE_INSTALL_DATADIR=nested\\..\\share')
        run([args.cmake, '--install', build], 'install-path-native-separators')
        required = [suffix, Path('lib/cmake/Telemetry/TelemetryTargets.cmake'), Path('share/telemetry/LICENSE')]
        if not all((prefix / path).is_file() for path in required):
            raise RuntimeError('Native Windows install directories were not converted before normalization')
        result[name] = True

    if os.name != 'nt':
        # CMake strips the slash from --prefix /; empty and root must never become CWD.
        for name, argument in (('root-prefix', '/'), ('empty-prefix', '')):
            destdir = root / name / 'destdir'
            run([args.cmake, '--install', build, '--prefix', argument], 'install-path-' + name,
                extra_environment={'DESTDIR': str(destdir)})
            expected = destdir / suffix if argument else destdir / (root / 'relative-prefix/configured').relative_to(out.anchor) / suffix
            if not expected.is_file():
                raise RuntimeError(name + ': root installation escaped the external DESTDIR')
            result[name] = True
        for name, configured in (('configure-relative-prefix',
                                  (root / 'configure-relative-prefix/installed').relative_to(out).as_posix()),
                                 ('configure-empty-prefix', '')):
            build = configure(name, configured)
            destdir = root / name / 'destdir'
            run([args.cmake, '--install', build], 'install-path-' + name,
                extra_environment={'DESTDIR': str(destdir)})
            if configured:
                expected = destdir / (out / configured).relative_to(out.anchor) / suffix
            else:
                expected = destdir / suffix
            if not expected.is_file():
                raise RuntimeError(name + ': unexpected actual header installation directory')
            result[name] = True
    if not symlinks:
        result['symlink_dotdot'] = 'unsupported symbolic-link creation on this host'
        return result

    name = 'includedir-escape'
    case = root / name
    outside = case / 'outside'
    outside.mkdir(parents=True)
    marker = outside / 'keep.txt'
    marker.write_bytes(b'Relative header escape must be refused before installation.\n')
    before = hashlib.sha256(marker.read_bytes()).hexdigest()
    build = configure(name, case / 'prefix', '-DCMAKE_INSTALL_INCLUDEDIR=../outside')
    text = run([args.cmake, '--install', build], 'install-path-' + name, expected_failure=True)
    if 'must stay inside' not in text or hashlib.sha256(marker.read_bytes()).hexdigest() != before:
        raise RuntimeError('Relative header escape was not safely refused')
    if list(outside.rglob('tiny_delegate.hpp')):
        raise RuntimeError('Relative header escape copied files before refusal')
    result[name] = {'refused': True, 'canary_sha256': before}

    for name in ('destinations-dotdot', 'configure-prefix-dotdot', 'runtime-prefix-dotdot',
                 'relative-prefix-dotdot', 'destdir-dotdot'):
        if name == 'destdir-dotdot' and os.name == 'nt':
            continue
        case = root / name
        prefix, outside = case / 'prefix', case / 'outside'
        prefix.mkdir(parents=True)
        (outside / 'deep').mkdir(parents=True)
        (prefix / 'link').symlink_to(outside / 'deep', target_is_directory=True)
        markers = [prefix / 'keep.txt', outside / 'keep.txt', outside / 'include/telemetry-client/keep.txt']
        for marker in markers:
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.write_bytes(b'Normalized installation must preserve sibling canaries.\n')
        before = {str(p.relative_to(case)): hashlib.sha256(p.read_bytes()).hexdigest() for p in markers}
        raw_prefix = prefix.as_posix() + '/link/../chosen'
        configured = raw_prefix if name == 'configure-prefix-dotdot' else str(prefix)
        options = []
        if name == 'destinations-dotdot':
            options = ['-DTELEMETRY_COMPONENTS=client;resource_protocol',
                       '-DCMAKE_INSTALL_INCLUDEDIR=link/../include', '-DCMAKE_INSTALL_LIBDIR=link/../lib',
                       '-DCMAKE_INSTALL_DATADIR=link/../share']
        build = configure(name, configured, *options)
        if name == 'destinations-dotdot':
            run([args.cmake, '--build', build, '-j2'], 'install-path-destinations-build')
        command = [args.cmake, '--install', build]
        expected_prefix = prefix
        extra_environment = None
        if name in ('configure-prefix-dotdot', 'runtime-prefix-dotdot', 'relative-prefix-dotdot'):
            expected_prefix = prefix / 'chosen'
        if name == 'runtime-prefix-dotdot':
            command += ['--prefix', raw_prefix]
        elif name == 'relative-prefix-dotdot':
            command += ['--prefix', prefix.relative_to(out).as_posix() + '/link/../chosen']
        elif name == 'destdir-dotdot':
            extra_environment = {'DESTDIR': prefix.as_posix() + '/link/../staged'}
            expected_prefix = prefix / 'staged' / prefix.relative_to(prefix.anchor)
        run(command, 'install-path-' + name, extra_environment=extra_environment)
        if not (expected_prefix / suffix).is_file() or list(outside.rglob('DeviceClient.hpp')):
            raise RuntimeError(name + ': actual header installation disagreed with normalized cleanup')
        if name == 'destinations-dotdot':
            required = ['lib/cmake/Telemetry/TelemetryTargets.cmake', 'share/telemetry/LICENSE']
            if not list((prefix / 'lib').glob('*telemetry_resource_protocol*')):
                raise RuntimeError('Normalized library destination lost the compiled archive')
            if not all((prefix / path).is_file() for path in required):
                raise RuntimeError('Package or license destination was not normalized')
            if list((outside / 'lib').glob('**/*')) or list((outside / 'share').glob('**/*')):
                raise RuntimeError('Unnormalized library/package/license installation escaped the prefix')
        after = {str(p.relative_to(case)): hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else None
                 for p in markers}
        if after != before:
            raise RuntimeError(name + ': installation changed a sibling canary')
        result[name] = {'actual_header': str((expected_prefix / suffix).relative_to(root)),
                        'canary_sha256_before': before, 'canary_sha256_after': after}
    return result


def check_windows_junctions(args, root, run, extra_config):
    """Mandatory Windows reparse-point controls; CreateJunction needs no symlink privilege."""
    import stat
    links = []
    try:
        return windows_junction_cases(args, root, run, extra_config, links)
    finally:
        # Unlink only recorded junction objects, never traverse their targets.
        # CMake may already have replaced owned leaf links with normal directories.
        for link in reversed(links):
            if os.path.lexists(link) and link.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                link.rmdir()


def windows_junction_cases(args, root, run, extra_config, links):
    import _winapi
    result = {}
    root = root / 'junctions'
    root.mkdir()

    def junction(link, target):
        link.parent.mkdir(parents=True, exist_ok=True)
        link.parent.resolve().relative_to(root.resolve())
        target.resolve().relative_to(root.resolve())
        _winapi.CreateJunction(str(target), str(link))
        links.append(link)
        link.resolve().relative_to(root.resolve())

    def configure(build, prefix, label, refusal=False):
        return run([args.cmake, '-S', ROOT, '-B', build, '-G', 'Ninja',
                    '-DTELEMETRY_COMPONENTS=client', '-DCMAKE_CXX_COMPILER=' + args.cxx,
                    '-DCMAKE_INSTALL_PREFIX=' + str(prefix), *extra_config], label,
                   expected_failure=refusal)

    def marker(path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'Windows junction containment must preserve this canary.\n')
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def check_marker(path, before):
        after = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
        if after != before:
            raise RuntimeError('Junction cleanup changed a canary: ' + str(path))
        return after

    for existing in (False, True):
        name = 'internal-' + ('existing' if existing else 'missing')
        case = root / name
        build, prefix = case / 'build', case / 'prefix'
        canaries = []
        for owner, link, leaf in ((build, build / 'module-headers', 'client'),
                                  (prefix, prefix / 'include', 'telemetry-client')):
            target = owner / 'owned'
            target.mkdir(parents=True)
            canary = target / 'sibling/keep.txt'
            canaries.append((canary, marker(canary)))
            if existing:
                marker(target / leaf / 'old-header.hpp')
            junction(link, target)
        configure(build, prefix, 'junction-' + name + '-configure')
        run([args.cmake, '--install', build], 'junction-' + name + '-install')
        if not (build / 'owned/client/client/DeviceClient.hpp').is_file() or not (
                prefix / 'owned/telemetry-client/client/DeviceClient.hpp').is_file():
            raise RuntimeError('Internal junction did not produce headers')
        if (build / 'owned/client/old-header.hpp').exists() or (prefix / 'owned/telemetry-client/old-header.hpp').exists():
            raise RuntimeError('Internal junction retained obsolete owned headers')
        result[name] = {'canaries': {p.relative_to(root).as_posix(): check_marker(p, h) for p, h in canaries}}

    for kind in ('staging', 'installation'):
        for existing in (False, True):
            name = kind + '-' + ('existing' if existing else 'missing')
            case = root / name
            build, prefix, outside = case / 'build', case / 'prefix', case / 'outside'
            leaf = 'delegate' if kind == 'staging' else 'telemetry-delegate'
            canary = outside / (leaf + '/keep.txt' if existing else 'keep.txt')
            before = marker(canary)
            if kind == 'staging':
                junction(build / 'module-headers', outside)
                text = configure(build, prefix, 'junction-' + name, refusal=True)
            else:
                configure(build, prefix, 'junction-' + name + '-configure')
                junction(prefix / 'include', outside)
                text = run([args.cmake, '--install', build], 'junction-' + name, expected_failure=True)
            after = check_marker(canary, before)
            if 'must stay inside' not in text or list(outside.rglob('tiny_delegate.hpp')):
                raise RuntimeError('Escaping junction was not refused before copying: ' + name)
            if not existing and (outside / leaf).exists():
                raise RuntimeError('Refused junction created an outside destination: ' + name)
            result[name] = {'refused': True, 'canary_sha256_before': before, 'canary_sha256_after': after}

    case = root / 'owned-leaf'
    build, prefix = case / '0', case / 'OFF'
    canaries = []
    for owner, link in ((build, build / 'module-headers/client'),
                        (prefix, prefix / 'include/telemetry-client')):
        target = owner / 'retained-client'
        canary = target / 'keep.txt'
        canaries.append((canary, marker(canary)))
        junction(link, target)
    configure(build, prefix, 'junction-leaf-configure')
    run([args.cmake, '--install', build], 'junction-leaf-install')
    result['owned-leaf'] = {'canaries': {p.relative_to(root).as_posix(): check_marker(p, h) for p, h in canaries}}
    if not (prefix / 'include/telemetry-client/client/DeviceClient.hpp').is_file():
        raise RuntimeError('Leaf junction was not replaced by installed headers')

    case = root / 'owner-root'
    actual_build, actual_prefix = case / 'actual-build', case / 'actual-prefix'
    actual_build.mkdir(parents=True)
    actual_prefix.mkdir()
    build, prefix = case / '0', case / 'OFF'
    junction(build, actual_build)
    junction(prefix, actual_prefix)
    configure(build, prefix, 'junction-root-configure')
    run([args.cmake, '--install', build], 'junction-root-install')
    if not (actual_prefix / 'include/telemetry-client/client/DeviceClient.hpp').is_file():
        raise RuntimeError('A junctioned owner root was not supported')
    result['owner-root'] = True

    # Invoke the exact production guard without accessing synthetic UNC paths.
    source = (ROOT / 'cmake/Modules.cmake').read_text(encoding='utf-8')
    guard = source.split('set(_telemetry_path_guard [=[\n', 1)[1].split('\n]=])', 1)[0]
    parent = root.as_posix()
    targets = [('\\??\\C:\\guard\\target', 'C:/guard/target'),
               ('\\\\?\\C:\\guard\\target', 'C:/guard/target'),
               ('\\??\\UNC\\server\\share\\target', '//server/share/target'),
               ('\\\\?\\UNC\\server\\share\\target', '//server/share/target'),
               ('../relative', root.parent.as_posix() + '/relative')]
    lines = [guard, 'set(WIN32 FALSE)']  # Filesystem behavior must use HOST, not target WIN32.
    for target, expected in targets:
        lines += ['_telemetry_windows_link_target([=[' + target + ']=] "' + parent + '" value)',
                  'if(NOT value STREQUAL "' + expected + '")',
                  'message(FATAL_ERROR "Unexpected Windows link conversion: ${value}")', 'endif()']
    lines += ['_telemetry_real_output("' + prefix.as_posix() + '" value)',
              'if(NOT value STREQUAL "' + actual_prefix.as_posix() + '")',
              'message(FATAL_ERROR "Windows host resolution was disabled by target WIN32")', 'endif()']
    script = root / 'link-semantics.cmake'
    script.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    run([args.cmake, '-P', script], 'junction-link-semantics')
    result['native_nt_unc_relative_and_host_semantics'] = True

    target = root / 'dangling-target'
    target.mkdir()
    dangling = root / 'dangling-link'
    junction(dangling, target)
    target.rmdir()  # Empty fixture directory only; leave the junction for refusal.
    cycle_a, cycle_b = root / 'cycle-a', root / 'cycle-b'
    cycle_a.mkdir()
    junction(cycle_b, cycle_a)
    cycle_a.rmdir()
    # Create the second edge only after recording both bounded fixture names.
    # Resolving this target is intentionally impossible; finally unlinks each edge.
    _winapi.CreateJunction(str(cycle_b), str(cycle_a))
    links.append(cycle_a)
    last = root / 'chain-target'
    last.mkdir()
    for index in range(65):
        link = root / ('chain-' + str(index))
        junction(link, last)
        last = link
    for name, statement in (
            ('dangling', '_telemetry_real_output("' + dangling.as_posix() + '/missing" value)'),
            ('cycle', '_telemetry_real_output("' + cycle_a.as_posix() + '/missing" value)'),
            ('bounded-chain', '_telemetry_real_output("' + last.as_posix() + '/missing" value)'),
            ('unsupported-nt', '_telemetry_windows_link_target([=[\\??\\Volume{unsupported}\\target]=] "' + parent + '" value)'),
            ('unsupported-device', '_telemetry_windows_link_target([=[\\Device\\HarddiskVolume1\\target]=] "' + parent + '" value)')):
        script = root / (name + '.cmake')
        script.write_text(guard + '\n' + statement + '\n', encoding='utf-8')
        text = run([args.cmake, '-P', script], 'junction-' + name, expected_failure=True)
        if 'must stay inside' not in text:
            raise RuntimeError('Windows link refusal did not identify containment: ' + name)
        result[name] = {'refused': True}
    return result


def check_containment(args, out, run, extra_config):
    """Exercise cleanup only in new cases below the verified external output root."""
    root = Path(tempfile.mkdtemp(prefix='containment-', dir=out))
    result = {'passed': False, 'root': root.relative_to(out).as_posix(), 'cases': {}}

    def paths(name):
        case = root / name
        case.mkdir()
        return case, case / 'build', case / 'prefix'

    def configure(build, prefix, label):
        return run([args.cmake, '-S', ROOT, '-B', build, '-G', 'Ninja',
                    '-DTELEMETRY_COMPONENTS=client', '-DCMAKE_CXX_COMPILER=' + args.cxx,
                    '-DCMAKE_INSTALL_PREFIX=' + str(prefix), *extra_config], label)

    def header(prefix):
        return prefix / 'include/telemetry-client/client/DeviceClient.hpp'

    # These controls need no compiled library, including with an ARM compiler.
    case, build, prefix = paths('fresh')
    configure(build, prefix, 'containment-fresh-configure')
    run([args.cmake, '--install', build], 'containment-fresh-install')
    if not header(prefix).is_file():
        raise RuntimeError('Fresh installation did not produce the client header')
    override = case / 'missing/relocated/prefix'
    run([args.cmake, '--install', build, '--prefix', override], 'containment-prefix-override')
    if not header(override).is_file():
        raise RuntimeError('Install-time prefix override did not produce the client header')
    result['cases']['fresh_and_prefix_override'] = True

    if os.name != 'nt':
        destdir = case / 'destdir'
        run([args.cmake, '--install', build], 'containment-destdir-install',
            extra_environment={'DESTDIR': str(destdir)})
        staged_prefix = destdir / prefix.relative_to(prefix.anchor)
        if not header(staged_prefix).is_file() or not header(prefix).is_file():
            raise RuntimeError('DESTDIR installation escaped its staging root')
        result['cases']['destdir'] = True
    else:
        result['cases']['destdir'] = 'unsupported by CMake for Windows drive-letter prefixes'
        result['junctions'] = check_windows_junctions(args, root, run, extra_config)

    probe = root / 'symlink-target'
    probe.mkdir()
    try:
        (root / 'symlink-probe').symlink_to(probe, target_is_directory=True)
    except OSError as error:
        if os.name != 'nt':
            raise
        result['symlinks'] = {'supported': False, 'reason': str(error)}
        result['install_paths'] = check_install_paths(args, out, root, run, extra_config, False)
        result['passed'] = True
        return result
    result['symlinks'] = {'supported': True}
    result['install_paths'] = check_install_paths(args, out, root, run, extra_config, True)

    # Existing intermediate links are allowed when their destination is owned.
    case, build, prefix = paths('internal-links')
    (build / 'owned').mkdir(parents=True)
    (build / 'module-headers').symlink_to(build / 'owned', target_is_directory=True)
    (prefix / 'owned').mkdir(parents=True)
    (prefix / 'include').symlink_to(prefix / 'owned', target_is_directory=True)
    configure(build, prefix, 'containment-internal-configure')
    run([args.cmake, '--install', build], 'containment-internal-install')
    if not header(prefix).is_file() or not (build / 'owned/client/client/DeviceClient.hpp').is_file():
        raise RuntimeError('Allowed internal links did not produce headers')
    result['cases']['internal_links'] = True

    # Removing a leaf link must unlink it, rather than erase its owned target.
    case, build, prefix = paths('leaf-links')
    canaries = []
    for owner, link in ((build, build / 'module-headers/client'),
                        (prefix, prefix / 'include/telemetry-client')):
        retained = owner / 'retained-client'
        retained.mkdir(parents=True)
        marker = retained / 'keep.txt'
        marker.write_bytes(b'Owned leaf-link target must survive cleanup.\n')
        canaries.append((marker, hashlib.sha256(marker.read_bytes()).hexdigest()))
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(retained, target_is_directory=True)
    configure(build, prefix, 'containment-leaf-configure')
    run([args.cmake, '--install', build], 'containment-leaf-install')
    for marker, expected in canaries:
        if not marker.is_file() or hashlib.sha256(marker.read_bytes()).hexdigest() != expected:
            raise RuntimeError('Cleanup followed an owned leaf symlink instead of removing the link')
    if not header(prefix).is_file() or (prefix / 'include/telemetry-client').is_symlink():
        raise RuntimeError('Leaf link was not replaced with installed headers')
    result['cases']['leaf_links'] = {'canaries_preserved': True}

    # The declared root itself may be a symlink; its real directory is the owner.
    case, build, prefix = paths('prefix-link')
    actual_prefix = case / 'actual-prefix'
    actual_prefix.mkdir()
    prefix.symlink_to(actual_prefix, target_is_directory=True)
    configure(build, prefix, 'containment-prefix-link-configure')
    run([args.cmake, '--install', build], 'containment-prefix-link-install')
    if not header(actual_prefix).is_file():
        raise RuntimeError('A symlinked installation prefix was not supported')
    result['cases']['prefix_link'] = True

    for kind in ('staging', 'installation'):
        for missing_leaf in (False, True):
            name = kind + ('-missing-leaf' if missing_leaf else '-existing-leaf')
            case, build, prefix = paths(name)
            outside = case / 'outside'
            outside.mkdir()
            owned_name = 'client' if kind == 'staging' else 'telemetry-client'
            marker = outside / ('keep.txt' if missing_leaf else owned_name + '/keep.txt')
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.write_bytes(b'Canary outside the declared build/install root.\n')
            before = hashlib.sha256(marker.read_bytes()).hexdigest()
            if kind == 'staging':
                build.mkdir()
                (build / 'module-headers').symlink_to(outside, target_is_directory=True)
                output = run([args.cmake, '-S', ROOT, '-B', build, '-G', 'Ninja',
                              '-DTELEMETRY_COMPONENTS=client', '-DCMAKE_CXX_COMPILER=' + args.cxx,
                              '-DCMAKE_INSTALL_PREFIX=' + str(prefix), *extra_config],
                             'containment-' + name, expected_failure=True)
            else:
                configure(build, prefix, 'containment-' + name + '-configure')
                prefix.mkdir()
                (prefix / 'include').symlink_to(outside, target_is_directory=True)
                output = run([args.cmake, '--install', build], 'containment-' + name,
                             expected_failure=True)
            after = hashlib.sha256(marker.read_bytes()).hexdigest() if marker.is_file() else None
            if 'must stay inside' not in output or after != before:
                raise RuntimeError(name + ': containment refusal did not preserve the canary')
            if missing_leaf and (outside / owned_name).exists():
                raise RuntimeError(name + ': refused cleanup wrote into the outside directory')
            result['cases'][name] = {'canary_sha256_before': before, 'canary_sha256_after': after,
                                     'refused': True, 'missing_leaf': missing_leaf}
    result['passed'] = True
    return result


def check_default_install(args, out, run, extra_config, flags):
    """The ordinary installation must satisfy the standalone Inspector consumer."""
    directory = out / 'default selection'
    build, prefix = directory / 'build tree', directory / 'initial install'
    # Deliberately do not supply TELEMETRY_COMPONENTS: this is the user default.
    run([args.cmake, '-S', ROOT, '-B', build, '-G', 'Ninja',
         '-DCMAKE_CXX_COMPILER=' + args.cxx, '-DCMAKE_CXX_FLAGS=' + flags,
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(prefix),
         *extra_config], 'default-configure')
    run([args.cmake, '--build', build, '-j2'], 'default-build')
    run([args.cmake, '--install', build], 'default-install')
    relocated = directory / 'relocated install'
    if relocated.exists():
        raise RuntimeError('Use a fresh build directory: ' + str(relocated))
    shutil.move(prefix, relocated)
    consumer = directory / 'consumer'
    consumer.mkdir(parents=True)
    # Reuse the real Resource/TEQ1 dispatch probe; additionally compile the
    # Inspector/Encoded client headers required by the standalone Qt application.
    probe = '#include <integration/inspector/InspectorModel.hpp>\n' \
            '#include <integration/workbench/WorkbenchModel.hpp>\n' \
            '#include <integration/encoded/Backend.hpp>\n' + (HERE / 'Probe.cpp').read_text()
    (consumer / 'main.cpp').write_text(probe, encoding='utf-8')
    components = ('inspector', 'workbench', 'read_many_workbench', 'encoded_backend', 'encoded_server', 'core', 'tcp_server')
    code = ['cmake_minimum_required(VERSION 3.20)', 'project(DefaultConsumer LANGUAGES CXX)',
            'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)',
            'find_package(Telemetry REQUIRED COMPONENTS ' + ' '.join(components) + ')',
            'add_executable(probe main.cpp)',
            'target_compile_definitions(probe PRIVATE MODULE_TCP_SERVER)',
            'target_link_libraries(probe PRIVATE ' +
            ' '.join('Telemetry::' + name for name in components) + ')']
    (consumer / 'CMakeLists.txt').write_text('\n'.join(code) + '\n', encoding='utf-8')
    downstream = directory / 'downstream'
    run([args.cmake, '-S', consumer, '-B', downstream, '-G', 'Ninja',
         '-DCMAKE_CXX_COMPILER=' + args.cxx, '-DCMAKE_CXX_FLAGS=' + flags,
         '-DTelemetry_DIR=' + str(relocated / 'lib/cmake/Telemetry'),
         *extra_config], 'default-downstream-configure')
    run([args.cmake, '--build', downstream, '-j2'], 'default-downstream-build')
    if not args.arm:
        run([downstream / ('probe.exe' if os.name == 'nt' else 'probe')], 'default-execute')
    commands = (downstream / 'compile_commands.json').read_text()
    if str(ROOT) in commands or str(build) in commands:
        raise RuntimeError('Default consumer can see source or build tree')
    return {'components': list(components), 'relocated_consumer': True,
            'paths_with_spaces': True, 'executed': not args.arm}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--arm', action='store_true', help='Offline ARM compilation and linking; never execute images')
    parser.add_argument('--modules', nargs='+', choices=MODULES, default=MODULES)
    args = parser.parse_args()
    if args.arm and args.sanitize:
        parser.error('ARM and runtime sanitizers are separate runs')
    out = args.build_dir.resolve()
    if out == ROOT or ROOT in out.parents:
        parser.error('Use an external build directory')
    out.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'commands': [], 'modules': {}}

    def snapshot():
        paths = [ROOT / 'CMakeLists.txt']
        for folder in ('lib', 'cmake', 'tests/modules'):
            paths += [path for path in (ROOT / folder).rglob('*')
                      if path.is_file() and '__pycache__' not in path.parts]
        return {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(paths)}

    report['source_sha256'] = snapshot()
    environment = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
                       UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

    def run(command, label, expected_failure=False, extra_environment=None):
        start = time.monotonic()
        command_environment = dict(environment, **(extra_environment or {}))
        completed = subprocess.run(list(map(str, command)), cwd=out, env=command_environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=600)
        (out / (label + '.log')).write_text(completed.stdout, encoding='utf-8')
        report['commands'].append({'name': label, 'command': list(map(str, command)),
                                   'exit': completed.returncode, 'seconds': time.monotonic() - start})
        if extra_environment:
            report['commands'][-1]['environment'] = extra_environment
        if (completed.returncode == 0) == expected_failure:
            raise RuntimeError(label + '\n' + completed.stdout)
        return completed.stdout

    flags = '-Wall -Wextra -Werror -pedantic-errors'
    extra_config = []
    if args.arm:
        flags += ' -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -fno-exceptions -fno-rtti -DTINY_DELEGATE_ENABLE_HEAP_FALLBACK=0'
        extra_config = ['-DCMAKE_SYSTEM_NAME=Generic', '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
                        '-DCMAKE_EXE_LINKER_FLAGS=-specs=nano.specs -specs=nosys.specs -nostartfiles -Wl,-e,main']
    if args.sanitize:
        flags += ' -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
    try:
        report['containment'] = check_containment(args, out, run, extra_config)
        report['default_install'] = check_default_install(args, out, run, extra_config, flags)
        for module in args.modules:
            directory = out / module
            build, prefix = directory / 'build', directory / 'install'
            run([args.cmake, '-S', ROOT, '-B', build, '-G', 'Ninja',
                 '-DTELEMETRY_COMPONENTS=' + module, '-DCMAKE_CXX_COMPILER=' + args.cxx,
                 '-DCMAKE_CXX_FLAGS=' + flags, '-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_INSTALL_PREFIX=' + str(prefix), *extra_config], module + '-configure')
            run([args.cmake, '--build', build, '-j2'], module + '-build')
            run([args.cmake, '--install', build], module + '-install')
            staged = build / 'module-headers' / module / 'removed-header.hpp'
            staged.write_text('#error obsolete staged header\n')
            installed = prefix / 'include' / ('telemetry-' + module) / 'removed-header.hpp'
            installed.write_text('#error obsolete installed header\n')
            run([args.cmake, '-S', ROOT, '-B', build], module + '-reconfigure')
            if staged.exists():
                raise RuntimeError(module + ': obsolete staged header survived reconfigure')
            run([args.cmake, '--install', build], module + '-upgrade-install')
            if installed.exists():
                raise RuntimeError(module + ': obsolete installed header survived upgrade')
            # A new prefix and absent original installation catch absolute export paths.
            relocated = directory / 'relocated'
            if relocated.exists():
                raise RuntimeError('Use a fresh build directory: ' + str(relocated))
            shutil.move(prefix, relocated)
            headers = {path.relative_to(parent).as_posix() for parent in (relocated / 'include').iterdir()
                       for path in parent.rglob('*') if path.is_file()}
            licenses = [('LICENSE', 'LICENSE')]
            for header, source, installed_license in (
                    ('delegate/tiny_delegate.hpp', 'lib/delegate/LICENSE', 'dependencies/delegate/LICENSE'),
                    ('magic_enum.hpp', 'lib/magic_enum/LICENSE', 'dependencies/magic_enum/LICENSE'),
                    ('boost/pfr.hpp', 'lib/boost_pfr/LICENSE_1_0.txt', 'dependencies/boost_pfr/LICENSE_1_0.txt')):
                if header in headers:
                    licenses.append((source, installed_license))
            for source, installed_license in licenses:
                installed_license = relocated / 'share/telemetry' / installed_license
                if not installed_license.is_file() or installed_license.read_bytes() != (ROOT / source).read_bytes():
                    raise RuntimeError(module + ': missing or changed installed license: ' + source)
            for forbidden in FORBIDDEN[module]:
                if forbidden in headers:
                    raise RuntimeError(module + ': installed forbidden header ' + forbidden)
            consumer = directory / 'consumer'
            consumer.mkdir(parents=True)
            shutil.copy2(HERE / 'Probe.cpp', consumer / 'main.cpp')
            code = ['cmake_minimum_required(VERSION 3.20)', 'project(Consumer LANGUAGES CXX)',
                    'set(CMAKE_EXPORT_COMPILE_COMMANDS ON)',
                    'find_package(Telemetry REQUIRED COMPONENTS ' + module + ')',
                    'add_executable(probe main.cpp)',
                    'target_compile_definitions(probe PRIVATE MODULE_' + module.upper() + ')',
                    'target_link_libraries(probe PRIVATE Telemetry::' + module + ')',
                    'include(CheckCXXSourceCompiles)',
                    'set(CMAKE_REQUIRED_LIBRARIES Telemetry::' + module + ')',
                    'set(CMAKE_REQUIRED_FLAGS "-std=c++20")',
                    'check_cxx_source_compiles("#include <cstddef>\nint main() { std::byte value{}; (void)value; }" MODULE_CONTROL)',
                    'if(NOT MODULE_CONTROL)', 'message(FATAL_ERROR "Positive compile control failed")', 'endif()']
            for index, header in enumerate(FORBIDDEN[module]):
                code += ['check_cxx_source_compiles("#include <' + header + '>\nint main() { return 0; }" LEAK_' + str(index) + ')',
                         'if(LEAK_' + str(index) + ')', 'message(FATAL_ERROR "Dependency leaked: ' + header + '")', 'endif()']
            (consumer / 'CMakeLists.txt').write_text('\n'.join(code) + '\n', encoding='utf-8')
            downstream = directory / 'downstream'
            run([args.cmake, '-S', consumer, '-B', downstream, '-G', 'Ninja',
                 '-DCMAKE_CXX_COMPILER=' + args.cxx, '-DCMAKE_CXX_FLAGS=' + flags,
                 '-DCMAKE_PREFIX_PATH=' + str(relocated), *extra_config], module + '-downstream-configure')
            run([args.cmake, '--build', downstream, '-j2'], module + '-downstream-build')
            if not args.arm:
                run([downstream / ('probe.exe' if os.name == 'nt' else 'probe')], module + '-execute')
            commands = (downstream / 'compile_commands.json').read_text()
            if str(ROOT) in commands or str(build) in commands:
                raise RuntimeError(module + ': consumer can see source or build tree')
            # Ensure a required compiled module is genuinely linked, not only parsed.
            if not args.arm and module in ('core', 'rpc_client', 'resource_protocol', 'tcp_server', 'batch_server'):
                compile_command = json.loads(commands)[0]
                import shlex
                command = shlex.split(compile_command['command'])
                object_path = directory / 'link-control.o'
                index = command.index('-o')
                command[index + 1] = str(object_path)
                run(command, module + '-link-control-compile')
                text = run([args.cxx, object_path, '-o', directory / 'missing-library'],
                           module + '-negative-link', expected_failure=True)
                expected = {'core': 'dispatchEncoded', 'rpc_client': 'RpcClient', 'resource_protocol': 'process',
                            'tcp_server': 'dispatchEncoded', 'batch_server': 'dispatchEncoded'}[module]
                if expected not in text:
                    raise RuntimeError('Link rejection did not identify the missing module: ' + text)
            report['modules'][module] = {'headers': len(headers), 'negative_includes': len(FORBIDDEN[module]),
                                          'relocated_consumer': True, 'executed': not args.arm}
            print(module + ': passed', flush=True)
        report['source_sha256_after'] = snapshot()
        if report['source_sha256_after'] != report['source_sha256']:
            raise RuntimeError('Source inputs changed during qualification')
        report['passed'] = True
    finally:
        (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
