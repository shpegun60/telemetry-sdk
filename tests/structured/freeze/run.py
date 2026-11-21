#!/usr/bin/env python3
"""Current software contract: API, immutable wire fixtures and dependency pins.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
ARM images are inspected offline. This runner never opens a device.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / 'qualification'))
from gates import counted_checks, forbidden_symbols  # noqa: E402

EXPECTED_CHECKS = 29  # 12 original + 13 traversal + 4 conversion conditions.
# Exact keys prevent deleting a ceiling from the manifest from silently
# deleting its compile-time check. The values remain in contract.json.
LIMIT_NAMES = (
    'maxTypeDepth', 'maxTypeCount', 'maxStructMembers', 'maxArrayElements',
    'maxDescriptorBytes', 'maxStringBytes', 'maxEnumEntriesTotal',
    'maxCatalogCountTotal', 'maxEndpointCountTotal', 'maxValueWireBytes',
    'maxExpandedValueNodes', 'maxGroupDepth', 'maxNameBytes', 'maxPathBytes',
)
DIAGNOSTICS = (
    'Structured field accepts name and bindings only; semantic metadata is not supported',
    'Command must return telemetry::CommandResult',
    'Command request must be an aggregate struct or void',
    'Service response must be an aggregate struct or void',
    'Structured endpoints must be noexcept',
    'Request must be by value or const lvalue reference, never volatile or a pointer',
    'Structured command accepts name and one binding only; semantic metadata is not supported',
    'Field readAs permits numeric conversion or the exact structural type',
    r'(?:no type named .Scalar.|.Scalar. .*does not name a type)',
    r'no matching function for call to .*service',
    r'no matching function for call to .*field',
)


def digest(payload):
    return hashlib.sha256(payload).hexdigest()


def git_blob(path):
    """Canonical tracked bytes, also checked against the current working copy."""
    blob = subprocess.check_output(['git', 'show', 'HEAD:' + path], cwd=ROOT)
    actual = (ROOT / path).read_bytes()
    if actual.replace(b'\r\n', b'\n') != blob.replace(b'\r\n', b'\n'):
        raise RuntimeError('Dependency differs from its tracked payload: ' + path)
    return blob


def verify_fixtures():
    contract = json.loads((HERE / 'contract.json').read_text(encoding='utf-8'))
    if contract['format_version'] != 1:
        raise RuntimeError('Unsupported contract manifest version')
    if set(contract['limits']) != set(LIMIT_NAMES):
        raise RuntimeError('The default v1 profile must specify all fourteen ceilings')
    if any(type(contract['limits'][name]) is not int or contract['limits'][name] <= 0
           for name in LIMIT_NAMES):
        raise RuntimeError('Ceilings must be positive integers')
    wire = contract['wire']
    if wire['byte_order'] != 'little-endian' or wire['fingerprint']['algorithm'] != 'FNV-1a-64':
        raise RuntimeError('Unsupported canonical byte order or fingerprint contract')
    pfr = contract['dependencies']['boost_pfr']
    prefix = 'lib/boost_pfr/include/'
    names = subprocess.check_output(['git', 'ls-tree', '-r', '--name-only', 'HEAD', prefix],
                                    cwd=ROOT, text=True).splitlines()
    if len(names) != pfr['headers']:
        raise RuntimeError('PFR header count changed')
    tree = hashlib.sha256()
    for name in sorted(names):
        tree.update(name[len(prefix):].encode('utf-8') + b'\0')
        tree.update(bytes.fromhex(digest(git_blob(name))))
    if tree.hexdigest() != pfr['tree_sha256']:
        raise RuntimeError('PFR header pin changed')
    if digest(git_blob('lib/boost_pfr/LICENSE_1_0.txt')) != pfr['license_sha256']:
        raise RuntimeError('PFR license pin changed')
    version = (ROOT / 'lib/boost_pfr/VERSION.md').read_text(encoding='utf-8')
    if pfr['tag'] not in version or pfr['commit'] not in version:
        raise RuntimeError('PFR provenance document differs from the pin')
    magic = contract['dependencies']['magic_enum']
    if digest(git_blob('lib/magic_enum/magic_enum.hpp')) != magic['header_sha256']:
        raise RuntimeError('magic_enum header pin changed')
    if digest(git_blob('lib/magic_enum/LICENSE')) != magic['license_sha256']:
        raise RuntimeError('magic_enum license pin changed')
    version = (ROOT / 'lib/magic_enum/README.md').read_text(encoding='utf-8')
    if magic['version'] not in version or magic['commit'] not in version:
        raise RuntimeError('magic_enum provenance document differs from the pin')
    for name, expected in contract['goldens'].items():
        payload = bytes.fromhex((ROOT / name).read_text(encoding='utf-8'))
        if len(payload) != expected['bytes'] or digest(payload) != expected['sha256']:
            raise RuntimeError('Frozen wire bytes changed: ' + name)
        magic = wire['values_magic'] if name.endswith('/values.hex') else wire['descriptor_magic']
        # v3.0 goldens carry the frozen minor; v3.1 goldens state their own.
        minor = expected.get('minor', wire['minor'])
        if minor not in (wire['minor'], contract['wire_v3_1']['minor']):
            raise RuntimeError('Golden states an unknown minor: ' + name)
        if payload[:4] != magic.encode('ascii') or struct.unpack_from('<HH', payload, 4) != (wire['major'], minor):
            raise RuntimeError('Manifest version/magic differs from frozen bytes: ' + name)
    return contract


def manifest_assertions(contract):
    """Keep the readable manifest tied to actual constants, never sizeof(C++ rows)."""
    wire = contract['wire']
    lines = ['#include <telemetry/Telemetry.hpp>',
             '#include <telemetry/descriptor/Format.hpp>',
             '#include <integration/telemetry/v3/ValuesFormat.hpp>']
    for key, name in (('major', 'binaryMajor'), ('minor', 'binaryMinor'),
                      ('descriptor_header_bytes', 'descriptorHeaderBytes'),
                      ('values_header_bytes', 'valuesHeaderBytes'),
                      ('record_header_bytes', 'recordHeaderBytes'), ('record_version', 'recordVersion')):
        owner = 'resource::telemetry::v3' if name == 'valuesHeaderBytes' else 'telemetry::descriptor'
        lines.append(f'static_assert({owner}::{name} == {int(wire[key])});')
    for key, name in (('record_kinds', 'RecordKind'), ('categories', 'Category'),
                      ('capabilities', 'Capability'), ('value_status', 'ValueStatus')):
        for entry, code in wire[key].items():
            owner = 'resource::telemetry::v3' if name == 'ValueStatus' else 'telemetry::descriptor'
            lines.append(f'static_assert(static_cast<unsigned>({owner}::{name}::{entry}) == {int(code)});')
    for key, name in (('type_kinds', 'TypeKind'), ('scalar_codes', 'ScalarCode')):
        for entry, code in wire[key].items():
            lines.append(f'static_assert(static_cast<unsigned>(telemetry::{name}::{entry}) == {int(code)});')
    for key, name in (('basis', 'fingerprintBasis'), ('prime', 'fingerprintPrime')):
        value = int(wire['fingerprint'][key], 16)
        lines.append(f'static_assert(telemetry::descriptor::{name} == {value}ULL);')
    lines += [f'static_assert(telemetry::structuredAbiRevision == {int(contract["structured_abi_revision"])});',
              f'static_assert(TELEMETRY_STRUCTURED_LOCAL_BYTES == {int(contract["default_local_object_bytes"])});',
              f'static_assert(TELEMETRY_TYPE_NAMES == {int(contract["default_type_names"])});']
    v31 = contract['wire_v3_1']
    lines += [f'static_assert(telemetry::descriptor::typeNamesMinor == {int(v31["minor"])});',
              f'static_assert(telemetry::descriptor::attributeHeaderBytes == {int(v31["attribute_header_bytes"])});']
    lines.append(f'static_assert(telemetry::descriptor::groupCategory == {int(v31["group_category"])});')
    for key, name in (('type_flags', 'TypeFlag'), ('record_flags', 'RecordFlag'), ('header_flags', 'HeaderFlag')):
        for entry, code in v31[key].items():
            lines.append(f'static_assert(static_cast<unsigned>(telemetry::descriptor::{name}::{entry}) == {int(code)});')
    tags = v31['attribute_tags']
    for key, first, last in (('sdk_advisory', 'sdkAdvisoryTagFirst', 'sdkAdvisoryTagLast'),
                             ('application', 'applicationTagFirst', 'applicationTagLast'),
                             ('critical', 'criticalTagFirst', 'criticalTagLast')):
        lines += [f'static_assert(telemetry::descriptor::{first} == 0x{tags[key][0]});',
                  f'static_assert(telemetry::descriptor::{last} == 0x{tags[key][1]});']
    lines += [f'static_assert(telemetry::descriptor::rangeTag == 0x{tags["range"]});',
              f'static_assert(telemetry::descriptor::unitTag == 0x{tags["unit"]});',
              f'static_assert(telemetry::descriptor::defaultTag == 0x{tags["default"]});']
    for tag in ('00', '80', 'c0', 'ff'):
        lines.append(f'static_assert(telemetry::descriptor::attributeTagClass(0x{tag}) == '
                     'telemetry::descriptor::AttributeTagClass::Reserved);')
    for name in LIMIT_NAMES:
        lines.append(f'static_assert(telemetry::Limits::{name} == {int(contract["limits"][name])});')
    for key, name in (('packed_id_bits', 'telemetry::PackedId'),
                      ('group_bits', 'telemetry::GroupId'), ('entry_bits', 'telemetry::EntryOffset'),
                      ('fingerprint_bits', 'std::uint64_t')):
        lines.append(f'static_assert(std::numeric_limits<{name}>::digits == {int(wire[key])});')
    enum = contract['dependencies']['magic_enum']
    major, minor, patch = map(int, enum['version'].split('.'))
    lines += [f'static_assert(MAGIC_ENUM_VERSION_MAJOR == {major});',
              f'static_assert(MAGIC_ENUM_VERSION_MINOR == {minor});',
              f'static_assert(MAGIC_ENUM_VERSION_PATCH == {patch});',
              'enum class FreezeEnum : std::int16_t { Value };',
              f'static_assert(magic_enum::customize::enum_range<FreezeEnum>::min == {int(enum["automatic_range"][0])});',
              f'static_assert(magic_enum::customize::enum_range<FreezeEnum>::max == {int(enum["automatic_range"][1])});']
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--arm', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--null-checks', action='store_true')
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    if args.arm and args.sanitize:
        parser.error('ARM qualification is offline and does not link host sanitizers')
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Keep clang++'s driver spelling instead of resolving it to clang.
    compiler = Path(shutil.which(args.cxx) or args.cxx).absolute()
    env = os.environ.copy()
    env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
    report = dict(execution='offline compile/link/inspect' if args.arm else 'host execution',
                  checks=0, commands=0, compile_rejections=0, ceiling_rejections=0,
                  frames={}, sections={}, images={})

    def run(command, label, diagnostic=None):
        command = list(map(str, command))
        start = time.perf_counter()
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                errors='replace', timeout=180)
        elapsed = time.perf_counter() - start
        (out / (label + '.log')).write_text(
            f'COMMAND {command!r}\nEXIT {result.returncode}\nSECONDS {elapsed:.6f}\n{result.stdout}',
            encoding='utf-8')
        if diagnostic is not None:
            if result.returncode == 0 or not re.search(diagnostic, result.stdout, re.S):
                raise RuntimeError(label + ': expected factory diagnostic missing\n' + result.stdout)
            report['ceiling_rejections' if label.startswith('ceiling-') else 'compile_rejections'] += 1
        elif result.returncode:
            raise RuntimeError(label + ': command failed\n' + result.stdout)
        else:
            report['commands'] += 1
        print(f'{label}: pass ({elapsed:.2f}s)', flush=True)
        return result.stdout

    report['contract'] = verify_fixtures()
    manifest_header = out / 'Manifest.hpp'
    manifest_header.write_text(manifest_assertions(report['contract']), encoding='utf-8')
    inputs = sorted(path for directory in (ROOT / 'lib', HERE)
                    for path in directory.rglob('*')
                    if path.is_file() and path.suffix in ('.h', '.hpp', '.cpp', '.pri', '.py', '.json'))
    inputs += [HERE.parent / 'traversal/Fixture.hpp', HERE.parent / 'qualification/gates.py']
    inputs += [ROOT / name for name in report['contract']['goldens']]
    inputs += [ROOT / name for name in ('lib/boost_pfr/LICENSE_1_0.txt',
               'lib/boost_pfr/VERSION.md', 'lib/magic_enum/LICENSE', 'lib/magic_enum/README.md')]

    def input_hashes():
        return {path.relative_to(ROOT).as_posix(): digest(path.read_bytes().replace(b'\r\n', b'\n'))
                for path in inputs}

    before = input_hashes()
    version = run([compiler, '--version'], 'compiler')
    report['compiler'] = version.splitlines()[0]
    report['compiler_sha256'] = digest(compiler.read_bytes())
    report['source_head'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    flags = [compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic-errors',
             '-fdiagnostics-color=never', '-ffunction-sections', '-fdata-sections',
             '-Ilib', '-Ilib/boost_pfr/include', '-Ilib/magic_enum']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if args.null_checks:
        flags += ['-fno-delete-null-pointer-checks']
    if args.arm:
        flags += ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard',
                  '-fno-exceptions', '-fno-rtti', '-fstack-usage', '-DFREEZE_ARM']
    report['flags'] = list(map(str, flags))
    suffix = '.exe' if os.name == 'nt' else ''
    link = ['-Wl,--gc-sections']
    if args.arm:
        link += ['-nostdlib', '-Wl,-e,main', '-lc', '-lgcc']
    elif 'clang' in version.lower():
        link += ['-fuse-ld=lld']

    for opt in ('O2', 'Os', 'Og'):
        obj = out / (opt + '.o')
        image = out / (opt + ('.elf' if args.arm else suffix))
        run(flags + ['-' + opt, '-include', manifest_header, '-c', HERE / 'Contract.cpp', '-o', obj], 'compile-' + opt)
        run(flags + ['-' + opt, obj, *link, '-o', image], 'link-' + opt)
        # Record the actual linked artifact in host and ARM roles. Compiler
        # provenance alone does not identify the executable that was checked.
        report['images'][opt] = dict(path=image.name, bytes=image.stat().st_size,
                                   sha256=digest(image.read_bytes()))
        if not args.arm:
            output = run([image], 'execute-' + opt)
            report['checks'] += counted_checks(output, opt, EXPECTED_CHECKS)
            continue
        def tool(name):
            return compiler.with_name('arm-none-eabi-' + name + suffix)
        symbols = run([tool('nm'), '-C', image], 'symbols-' + opt)
        if forbidden_symbols(symbols):
            raise RuntimeError(opt + ': allocation/formatting/Scalar symbols retained')
        sections = run([tool('size'), '-A', image], 'sections-' + opt)
        parsed = {name: int(size) for name, size in re.findall(r'^(\S+)\s+(\d+)\s+\d+\s*$', sections, re.M)}
        if any(name.startswith(('.init_array', '.preinit_array', '.ctors')) and size
               for name, size in parsed.items()):
            raise RuntimeError(opt + ': static metadata needs startup constructors')
        report['sections'][opt] = parsed
        frames = {}
        for line in obj.with_suffix('.su').read_text().splitlines():
            name, size, kind = line.split('\t')
            if kind != 'static' or int(size) > 512:
                raise RuntimeError(opt + ': large/dynamic individual frame: ' + line)
            frames[name] = int(size)
        if not any('main' in name for name in frames):
            raise RuntimeError(opt + ': main frame missing')
        report['frames'][opt] = frames
        run([tool('objdump'), '-drC', image], 'assembly-' + opt)
    for case, diagnostic in enumerate(DIAGNOSTICS, 1):
        syntax = [flag for flag in flags if flag != '-fstack-usage']
        run(syntax + ['-O2', '-fsyntax-only', '-DCASE=' + str(case), HERE / 'Negative.cpp'],
            'negative-' + str(case), diagnostic)
    # Change each expected ceiling independently. A static-assert failure
    # naming that exact member proves the corresponding guard is compiled.
    # No production header or tracked manifest is modified by these controls.
    control_source = out / 'LimitControl.cpp'
    control_source.write_text('// Compile-time ceiling controls.\n', encoding='utf-8')
    for name in LIMIT_NAMES:
        changed = {**report['contract'], 'limits': dict(report['contract']['limits'])}
        changed['limits'][name] += 1
        header = out / ('Ceiling-' + name + '.hpp')
        header.write_text(manifest_assertions(changed), encoding='utf-8')
        run(syntax + ['-O2', '-fsyntax-only', '-include', header, control_source],
            'ceiling-' + name, r'static assertion failed.*\b' + name + r'\b')
    if input_hashes() != before:
        raise RuntimeError('Qualification inputs changed during the run')
    for image in report['images'].values():
        if digest((out / image['path']).read_bytes()) != image['sha256']:
            raise RuntimeError('Linked artifact changed during qualification: ' + image['path'])
    report['input_hashes'] = before
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('Stage 15 software contract passed:',
          {key: report[key] for key in ('commands', 'checks', 'compile_rejections', 'ceiling_rejections')}, flush=True)


if __name__ == '__main__':
    main()
