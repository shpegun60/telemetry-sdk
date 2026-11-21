#!/usr/bin/env python3
"""Execute MCU probe bodies on the host; compile/link/inspect ARM without a board.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
This script has no programmer, serial-port or device-discovery integration.
ARM output is codegen evidence, never reported as an MCU execution result.
"""
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
ROOT = HERE.parents[2]
QUALIFICATION = HERE.parent / 'qualification'
sys.path.insert(0, str(QUALIFICATION))
from gates import EXPECTED_CONSUMER_CHECKS, counted_checks, controls, forbidden_symbols, linked_control

# Mixed checks the descriptor twice at every cursor; the traversal fixture's group
# tree describes itself in 1657 bytes, 170 fewer than its per-family catalogs did.
EXPECTED_PROBE_CHECKS = {'Mixed': 11890, 'Scale': 2316}
LIBRARY = [ROOT / name for name in (
    'lib/telemetry/model/Adapter.cpp',
    'lib/telemetry/abi/StructuredAbi.cpp',
    'lib/integration/telemetry/v3/detail/Values.cpp')]


def bodies(assembly):
    """Normalize instruction addresses, retaining operand/branch structure."""
    result, current = {}, None
    for line in assembly.splitlines():
        header = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if header:
            current = header.group(1)
            result[current] = []
            continue
        instruction = re.match(r'^\s*[0-9a-f]+:\s+(?:[0-9a-f]{4,8}\s+)+(.+)$', line)
        if current and instruction:
            text = instruction.group(1).split(';', 1)[0].strip()
            # Object-local branch target labels include the enclosing symbol.
            text = re.sub(r'<[^>]+>', '<target>', text)
            result[current].append(text)
    return result


def tail_targets(assembly):
    result, current = {}, None
    for line in assembly.splitlines():
        header = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
        if header:
            current = header.group(1)
        jump = re.search(r'R_ARM_THM_JUMP24\s+(.+)$', line)
        if current and jump:
            result[current] = jump.group(1)
    return result


def native_group(assembly, group, *, null_checks, optimized):
    """Preserve raw streams and account explicitly for compiler tail forwards."""
    normalized = bodies(assembly)
    jumps = tail_targets(assembly)
    selected, effective, forwards = {}, {}, {}
    for name in group:
        symbols = [symbol for symbol in normalized if f'::{name}(unsigned' in symbol]
        if len(symbols) != 1 or not normalized[symbols[0]]:
            raise RuntimeError('Missing probe body ' + name)
        symbol = symbols[0]
        selected[name] = normalized[symbol]
        chain = []
        while len(normalized[symbol]) == 1 and re.match(r'^b(?:\.w)?\s', normalized[symbol][0]):
            target = jumps.get(symbol)
            if target not in normalized or target in chain or len(chain) >= 4:
                raise RuntimeError('Unresolved compiler tail forward from ' + symbol)
            chain.append(target)
            symbol = target
        if chain:
            forwards[name] = chain
        effective[name] = normalized[symbol]
    if optimized:
        # Null-check mode intentionally retains target-address checks. It must
        # still give equivalent local/global implementations. Its difference
        # from direct C++ is recorded, rather than hidden or called zero cost.
        reference = group[1] if null_checks else group[0]
        required = group[1:] if null_checks else group
        if any(effective[name] != effective[reference] for name in required):
            raise RuntimeError('Native probes differ: ' + str(selected))
    return dict(bodies=selected, tail_forwards=forwards,
                raw_direct_equivalent={name: selected[name] == selected[group[0]] for name in group},
                effective_direct_equivalent={name: effective[name] == effective[group[0]] for name in group})


