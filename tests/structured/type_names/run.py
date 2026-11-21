#!/usr/bin/env python3
"""Descriptor v3.1 type names and attribute areas: goldens, decoders, seam, switch and matrix.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
Checks, in order: the independent v3.1 goldens and decoder table; the native
producer with names on (default) and off; the C++, JS and Python decoders on
one table with one verdict per case and one model dump per accepted case;
compile-time refusals; a link failure between names-on and names-off
objects; the configuration file and -D overrides; the cross-compiler byte
test of automatic names (every given compiler against cross.hex); the
cross-backend test (reported "not established" until a compiler implements
P2996 and the std::meta backend exists); and the ARM size of the decoder.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DESCRIPTOR = ROOT / 'tests/structured/descriptor'
sys.path.insert(0, str(DESCRIPTOR))
import golden  # noqa: E402
from parser import InvalidDescriptor, fingerprint, parse  # noqa: E402
from tools.telemetry_codegen.parser import endpoint_path, find_endpoint  # noqa: E402

COMPILE_FAILURES = {
    1: r'duplicateTypeName', 2: r'duplicateTypeName', 3: r'duplicateTypeName',
    4: r'abort', 5: r'abort', 6: r'Type name length is outside 1\.\.4096 bytes',
    7: r'TypeReflection names apply to enums and structs only',
    8: r'TypeReflection<T>::name must be typeName', 9: r'invalidDescriptorName',
    10: r'abort', 11: r'abort', 12: r'A local class has no stable automatic type name',
}
GCC_ONLY_FAILURES = {12}
COMPILE_CONTROLS = {101: ['-DTELEMETRY_TYPE_NAMES=0'], 102: [], 103: [], 104: [], 105: [],
                    106: ['-DTELEMETRY_TYPE_NAMES=0'], 107: []}
CROSS_NAMES = {'device::Mode', 'v2::Config', 'Board::Probe', 'Board::Channel', 'Пара',
               'device::Legacy', 'device::Level', 'Sealed', 'Global'}
# Rule 4 of plan 13.6: GCC spells an inline namespace, Clang does not.
INLINE_SPELLING = {'gcc': 'v2::Status', 'clang': 'probe::Status'}
ARM_FLAGS = ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard', '-fno-exceptions',
             '-fno-rtti', '-ffunction-sections', '-fdata-sections']


def sha(data):
    return hashlib.sha256(data).hexdigest()


def frozen(name):
    return bytes.fromhex((DESCRIPTOR / (name + '.hex')).read_text())


class Compiler:
    """A C++ compiler, optionally reached through WSL from a Windows host."""

    def __init__(self, label, command):
        self.label = label
        self.wsl = command.startswith('wsl:')
        self.command = command[4:] if self.wsl else command

    def path(self, value):
        if not self.wsl or not isinstance(value, Path):
            return str(value)
        posix = PurePosixPath(value.resolve().as_posix())
        drive = posix.parts[0].rstrip(':/').lower()
        return str(PurePosixPath('/mnt', drive, *posix.parts[1:]))

    def argv(self, arguments):
        argv = [self.command, *[self.path(a) for a in arguments]]
        return ['wsl', '-e', *argv] if self.wsl else argv

    def execute(self, program, *arguments):
        argv = [self.path(program), *[self.path(a) for a in arguments]]
        return ['wsl', '-e', *argv] if self.wsl else argv

    def tool(self, name):
        """Sibling binutils of an ARM compiler (objcopy, size, nm)."""
        directory = Path(shutil.which(self.command) or self.command).resolve().parent
        suffix = '.exe' if os.name == 'nt' else ''
        return directory / ('arm-none-eabi-' + name + suffix)


def dump_python(data):
    """The canonical model dump, identical in Decode.cpp and decode.mjs."""
    model = parse(data)
    hexed = lambda text: text.encode('utf-8').hex() if text else '-'
    lines = [f'minor {model["minor"]}', f'fingerprint {model["fingerprint"]:016x}']
    types = model['types']
    for item in types:
        kind = item['kind']
        if kind == 0:
            detail = ' V'
        elif kind == 1:
            detail = f' S {item["code"]}'
        elif kind == 2:
            mask = (1 << (8 * types[item['underlying']]['wire'])) - 1
            detail = f' E {item["underlying"]}' + ''.join(
                f' {value & mask:x}={hexed(name)}' for value, name in item['entries'])
        elif kind == 3:
            detail = ' M' + ''.join(f' {child}={hexed(name)}' for child, name in item['members'])
        else:
            detail = f' A {item["element"]} {item["count"]}'
        lines.append(f'type {item["id"]} {kind} {item["wire"]} {hexed(item.get("name"))}{detail}')
    for category, rows in enumerate(model['catalogs'], 1):
        for row in rows:
            lines.append(f'catalog {category} {row["group"]} {row["first"]} {row["count"]} {hexed(row["name"])}')
    for category, rows in enumerate(model['endpoints'], 1):
        for row in rows:
            lines.append(f'endpoint {category} {row["id"]} {row["type"]} {row.get("response", 0)} '
                         f'{row.get("capabilities", 0)} {hexed(row["name"])}')
    for index, group in enumerate(model['groups'] or []):
        parent = '-' if group['parent'] is None else group['parent']
        lines.append(f'group {index} {group["depth"]} {parent} {hexed(group["path"])}')
    for category, rows in enumerate(model['endpoints']):
        for row in rows:
            path = endpoint_path(model, category, row)
            lines.append(f'path {category + 1} {row["id"]} {hexed(path)}')
            found = find_endpoint(model, path)
            if model['groups'] is not None and (found is None or found[1] is not row):
                raise RuntimeError('Python find_endpoint misses ' + path)
    return lines


def split_cases(text, marker):
    """Map case name to (verdict line, dump lines) from a decoder's output."""
    cases, current = {}, None
    for line in text.splitlines():
        if line.startswith('case '):
            _, name, rest = line.split(' ', 2)
            current = cases[name] = [rest, []]
        elif line.startswith(marker) or line.startswith('FAIL'):
            continue
        elif current is not None:
            current[1].append(line)
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--node', default=os.environ.get('NODE', 'node'))
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--null-checks', action='store_true')
    parser.add_argument('--cross', action='append', default=[], metavar='LABEL=COMPILER',
                        help='another host compiler for the cross-compiler byte test; '
                             'a wsl: prefix runs it through WSL')
    parser.add_argument('--arm-cxx', help='ARM GCC: cross-compiler bytes, backend probe, decoder size')
    parser.add_argument('--base-ref', help='git revision whose lib/telemetry is the decoder size baseline')
    args = parser.parse_args()
    out = args.build_dir.resolve()
    # Outside the checkout, or under its ignored build/ directory as CI uses.
    ignored = ROOT / 'build'
    if out in (ROOT, ignored) or out in ROOT.parents or (
            ROOT in out.parents and ignored not in out.parents):
        parser.error('--build-dir must be outside the checkout or under its ignored build/')
    out.mkdir(parents=True, exist_ok=True)
    commands, counts = [], dict(passed=0, rejected=0)
    summary = dict(passed=False, sanitize=args.sanitize, null_checks=args.null_checks)
    environment = dict(os.environ)
    if args.sanitize:
        environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

    def run(command, label, diagnostic=None, cwd=ROOT):
        command = [str(part) for part in command]
        started = time.monotonic()
        result = subprocess.run(command, cwd=cwd, env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                errors='replace', timeout=600)
        log = f'COMMAND {command!r}\nEXIT {result.returncode}\n{result.stdout}'
        (out / (label + '.log')).write_text(log, encoding='utf-8')
        commands.append(dict(label=label, returncode=result.returncode,
                             seconds=round(time.monotonic() - started, 2)))
        if diagnostic is not None:
            if result.returncode == 0 or re.search(diagnostic, result.stdout, re.S) is None:
                raise RuntimeError(f'{label}: intended refusal missing\n{log}')
            counts['rejected'] += 1
        elif result.returncode:
            raise RuntimeError(f'{label} failed\n{log}')
        else:
            counts['passed'] += 1
        print(f'{label}: pass', flush=True)
        return result.stdout

    host = Compiler('host', args.cxx)
    suffix = '.exe' if os.name == 'nt' else ''
    include = ['-Ilib', '-Ilib/boost_pfr/include', '-Ilib/magic_enum']
    strict = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic-errors', '-fdiagnostics-color=never']
    if args.null_checks:
        strict.append('-fno-delete-null-pointer-checks')
    flags = strict + include + (['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                 '-fno-omit-frame-pointer'] if args.sanitize else ['-O2'])
    host_clang = 'clang' in run(host.argv(['--version']), 'compiler').lower()

    # 1. Independent goldens and the shared decoder table.
    for name in ('mixed', 'edge', 'empty', 'named', 'attrs', 'tree'):
        if golden.fixture(name) != frozen(name):
            raise RuntimeError('golden.py disagrees with ' + name + '.hex')
    table = json.loads((DESCRIPTOR / 'v31_cases.json').read_text(encoding='utf-8'))
    if table != json.loads(json.dumps(golden.table())):
        raise RuntimeError('golden.py disagrees with v31_cases.json')
    if frozen('named')[6] != 1 or frozen('mixed')[6] != 0 or len(frozen('named')) != 968 + 59:
        raise RuntimeError('named.hex is not mixed plus its three type names')
    attrs, named = frozen('attrs'), frozen('named')
    if fingerprint(attrs) != struct.unpack_from('<Q', attrs, 16)[0] or fingerprint(attrs) == fingerprint(named):
        raise RuntimeError('attrs.hex fingerprint does not cover its attribute bytes')
    print('goldens: pass', flush=True)

    # 2. Native producer, names on (default) and off.
    produced = {}
    for mode, extra in (('on', []), ('off', ['-DTELEMETRY_TYPE_NAMES=0'])):
        directory = out / ('producer-' + mode)
        directory.mkdir(exist_ok=True)
        program = out / ('Producer-' + mode + suffix)
        run(host.argv([*flags, *extra, HERE / 'Producer.cpp', '-o', program]), 'build-producer-' + mode)
        run(host.execute(program, directory), 'execute-producer-' + mode)
        produced[mode] = {path.stem: path.read_bytes() for path in directory.glob('*.bin')}
    for name in ('mixed', 'edge', 'empty'):
        if produced['off']['default-' + name] != frozen(name):
            raise RuntimeError('names off does not reproduce ' + name + '.hex')
        for mode in ('on', 'off'):
            if produced[mode]['strip-' + name] != frozen(name):
                raise RuntimeError('withoutTypeNames does not reproduce ' + name + '.hex (' + mode + ')')
    if produced['on']['default-mixed'] != named:
        raise RuntimeError('the default build does not emit named.hex')
    if produced['on']['default-empty'] != frozen('empty'):
        raise RuntimeError('a model without enums/structs must stay v3.0 with names on')
    edge = parse(produced['on']['default-edge'])
    edge_names = {item['id']: item.get('name') for item in edge['types'] if item['kind'] >= 2}
    scope = 'descriptor_fixture::'
    if edge['minor'] != 1 or edge_names != {12: 'fixture::Mode', 13: None, 14: 'fixture::State',
                                             15: 'fixture::MotorConfig', 16: scope + 'Empty', 17: None,
                                             18: None, 19: scope + 'Box', 20: scope + 'Signed',
                                             21: scope + 'Wide'}:
        raise RuntimeError('default edge names differ: ' + repr(edge_names))
    explicit = {mode: parse(produced[mode]['explicit']) for mode in ('on', 'off')}
    explicit_names = [item.get('name') for item in explicit['on']['types'][12:]]
    if (explicit_names != ['PairF²', None, 'PhaseSelector', 'names_fixture::Level', None]
            or explicit['off']['minor'] != 0):
        raise RuntimeError('explicit/removed names differ: ' + repr(explicit_names))
    summary['producer'] = dict(named_bytes=len(named), stripped_bytes=len(frozen('mixed')),
                               edge_named_bytes=len(produced['on']['default-edge']))

    # 3. Three decoders, one table: verdicts and model dumps.
    positives = [dict(name=case['name'], hex=case['hex'], equals=case['equals']) for case in table['positive']]
    positives += [dict(name='golden-' + name, hex=frozen(name).hex(), equals=None)
                  for name in ('mixed', 'edge', 'empty', 'named', 'attrs', 'tree')]
    positives += [dict(name='producer-explicit', hex=produced['on']['explicit'].hex(), equals=None),
                  dict(name='producer-edge-named', hex=produced['on']['default-edge'].hex(), equals=None)]
    cases = positives + table['negative']
    pack = out / 'cases.bin'
    pack.write_bytes(b''.join(struct.pack('<I', len(c['name'])) + c['name'].encode() +
                              struct.pack('<I', len(c['hex']) // 2) + bytes.fromhex(c['hex']) for c in cases))
    (out / 'cases.json').write_text(json.dumps(cases), encoding='utf-8')
    decoder = out / ('Decode' + suffix)
    run(host.argv([*strict, '-Ilib', *flags[len(strict) + len(include):], HERE / 'Decode.cpp', '-o', decoder]),
        'build-decoder')
    cpp = split_cases(run(host.execute(decoder, pack), 'execute-decoder-cpp'), 'C++ decoder')
    js = split_cases(run([args.node, HERE / 'decode.mjs', ROOT / 'web/telemetry.js', out / 'cases.json'],
                         'execute-decoder-js'), 'JS decoder')
    models = {}
    for case in positives:
        data = bytes.fromhex(case['hex'])
        expected = dump_python(data)
        if cpp[case['name']] != ['Ok', expected]:
            raise RuntimeError('C++ model differs from Python: ' + case['name'])
        if js[case['name']] != ['Ok', expected]:
            raise RuntimeError('JS model differs from Python: ' + case['name'])
        if expected[1] != f'fingerprint {fingerprint(data):016x}':
            raise RuntimeError('fingerprint is not FNV-1a over every byte: ' + case['name'])
        models[case['name']] = expected
    for case in positives:
        if case['equals']:
            reference = dump_python(frozen(case['equals']))
            if models[case['name']][2:] != reference[2:]:
                raise RuntimeError(case['name'] + ' decodes to another model than ' + case['equals'])
    for case in table['negative']:
        data = bytes.fromhex(case['hex'])
        try:
            parse(data)
            raise RuntimeError('Python accepted ' + case['name'])
        except InvalidDescriptor as error:
            if case['python'] not in str(error):
                raise RuntimeError(f'Python verdict for {case["name"]}: {error}')
        if cpp[case['name']][0] != case['verdict']:
            raise RuntimeError(f'C++ verdict for {case["name"]}: {cpp[case["name"]][0]}')
        verdict = js[case['name']][0]
        if not verdict.startswith('refused ') or case['js'] not in verdict:
            raise RuntimeError(f'JS verdict for {case["name"]}: {verdict}')
    summary['decoders'] = dict(positive=len(positives), negative=len(table['negative']),
                               languages=['C++', 'JS', 'Python'])
    print(f'decoder table: {len(positives)} accepted, {len(table["negative"])} refused, '
          'same verdict and model in C++, JS and Python', flush=True)

    # 4. Compile-time refusals and controls. Clang spells a local class without
    # its function (plan 13.6 rule 3), so only GCC-family compilers refuse it;
    # under Clang case 12 must build and name the class as if it were global.
    for case, diagnostic in COMPILE_FAILURES.items():
        if case in GCC_ONLY_FAILURES and host_clang:
            run(host.argv([*strict, *include, '-O2', f'-DCASE={case}', '-fsyntax-only', HERE / 'Negative.cpp']),
                f'clang-control-{case}')
            continue
        run(host.argv([*strict, *include, '-O2', f'-DCASE={case}', '-fsyntax-only', HERE / 'Negative.cpp']),
            f'negative-{case}', diagnostic)
    for case, extra in COMPILE_CONTROLS.items():
        run(host.argv([*strict, *include, '-O2', f'-DCASE={case}', *extra, '-fsyntax-only',
                       HERE / 'Negative.cpp']), f'control-{case}')

    # 5. Names on and names off objects must not link together.
    objects = {}
    for mode, extra in (('on', []), ('off', ['-DTELEMETRY_TYPE_NAMES=0'])):
        for stem, source in (('app', HERE / 'LinkApp.cpp'), ('abi', ROOT / 'lib/telemetry/abi/StructuredAbi.cpp'),
                             ('adapter', ROOT / 'lib/telemetry/model/Adapter.cpp')):
            objects[mode, stem] = out / f'link-{stem}-{mode}.o'
            run(host.argv([*flags, *extra, '-c', source, '-o', objects[mode, stem]]), f'compile-link-{stem}-{mode}')
    for app, library in (('on', 'on'), ('off', 'off'), ('on', 'off'), ('off', 'on')):
        program = out / f'link-{app}-{library}{suffix}'
        command = host.argv([*flags, objects[app, 'app'], objects[library, 'abi'], objects[library, 'adapter'],
                             '-o', program])
        if app == library:
            run(command, f'link-{app}-{library}')
            run(host.execute(program), f'execute-link-{app}-{library}')
        else:
            run(command, f'link-{app}-{library}', r'undefined (?:reference|symbol).*(?:requireStructuredAbi|readFieldEncoded)')

    # 6. The configuration file and -D definitions.
    def probe(label, extra, diagnostic=None):
        program = out / (label + suffix)
        text = run(host.argv([*flags, *extra, HERE / 'ConfigProbe.cpp', '-o', program]), 'build-' + label, diagnostic)
        return None if diagnostic else run(host.execute(program), 'execute-' + label).strip()

    config = '-I' + host.path(HERE / 'config')
    expectations = {
        'config-default': ([], 'TELEMETRY_TYPE_NAMES=1 TELEMETRY_STRUCTURED_LOCAL_BYTES=32 maxLocalObjectBytes=32 minor=1'),
        'config-project-file': ([config], 'TELEMETRY_TYPE_NAMES=0 TELEMETRY_STRUCTURED_LOCAL_BYTES=16 maxLocalObjectBytes=16 minor=0'),
        'config-define-names': (['-DTELEMETRY_TYPE_NAMES=0'], 'TELEMETRY_TYPE_NAMES=0 TELEMETRY_STRUCTURED_LOCAL_BYTES=32 maxLocalObjectBytes=32 minor=0'),
        'config-define-bytes': (['-DTELEMETRY_STRUCTURED_LOCAL_BYTES=8'], 'TELEMETRY_TYPE_NAMES=1 TELEMETRY_STRUCTURED_LOCAL_BYTES=8 maxLocalObjectBytes=8 minor=1'),
        'config-file-and-same-define': ([config, '-DTELEMETRY_TYPE_NAMES=0'], 'TELEMETRY_TYPE_NAMES=0 TELEMETRY_STRUCTURED_LOCAL_BYTES=16 maxLocalObjectBytes=16 minor=0'),
    }
    for label, (extra, expected) in expectations.items():
        if probe(label, extra) != expected:
            raise RuntimeError(label + ' selected another configuration')
    probe('config-file-and-other-define', [config, '-DTELEMETRY_TYPE_NAMES=1'], r'TELEMETRY_TYPE_NAMES.*redefined|redefin.*TELEMETRY_TYPE_NAMES')
    probe('config-invalid-value', ['-DTELEMETRY_TYPE_NAMES=2'], r'TELEMETRY_TYPE_NAMES must be 0 or 1')

    # 7. Cross-compiler bytes of automatic names, and the backend probe.
    expected_cross = bytes.fromhex((HERE / 'cross.hex').read_text())
    cross_model = parse(expected_cross)
    cross_names = {item.get('name') for item in cross_model['types'] if item['kind'] in (2, 3)}
    unnamed_structs = sum(1 for item in cross_model['types'] if item['kind'] == 3 and 'name' not in item)
    if cross_names - {None} != CROSS_NAMES or unnamed_structs != 3:
        raise RuntimeError('cross.hex names differ from the fixture specification: ' + repr(cross_names))
    compilers = [host] + [Compiler(*item.split('=', 1)) for item in args.cross]
    cross, aliases, backends, families = {}, {}, {}, {}
    for compiler in compilers:
        label = compiler.label
        flags_cross = strict + include + ['-O2']
        program = out / f'cross-{label}{suffix}'
        version = run(compiler.argv(['--version']), f'cross-{label}-version')
        families[label] = 'clang' if 'clang' in version.lower() else 'gcc'
        run(compiler.argv([*flags_cross, HERE / 'CrossCompiler.cpp', '-o', program]), f'cross-{label}-build')
        produced_bytes = out / f'cross-{label}.bin'
        run(compiler.execute(program, produced_bytes), f'cross-{label}-execute')
        cross[label] = produced_bytes.read_bytes()
        alias = out / f'alias-{label}{suffix}'
        run(compiler.argv([*flags_cross, HERE / 'AliasProbe.cpp', '-o', alias]), f'alias-{label}-build')
        aliases[label] = run(compiler.execute(alias), f'alias-{label}-execute').strip()
        backend = out / f'backend-{label}{suffix}'
        run(compiler.argv([*flags_cross, HERE / 'Backend.cpp', '-o', backend]), f'backend-{label}-build')
        backends[label] = run(compiler.execute(backend), f'backend-{label}-execute').strip().endswith('=1')
    if args.arm_cxx:
        arm = Compiler('arm', args.arm_cxx)
        run(arm.argv(['--version']), 'cross-arm-version')
        arm_flags = strict + include + ARM_FLAGS + ['-O2', '-DCROSS_ARM']
        for stem in ('CrossCompiler', 'AliasProbe', 'Backend'):
            run(arm.argv([*arm_flags, '-c', HERE / (stem + '.cpp'), '-o', out / f'arm-{stem}.o']), f'arm-{stem}-build')
        sections = {'names': ('CrossCompiler', '.descriptor.names'), 'alias': ('AliasProbe', '.probe.alias'),
                    'typedef': ('AliasProbe', '.probe.typedef'), 'inline': ('AliasProbe', '.probe.inline'),
                    'backend': ('Backend', '.probe.backend')}
        extracted = {}
        for key, (stem, section) in sections.items():
            target = out / f'arm-{key}.bin'
            run([arm.tool('objcopy'), '--dump-section', f'{section}={target}', out / f'arm-{stem}.o'],
                f'arm-extract-{key}')
            extracted[key] = target.read_bytes()
        cross['arm'] = extracted['names']
        aliases['arm'] = 'alias=[{}] typedef=[{}] inline=[{}]'.format(
            *(extracted[k].rstrip(b'\0').decode() for k in ('alias', 'typedef', 'inline')))
        families['arm'] = 'gcc'
        backends['arm'] = extracted['backend'] == b'\x01'
    for label, data in cross.items():
        if data != expected_cross:
            raise RuntimeError(f'{label} packs other automatic names than cross.hex')
    for label, text in aliases.items():
        if f'typedef=[probe::Typedef] inline=[{INLINE_SPELLING[families[label]]}]' not in text:
            raise RuntimeError(f'{label} ({families[label]}) spells the probes otherwise: {text}')
    summary['cross_compiler'] = {label: dict(bytes=len(data), sha256=sha(data), equal=True)
                                 for label, data in cross.items()}
    summary['alias_declared_unnamed_struct'] = aliases
    print('cross-compiler bytes equal cross.hex: ' + ', '.join(cross), flush=True)
    for label, text in aliases.items():
        print(f'alias-declared unnamed struct, {label}: {text}', flush=True)
    capable = [label for label, available in backends.items() if available]
    summary['cross_backend'] = dict(status='not established', reflection_capable=capable,
                                    reason=('no compiler given announces __cpp_impl_reflection and '
                                            '__cpp_lib_reflection' if not capable else
                                            'the std::meta backend is not written yet (later item R)'))
    print('cross-backend byte test: NOT ESTABLISHED - ' + summary['cross_backend']['reason'] +
          ' (compilers: ' + ', '.join(backends) + ')', flush=True)

    # 8. ARM code size of the decoder, against the base revision's lib/telemetry.
    if args.arm_cxx:
        arm = Compiler('arm', args.arm_cxx)
        trees = {'current': ROOT / 'lib'}
        if args.base_ref:
            base = out / 'base-tree'
            if base.exists():
                shutil.rmtree(base)
            archive = subprocess.run(['git', 'archive', '--format=tar', args.base_ref, 'lib/telemetry'],
                                     cwd=ROOT, stdout=subprocess.PIPE, check=True).stdout
            with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
                tar.extractall(base, filter='data')
            trees['base'] = base / 'lib'
        sizes = {}
        for tree, lib in trees.items():
            for opt in ('O2', 'Os'):
                elf = out / f'decoder-{tree}-{opt}.elf'
                run(arm.argv([*strict, '-I' + str(lib), *ARM_FLAGS, '-' + opt, HERE / 'DecoderSize.cpp',
                              '-nostdlib', '-Wl,-e,decoder_root', '-Wl,--gc-sections', '-lc', '-lgcc', '-o', elf]),
                    f'decoder-{tree}-{opt}')
                text = run([arm.tool('size'), '-A', elf], f'decoder-size-{tree}-{opt}')
                sections = {name: int(size) for name, size in re.findall(r'^(\.\w+)\s+(\d+)', text, re.M)}
                sizes[f'{tree}-{opt}'] = {name: sections.get(name, 0) for name in ('.text', '.rodata', '.data', '.bss')}
        summary['decoder_arm_size'] = dict(compiler=arm.command, base_ref=args.base_ref, sections=sizes)
        for opt in ('O2', 'Os'):
            current = sizes[f'current-{opt}']['.text']
            line = f'decoder .text {opt}: {current} B'
            if 'base' in trees:
                line += f' (base {sizes[f"base-{opt}"][".text"]} B, delta {current - sizes[f"base-{opt}"][".text"]:+d} B)'
            print(line, flush=True)

    summary.update(passed=True, counts=counts, commands=commands,
                   source_head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip())
    (out / 'summary.json').write_text(json.dumps(summary, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print('Type-name checks passed:', counts, flush=True)


if __name__ == '__main__':
    main()
