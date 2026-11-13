#!/usr/bin/env python3
"""Qualify runtime schema/value reads and staged writes against Native and SDK bytes.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT))
from tools.telemetry_codegen import parser as descriptor_parser


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def access_descriptor(descriptor):
    """Build a long Boolean DTO for deterministic protected-page access checks."""
    parsed = descriptor_parser.parse(descriptor)
    types = []
    for item in parsed['types'][:12]:
        pos = item['offset']
        size = 8 + struct.unpack_from('<I', descriptor, pos + 4)[0]
        types.append(descriptor[pos:pos + size])

    def record(payload):
        return struct.pack('<BBHI', 1, 1, 0, len(payload)) + payload

    types.append(record(struct.pack('<IB3xIII', 12, 4, 65536, 1, 65536)))
    members = struct.pack('<II', 1, 5) + b'first' + struct.pack('<II', 12, 4) + b'body'
    types.append(record(struct.pack('<IB3xII', 13, 3, 65537, 2) + members))
    body = b''.join(types)
    end = 64 + len(body)
    data = bytearray(struct.pack('<4s4HIQ10I', b'TDS3', 3, 0, 64, 0, end, 0,
                                 len(types), 0, 0, 0, 0, 0, 0, 64, end, end) + body)
    struct.pack_into('<Q', data, 16, descriptor_parser.fingerprint(data))
    descriptor_parser.parse(data)
    return data


def corpus(descriptor):
    """Rehash semantic mutations so the semantic parser, not just FNV, is tested."""
    parsed = descriptor_parser.parse(descriptor)
    cases = [('native', descriptor)]

    def add(name, data, valid=None, rehash=True):
        data = bytearray(data)
        if rehash and len(data) >= 24:
            struct.pack_into('<Q', data, 16, descriptor_parser.fingerprint(data))
        cases.append((name, bytes(data)))
        if valid is not None:
            try:
                descriptor_parser.parse(data)
                accepted = True
            except descriptor_parser.InvalidDescriptor:
                accepted = False
            if accepted != valid:
                raise RuntimeError('Corpus construction does not exercise intended case: ' + name)

    def change(name, offset, raw, valid=False):
        data = bytearray(descriptor)
        data[offset:offset + len(raw)] = raw
        add(name, data, valid)

    change('header-flags', 10, b'\x01\0')
    change('type-count-overflow', 24, struct.pack('<I', 0xffffffff))
    for label, positions in [('endpoint-count-wrap', (32, 40)), ('catalog-count-wrap', (28, 36))]:
        data = bytearray(descriptor)
        struct.pack_into('<I', data, positions[0], 0xffffffff)
        struct.pack_into('<I', data, positions[1], 1)
        add(label, data, False)
    for label, positions in [('endpoint-count-all-max', (32, 40, 48)), ('catalog-count-all-max', (28, 36, 44))]:
        data = bytearray(descriptor)
        for pos in positions:
            struct.pack_into('<I', data, pos, 0xffffffff)
        add(label, data, False)
    change('type-section-offset', 52, struct.pack('<I', 63))
    change('catalog-section-offset', 56, struct.pack('<I', len(descriptor) + 1))
    change('record-version', 65, b'\x02')
    change('record-reserved', 66, b'\x01')
    change('type-reserved', 64 + 8 + 5, b'\x01')
    change('scalar-code', parsed['types'][5]['offset'] + 20, b'\x01')
    compounds = [item for item in parsed['types'] if item['kind'] >= 2]
    aggregate = next(item for item in compounds if item['kind'] == 3 and len(item['members']) > 3)
    array = next(item for item in compounds if item['kind'] == 4)
    enumeration = next(item for item in compounds if item['kind'] == 2 and len(item['entries']) >= 2)
    offset = aggregate['offset']
    change('struct-wire-size', offset + 16, struct.pack('<I', aggregate['wire'] + 1))
    change('struct-forward-reference', offset + 24, struct.pack('<I', aggregate['id']))
    change('struct-void-member', offset + 24, struct.pack('<I', 0))
    change('struct-member-count', offset + 20, struct.pack('<I', 257))
    change('array-self-reference', array['offset'] + 20, struct.pack('<I', array['id']))
    change('array-count-overflow', array['offset'] + 24, struct.pack('<I', 65537))
    change('enum-float-underlying', enumeration['offset'] + 20, struct.pack('<I', 10))
    width = parsed['types'][enumeration['underlying']]['wire']
    first = enumeration['offset'] + 28
    first_name_size = struct.unpack_from('<I', descriptor, first + width)[0]
    second = first + width + 4 + first_name_size
    change('enum-duplicate-code', second, descriptor[first:first + width])
    change('enum-name-invalid-utf8', first + width + 4, b'\xff')
    change('enum-name-nul', first + width + 4, b'\0')
    change('member-name-invalid-utf8', offset + 32, b'\xc0')
    # Two equal-length struct member names become duplicates without changing
    # any record length. Packet's u8 and i8 are one such pair.
    data = bytearray(descriptor)
    cursor = offset + 24
    names = {}
    for _, name in aggregate['members']:
        size = struct.unpack_from('<I', data, cursor + 4)[0]
        if size in names:
            data[cursor + 8:cursor + 8 + size] = names[size]
            add('duplicate-member-name', data, False)
            break
        names[size] = bytes(data[cursor + 8:cursor + 8 + size])
        cursor += 8 + size
    catalog = parsed['catalogs'][0][0]['offset']
    endpoint = parsed['endpoints'][0][0]['offset']
    change('catalog-reserved', catalog + 9, b'\x01')
    change('catalog-first-gap', catalog + 16, struct.pack('<I', 1))
    change('endpoint-id', endpoint + 8, struct.pack('<I', 99))
    change('endpoint-type', endpoint + 12, struct.pack('<I', 0xffffffff))
    change('endpoint-capabilities', endpoint + 16, b'\x02')
    change('endpoint-reserved', endpoint + 17, b'\x01')
    change('endpoint-name-utf8', endpoint + 24, b'\xff')
    bad_hash = bytearray(descriptor)
    bad_hash[16] ^= 1
    add('wrong-fingerprint-not-rehashed', bad_hash, False, False)

    primitives = []
    for item in parsed['types'][:12]:
        pos = item['offset']
        length = 8 + struct.unpack_from('<I', descriptor, pos + 4)[0]
        primitives.append(descriptor[pos:pos + length])

    def record(payload):
        return struct.pack('<BBHI', 1, 1, 0, len(payload)) + payload

    def structure(type_id, child, name=b'x'):
        return record(struct.pack('<IB3xIII', type_id, 3, 1, 1, child) + struct.pack('<I', len(name)) + name)

    def build(types):
        body = b''.join(types)
        end = 64 + len(body)
        return struct.pack('<4s4HIQ10I', b'TDS3', 3, 0, 64, 0, end, 0,
                           len(types), 0, 0, 0, 0, 0, 0, 64, end, end) + body

    chain = list(primitives)
    for depth in range(1, 34):
        chain.append(structure(len(chain), 2 if depth == 1 else len(chain) - 1))
        if depth in (31, 32, 33):
            add('depth-' + str(depth), build(chain), depth <= 32)
    punctuated = list(primitives) + [structure(12, 2, b'a.b')]
    add('punctuated-member-name', build(punctuated), True)
    add('utf8-member-name', build(list(primitives) + [structure(12, 2, 'темп'.encode())]), True)
    empty_struct = record(struct.pack('<IB3xII', 12, 3, 0, 0))

    def array_record(type_id, child, count, wire):
        return record(struct.pack('<IB3xIII', type_id, 4, wire, child, count))

    for multiplier in (3, 4):
        add('expanded-nodes-' + str(multiplier), build(list(primitives) + [empty_struct,
            array_record(13, 12, 65536, 0), array_record(14, 13, multiplier, 0)]), multiplier == 3)
    for multiplier in (2, 3):
        add('wire-limit-' + str(multiplier), build(list(primitives) + [
            array_record(12, 8, 65536, 524288),
            array_record(13, 12, multiplier, 524288 * multiplier)]), multiplier == 2)
    for count in (256, 257):
        members = b''.join(struct.pack('<II', 2, 4) + ('m%03d' % index).encode() for index in range(count))
        add('member-limit-' + str(count), build(list(primitives) + [
            record(struct.pack('<IB3xII', 12, 3, count, count) + members)]), count == 256)

    def named_record(kind, prefix, name):
        payload = prefix + struct.pack('<I', len(name)) + name
        return struct.pack('<BBHI', kind, 1, 0, len(payload)) + payload

    def field_catalog(group, first, count, name):
        return named_record(2, struct.pack('<B3xIII', 1, group, first, count), name)

    def field_endpoint(packed, name):
        return named_record(3, struct.pack('<IIB3x', packed, 2, 1), name)

    def endpoint_descriptor(catalogs, endpoints):
        types = b''.join(primitives)
        cat_start = 64 + len(types)
        ep_start = cat_start + sum(map(len, catalogs))
        total = ep_start + sum(map(len, endpoints))
        return struct.pack('<4s4HIQ10I', b'TDS3', 3, 0, 64, 0, total, 0, 12,
                           len(catalogs), len(endpoints), 0, 0, 0, 0, 64, cat_start, ep_start) + types + b''.join(catalogs + endpoints)

    add('endpoint-name-positive', endpoint_descriptor([field_catalog(0, 0, 2, b'g')],
        [field_endpoint(0, b'a'), field_endpoint(1, b'b')]), True)
    add('endpoint-name-duplicate', endpoint_descriptor([field_catalog(0, 0, 2, b'g')],
        [field_endpoint(0, b'a'), field_endpoint(1, b'a')]), False)
    add('catalog-name-duplicate', endpoint_descriptor([field_catalog(0, 0, 1, b'g'), field_catalog(1, 1, 1, b'g')],
        [field_endpoint(0, b'a'), field_endpoint(65536, b'a')]), False)
    for name in ('mixed', 'edge', 'empty'):
        path = ROOT / 'tests/structured/descriptor' / (name + '.hex')
        add('golden-' + name, bytes.fromhex(path.read_text()), True)
    # Raw record and section truncations remain independently fingerprinted.
    for size in sorted(set([0, 1, 15, 23, 24, 63, 64, len(descriptor) - 1] +
                           [item['offset'] + 1 for item in parsed['types']])):
        add('truncated-' + str(size), descriptor[:size], False)
    rng = random.Random(0x54445333)
    for index in range(384):
        data = bytearray(descriptor)
        for _ in range(1 + index % 4):
            pos = rng.randrange(len(data))
            data[pos] ^= rng.randrange(1, 256)
        add('mutated-' + str(index), data)
    for index in range(128):
        add('raw-' + str(index), rng.randbytes(rng.randrange(0, 2048)))
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--big-endian', action='store_true',
                        help='also execute the reader and writer on s390x through QEMU')
    args = parser.parse_args()
    out = args.build_dir.resolve()
    if out == ROOT or ROOT in out.parents or out in ROOT.parents:
        parser.error('--build-dir must be external and cannot contain the checkout')
    out.mkdir(parents=True, exist_ok=True)
    compiler = Path(shutil.which(args.cxx) or args.cxx).absolute()
    commands = []
    outputs = {}
    environment = dict(os.environ)
    if args.sanitize:
        environment.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')

    def run(label, command, expected=0, marker=None):
        command = list(map(str, command))
        started = time.monotonic()
        result = subprocess.run(command, cwd=ROOT, env=environment, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                encoding='utf-8', errors='replace', timeout=600)
        log = out / (label + '.log')
        log.write_text('COMMAND ' + repr(command) + '\nEXIT ' + str(result.returncode) + '\n' + result.stdout, encoding='utf-8')
        commands.append(dict(label=label, command=command, returncode=result.returncode,
                             expected=expected, seconds=time.monotonic() - started, log=log.name, sha256=digest(log)))
        if result.returncode != expected or (marker and marker not in result.stdout):
            raise RuntimeError(label + ' failed\n' + result.stdout)
        print(label + ': pass', flush=True)
        return result.stdout

    base_flags = [compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic-errors', '-Ilib']
    flags = base_flags + (['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer'] if args.sanitize else ['-O2'])
    native_flags = flags + ['-Ilib/boost_pfr/include', '-Ilib/magic_enum']
    run('compiler', [compiler, '--version'])
    run('python', [sys.executable, '--version'])
    dependency_texts = [run('runtime-dependencies', flags + ['-MM', HERE / 'Probe.cpp']),
                        run('native-dependencies', native_flags + ['-MM', HERE / 'Producer.cpp'])]
    if re.search(r'(?:^|[/\\])(?:resource|client|integration|reflection|boost_pfr|magic_enum)[/\\]', dependency_texts[0]):
        raise RuntimeError('Standalone runtime pulled in Native reflection, Resource, Client or Integration')
    sources = set(HERE.glob('*')) | set((ROOT / 'examples/runtime_schema').glob('*'))
    sources.update((ROOT / 'lib/telemetry/runtime').rglob('*.hpp'))
    sources.update((ROOT / 'tools/telemetry_codegen').rglob('*.py'))
    sources.update(ROOT / name for name in ('lib/telemetry/model/Adapter.cpp', 'lib/telemetry/abi/StructuredAbi.cpp'))
    sources.update(ROOT / 'tests/structured/descriptor' / (name + '.hex') for name in ('mixed', 'edge', 'empty'))
    for text in dependency_texts:
        for part in text.replace('\\\n', ' ').split()[1:]:
            path = (ROOT / part).resolve()
            if path.is_file():
                sources.add(path)

    def snapshot():
        return {path.relative_to(ROOT).as_posix(): digest(path) for path in sorted(sources) if path.is_file()}

    before = snapshot()
    native = out / 'native'
    native.mkdir(exist_ok=True)
    suffix = '.exe' if os.name == 'nt' else ''

    def build(stem, sources, extra=(), native=False, plain=False):
        binary = out / (stem + suffix)
        selected_flags = base_flags + ['-O2'] if plain else (native_flags if native else flags)
        run('build-' + stem, selected_flags + list(extra) + list(sources) + ['-o', binary])
        outputs[binary.name] = digest(binary)
        return binary

    producer = build('Producer', [HERE / 'Producer.cpp', ROOT / 'lib/telemetry/model/Adapter.cpp', ROOT / 'lib/telemetry/abi/StructuredAbi.cpp'], native=True)
    execution_counts = {}
    text = run('execute-Producer', [producer, native])
    execution_counts['native'] = int(re.search(r': (\d+) checks, 0 failures', text)[1])
    if execution_counts['native'] < 15:
        raise RuntimeError('Native producer count below floor')
    cases = corpus((native / 'descriptor.bin').read_bytes())
    for name, data in cases:
        if name == 'punctuated-member-name':
            (native / 'punctuated.bin').write_bytes(data)
    for stem in ('Check', 'NoHeap'):
        binary = build(stem, [HERE / (stem + '.cpp')])
        text = run('execute-' + stem, [binary, native])
        match = re.search(r': (\d+) checks, 0 failures', text)
        if not match or int(match[1]) < 381:
            raise RuntimeError(stem + ' count below floor')
        execution_counts[stem] = int(match[1])
    control = build('NoHeapControl', [HERE / 'NoHeap.cpp'], ['-DRUNTIME_HEAP_CONTROL'])
    run('execute-NoHeapControl', [control, native], 86, 'runtime heap allocation refused')
    for stem, source, extra in (('Writer', 'Writer.cpp', []),
                                ('WriterNoHeap', 'NoHeap.cpp', ['-DRUNTIME_WRITER'])):
        binary = build(stem, [HERE / source], extra)
        text = run('execute-' + stem, [binary, native])
        match = re.search(r': (\d+) checks, 0 failures', text)
        if not match or int(match[1]) < 1587:
            raise RuntimeError(stem + ' count below floor')
        execution_counts[stem] = int(match[1])
    writer_native = build('WriterNative', [HERE / 'WriterNative.cpp', ROOT / 'lib/telemetry/model/Adapter.cpp',
                                          ROOT / 'lib/telemetry/abi/StructuredAbi.cpp'], native=True)
    text = run('execute-WriterNative', [writer_native, native / 'writer.bin'])
    match = re.search(r': (\d+) checks, 0 failures', text)
    if not match or int(match[1]) < 201:
        raise RuntimeError('WriterNative count below floor')
    execution_counts['WriterNative'] = int(match[1])
    writer_lifetime = build('WriterLifetime', [HERE / 'WriterLifetime.cpp'])
    text = run('execute-WriterLifetime', [writer_lifetime, native / 'descriptor.bin', native / 'payload.bin'])
    match = re.search(r': (\d+) checks, 0 failures', text)
    if not match or int(match[1]) < 109:
        raise RuntimeError('WriterLifetime count below floor')
    execution_counts['WriterLifetime'] = int(match[1])
    lifetime = build('Lifetime', [HERE / 'Lifetime.cpp'])
    text = run('execute-Lifetime', [lifetime, native / 'descriptor.bin', native / 'payload.bin'])
    match = re.search(r': (\d+) checks, 0 failures', text)
    if not match or int(match[1]) < 117:
        raise RuntimeError('Lifetime count below floor')
    execution_counts['Lifetime'] = int(match[1])
    access_checks = dict(executed=False, reason='requires Linux protected pages and child-process signals')
    if sys.platform.startswith('linux'):
        fixture = native / 'access-descriptor.bin'
        fixture.write_bytes(access_descriptor((native / 'descriptor.bin').read_bytes()))
        # The child-process controls must receive SIGSEGV directly, without
        # AddressSanitizer intercepting their deliberately inaccessible pages.
        access = build('AccessContracts', [HERE / 'AccessContracts.cpp'], plain=True)
        text = run('execute-AccessContracts', [access, fixture], marker='two SIGSEGV negative controls')
        match = re.search(r': (\d+) checks, 0 failures', text)
        if not match or int(match[1]) < 56:
            raise RuntimeError('AccessContracts count below floor')
        execution_counts['AccessContracts'] = int(match[1])
        access_checks = dict(executed=True, sanitized=False, negative_controls=2)
        writer_access = build('WriterAccess', [HERE / 'WriterAccess.cpp'], plain=True)
        text = run('execute-WriterAccess', [writer_access, fixture],
                   marker='three SIGSEGV negative controls')
        match = re.search(r': (\d+) checks, 0 failures', text)
        if not match or int(match[1]) < 46:
            raise RuntimeError('WriterAccess count below floor')
        execution_counts['WriterAccess'] = int(match[1])
        access_checks['writer_negative_controls'] = 3
    generated = out / 'generated'
    run('generate-sdk', [sys.executable, '-B', '-m', 'tools.telemetry_codegen', 'generate', native / 'descriptor.bin', '--targets', 'cpp', '--namespace', 'runtime_api', '--output', generated])
    parsed = descriptor_parser.parse((native / 'descriptor.bin').read_bytes())
    root = parsed['endpoints'][0][0]['type']
    sdk = out / 'Sdk.cpp'
    sdk.write_text('''#include <runtime_api/Codec.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <cstdio>
int main(int argc,char**argv) {
 if(argc!=3)return 2; std::array<std::byte,4096> bytes{};
 auto*f=std::fopen(argv[1],"rb");if(!f)return 2;
 auto n=std::fread(bytes.data(),1,bytes.size(),f);std::fclose(f);
 runtime_api::detail::Type_ROOT value{}; std::array<std::byte,4096> output{};
 if(runtime_api::codec::decode(std::span<const std::byte>{bytes.data(),n},value)!=runtime_api::codec::Status::Ok)return 1;
 const bool variant=argv[2][0]=='1';
 if(value.u64!=UINT64_MAX || value.i64!=INT64_MIN || value.nested.code!=0x1234 ||
    value.rows[1].bins[2]!=-3 || value.matrix[0][1]!=0xaabbccddu || value.matrix[1][0]!=42 ||
    std::bit_cast<std::uint32_t>(value.f32)!=(variant?0x7fc12345u:0x80000000u) ||
    value.f64!=(variant?-std::numeric_limits<double>::infinity():std::numeric_limits<double>::infinity()) ||
    value.flag==variant || static_cast<std::int16_t>(value.nested.mode)!=-7 ||
    static_cast<std::int16_t>(value.rows[1].mode)!=-2 || static_cast<std::uint64_t>(value.wide)!=UINT64_MAX)return 1;
 if(runtime_api::codec::encode(value,std::span<std::byte>{output.data(),n})!=runtime_api::codec::Status::Ok)return 1;
 for(std::size_t i=0;i<n;++i)if(bytes[i]!=output[i])return 1;
 std::printf("generated SDK: %zu byte comparisons, 0 failures\\n",n); }
'''.replace('Type_ROOT', 'Type_' + str(root)), encoding='utf-8')
    sdk_binary = build('Sdk', [sdk], ['-I' + str(generated / 'cpp'), '-Wno-misleading-indentation'])
    for index, filename in enumerate(('payload.bin', 'variants.bin')):
        run('execute-Sdk-' + filename, [sdk_binary, native / filename, str(index)])
    writer_sdk = out / 'WriterSdk.cpp'
    writer_sdk.write_text('''#include <runtime_api/Codec.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
int main(int argc,char**argv) {
 if(argc!=2)return 2; std::array<std::byte,4096> bytes{}, output{};
 auto*f=std::fopen(argv[1],"rb");if(!f)return 2;
 auto n=std::fread(bytes.data(),1,bytes.size(),f);std::fclose(f);
 runtime_api::detail::Type_ROOT value{};
 if(runtime_api::codec::decode(std::span<const std::byte>{bytes.data(),n},value)!=runtime_api::codec::Status::Ok)return 1;
 if(value.flag || value.u32!=115200 || std::bit_cast<std::uint32_t>(value.f32)!=0x7fc12345u ||
    static_cast<std::int16_t>(value.nested.mode)!=-2 || value.u64!=UINT64_MAX || value.i64!=INT64_MIN)return 1;
 if(runtime_api::codec::encode(value,std::span<std::byte>{output.data(),n})!=runtime_api::codec::Status::Ok)return 1;
 for(std::size_t i=0;i<n;++i)if(bytes[i]!=output[i])return 1;
 std::printf("generated writer SDK: %zu byte comparisons, 0 failures\\n",n); }
'''.replace('Type_ROOT', 'Type_' + str(root)), encoding='utf-8')
    writer_sdk_binary = build('WriterSdk', [writer_sdk], ['-I' + str(generated / 'cpp'), '-Wno-misleading-indentation'])
    run('execute-WriterSdk', [writer_sdk_binary, native / 'writer.bin'])
    parser_binary = build('Parser', [HERE / 'Parser.cpp'])
    packed = out / 'corpus.bin'
    packed.write_bytes(b''.join(struct.pack('<I', len(data)) + data for _, data in cases))
    native_results = run('differential-parser', [parser_binary, packed]).splitlines()
    if len(native_results) != len(cases):
        raise RuntimeError('Differential result count differs')
    records = []
    for (name, data), actual in zip(cases, native_results):
        try:
            descriptor_parser.parse(data)
            expected, reason = True, ''
        except descriptor_parser.InvalidDescriptor as error:
            expected, reason = False, str(error)
        if actual != str(int(expected)):
            raise RuntimeError('Python/C++ parser disagreement: ' + name + ' expected ' + str(expected))
        records.append(dict(name=name, accepted=expected, reason=reason, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    (out / 'corpus.json').write_text(json.dumps(records, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    example = build('Example', [ROOT / 'examples/runtime_schema/main.cpp'])
    run('execute-Example', [example, native / 'descriptor.bin', native / 'payload.bin'])
    writer_example = build('WriterExample', [ROOT / 'examples/runtime_schema/Writer.cpp'])
    run('execute-WriterExample', [writer_example, native / 'descriptor.bin', native / 'payload.bin',
                                  out / 'example-edited.bin'])
    endian_checks = dict(executed=False)
    if args.big_endian:
        cross = shutil.which('s390x-linux-gnu-g++')
        emulator = shutil.which('qemu-s390x')
        if not cross or not emulator:
            raise RuntimeError('--big-endian requires s390x-linux-gnu-g++ and qemu-s390x')
        run('compiler-big', [cross, '--version'])
        run('emulator-big', [emulator, '--version'])
        writer_digest = digest(native / 'writer.bin')
        endian_checks = dict(executed=True, targets={}, writer_sha256=writer_digest)
        for stem, source, extra in (('CheckBig', 'Check.cpp', []),
                                    ('WriterBig', 'Writer.cpp', ['-DRUNTIME_EXPECT_BIG_ENDIAN']),
                                    ('WriterNoHeapBig', 'NoHeap.cpp', ['-DRUNTIME_WRITER', '-DRUNTIME_EXPECT_BIG_ENDIAN'])):
            binary = out / stem
            run('build-' + stem, [cross, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                                 '-pedantic-errors', '-Ilib'] + extra + [HERE / source, '-o', binary])
            outputs[binary.name] = digest(binary)
            text = run('execute-' + stem, [emulator, '-L', '/usr/s390x-linux-gnu', binary, native])
            match = re.search(r': (\d+) checks, 0 failures', text)
            if not match:
                raise RuntimeError(stem + ' did not report successful checks')
            endian_checks['targets'][stem] = int(match[1])
            host_stem = {'CheckBig': 'Check', 'WriterBig': 'Writer',
                         'WriterNoHeapBig': 'WriterNoHeap'}[stem]
            if int(match[1]) != execution_counts[host_stem]:
                raise RuntimeError(stem + ' check count differs from the host')
            if digest(native / 'writer.bin') != writer_digest:
                raise RuntimeError(stem + ' produced different writer bytes from the host')
    after = snapshot()
    if before != after:
        raise RuntimeError('Source inputs changed during qualification')
    summary = dict(passed=True, sanitize=args.sanitize, hardware=False,
                   source_head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                   source_dirty=bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True)),
                   executions=execution_counts, access_contracts=access_checks, endian=endian_checks,
                   corpus_cases=len(records), corpus_accepted=sum(row['accepted'] for row in records),
                   corpus_sha256=digest(packed), generated_value_comparisons=24,
                   generated_writer_comparisons=6, writer_wire_bytes=(native / 'writer.bin').stat().st_size,
                   commands=commands, outputs=outputs, source_sha256_before=before, source_sha256=after,
                   generated={path.relative_to(generated).as_posix(): digest(path) for path in generated.rglob('*') if path.is_file()},
                   fixture_sha256={path.name: digest(path) for path in native.iterdir() if path.is_file()})
    layout = re.search(r'runtime layout: (.+)', (out / 'execute-Check.log').read_text())
    summary['layout_bytes'] = {name: int(size) for name, size in
                              re.findall(r'(\w+)=(\d+)', layout[1])}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print('Runtime schema checks passed:', execution_counts, 'differential', len(records), flush=True)


if __name__ == '__main__':
    main()