def codegen_controls():
    """Check that accounting for a forward cannot conceal different code."""
    signature = lambda name: f'probe::{name}(unsigned int)'
    def sample(local='ldr r0, [r1]', global_body='ldr r0, [r1]', relocation=''):
        return '\n'.join(
            f'00000000 <{signature(name)}>:\n  0: 6808 {instruction}\n{reloc}'
            for name, instruction, reloc in (
                ('direct', 'ldr r0, [r1]', ''), ('local', local, ''), ('global', global_body, relocation)))
    group = ('direct', 'local', 'global')
    native_group(sample(), group, null_checks=False, optimized=True)
    forward = f'  0: R_ARM_THM_JUMP24 {signature("local")}'
    positive = native_group(sample(global_body='b.w 0 <target>', relocation=forward), group,
                            null_checks=False, optimized=True)
    if not positive['tail_forwards'] or positive['raw_direct_equivalent']['global']:
        raise AssertionError('Tail-forward accounting lost its raw branch')
    mutations = [
        (sample(local='adds r0, #1'), False),
        (sample(global_body='adds r0, #1'), False),
        (sample(global_body='b.w 0 <target>'), False),
        (sample(global_body='b.w 0 <target>',
                relocation=f'  0: R_ARM_THM_JUMP24 {signature("global")}'), False),
        (sample(local='cbz r1, 8 <target>', global_body='adds r0, #1'), True),
    ]
    for assembly, null_checks in mutations:
        try:
            native_group(assembly, group, null_checks=null_checks, optimized=True)
        except RuntimeError:
            continue
        raise AssertionError('Changed/unresolved native stream accepted')
    return {'positive': 2, 'rejected_mutations': len(mutations)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--arm', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--null-checks', action='store_true')
    parser.add_argument('--optimizations', nargs='+', choices=['O2', 'Os', 'Og'], default=['O2', 'Os', 'Og'])
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    if args.arm and args.sanitize:
        parser.error('ARM sanitizer execution is not part of this offline runner')
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    compiler = Path(shutil.which(args.cxx) or args.cxx).absolute()
    env = os.environ.copy()
    env['PATH'] = str(compiler.parent) + os.pathsep + env.get('PATH', '')
    report = dict(commands=0, checks=0, configurations={}, execution='compile/link only' if args.arm else 'host')
    report['codegen_controls'] = codegen_controls()
    report['gate_controls'] = controls()
    inputs = sorted(path for directory in (ROOT / 'lib', HERE, QUALIFICATION)
                    for path in directory.rglob('*')
                    if path.is_file() and path.suffix in ('.h', '.hpp', '.cpp', '.c', '.pri', '.py'))
    inputs += [HERE.parent / 'traversal/Fixture.hpp']

    def input_hashes():
        return {str(path.relative_to(ROOT)).replace('\\', '/'):
                hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest() for path in inputs}

    before = input_hashes()

    def run(command, label):
        command = list(map(str, command))
        start = time.perf_counter()
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                                errors='replace', timeout=180)
        seconds = time.perf_counter() - start
        (out / (label + '.log')).write_text(
            f'COMMAND {command!r}\nEXIT {result.returncode}\nSECONDS {seconds:.6f}\n{result.stdout}',
            encoding='utf-8')
        if result.returncode:
            raise RuntimeError(label + ': command failed\n' + result.stdout[-6000:])
        report['commands'] += 1
        print(f'{label}: pass ({seconds:.2f}s)', flush=True)
        return result.stdout

    version = run([compiler, '--version'], 'compiler')
    report['compiler'] = version.splitlines()[0]
    flags = [compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic-errors',
             '-fdiagnostics-color=never', '-Ilib', '-Ilib/boost_pfr/include', '-Ilib/magic_enum',
             '-fstack-usage',
             # The fixture's own telemetry_config.hpp keeps its descriptor v3.0 (no type names).
             '-Itests/structured/mcu/config']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    if args.null_checks:
        flags += ['-fno-delete-null-pointer-checks']
    if args.arm:
        flags += ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard',
                  '-fno-exceptions', '-fno-rtti', '-ffunction-sections', '-fdata-sections',
                  '-DQUALIFICATION_ARM']
    # 128 host rows deliberately omit COFF per-function named sections: the
    # identical Stage 13 stressor otherwise exceeds its section-name offset.
    link = ['-Wl,--gc-sections']
    if args.arm:
        link += ['-nostdlib', '-Wl,-e,main', '-lc', '-lgcc']
    elif 'clang' in version.lower():
        link += ['-fuse-ld=lld']
    report['flags'] = list(map(str, flags))
    if args.arm:
        report['linked_symbol_control'] = linked_control(
            flags, out, run, compiler.with_name('arm-none-eabi-nm' + compiler.suffix))

    for family in ('Mixed', 'Scale'):
        sources = [HERE / 'Host.cpp', HERE / (family + '.cpp')]
        if family == 'Mixed':
            sources += [QUALIFICATION / (name + '.cpp') for name in ('Provider', 'Typed', 'Encoded')]
        sources += LIBRARY
        family_flags = ['-DMCU_SCALE'] if family == 'Scale' else []
        for opt in args.optimizations:
            label = family + '-' + opt
            objects = []
            for source in sources:
                obj = out / f'{label}-{source.stem}.o'
                run(flags + ['-' + opt, *family_flags, '-c', source, '-o', obj], obj.stem)
                objects.append(obj)
            program = out / (label + ('.elf' if args.arm else '.exe' if os.name == 'nt' else ''))
            run(flags + ['-' + opt, *family_flags, *objects, *link, '-o', program], 'link-' + label)
            record = dict(frames={})
            if not args.arm:
                text = run([program], 'execute-' + label)
                count = counted_checks(text, label, EXPECTED_PROBE_CHECKS[family])
                consumer_checks = json.loads(text)['consumer_checks']
                expected_consumer = EXPECTED_CONSUMER_CHECKS if family == 'Mixed' else 0
                if consumer_checks != expected_consumer:
                    raise RuntimeError(label + ': changed multi-TU consumer check count')
                report['checks'] += count
                record['checks'] = count
                record['consumer_checks'] = consumer_checks
            else:
                def tool(name):
                    return compiler.with_name('arm-none-eabi-' + name + compiler.suffix)
                sections = run([tool('size'), '-A', program], 'sections-' + label)
                parsed = {name: int(size) for name, size in re.findall(
                    r'^(\S+)\s+(\d+)\s+\d+\s*$', sections, re.M)}
                record['sections'] = {kind: sum(size for name, size in parsed.items()
                    if name.startswith('.' + kind)) for kind in ('text', 'rodata', 'data', 'bss')}
                if any(name.startswith(('.init_array', '.preinit_array', '.ctors')) and size
                       for name, size in parsed.items()):
                    raise RuntimeError(label + ': metadata acquired startup constructors')
                symbols = run([tool('nm'), '-C', program], 'symbols-' + label)
                rejected = forbidden_symbols(symbols)
                if rejected:
                    raise RuntimeError(label + ': allocation/formatting retained in linked probes: ' + str(rejected))
                probe_object = next(obj for obj in objects if obj.stem.endswith('-' + family))
                assembly = run([tool('objdump'), '-drC', probe_object], 'assembly-' + label)
                if 'requireStructuredAbi' in assembly:
                    raise RuntimeError(label + ': explicit startup ABI check entered a hot probe')
                # Compare the generated bodies of actual probe roots, not a
                # tiny standalone example which bypasses endpoint binding.
                groups = [('direct', 'local', 'global'), ('serviceDirect', 'serviceLocal', 'serviceGlobal')] \
                    if family == 'Mixed' else [('directKnown', 'typedLocal', 'typedGlobal')]
                record['native_codegen'] = []
                for group in groups:
                    record['native_codegen'].append(native_group(assembly, group,
                        null_checks=args.null_checks, optimized=opt != 'Og'))
            for obj in objects:
                for line in obj.with_suffix('.su').read_text().splitlines():
                    name, frame, kind = line.split('\t')
                    record['frames'][name] = {'bytes': int(frame), 'kind': kind}
                    # Only the pre-existing consumer roots need the larger
                    # argument/spill budget; a 4 KiB endpoint local is refused.
                    # Directory names never exempt final-core frames from
                    # the bounded-stack contract.
                    root = 'consumer_typed(' in name or 'consumer_encoded(' in name
                    limit = (768 if opt == 'Og' else 512) if root else 256
                    if args.arm and (kind != 'static' or int(frame) > limit):
                        raise RuntimeError(label + ': large/dynamic frame: ' + line)
            report['configurations'][label] = record
    if input_hashes() != before:
        raise RuntimeError('Captured inputs changed during the run')
    report['input_lf_sha256'] = before
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: report[key] for key in ('commands', 'checks', 'execution')}), flush=True)


if __name__ == '__main__':
    main()
