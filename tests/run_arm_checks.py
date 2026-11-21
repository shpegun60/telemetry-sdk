#!/usr/bin/env python3
"""Compile final C++20 shared Cortex-M7 contracts and inspect ID boundaries (MIT)."""
import argparse
from pathlib import Path
import os
import re
import shutil
import sys
from regression.checks import Commands, POSITIVES, syntax_contracts, header_and_fp_checks

ROOT = Path(__file__).resolve().parent.parent
HERE = Path(__file__).resolve().parent

def function_body(disassembly, name):
    match = re.search(rf'^[0-9a-fA-F]+ <{re.escape(name)}>:\n(.*?)(?=^[0-9a-fA-F]+ <|^Disassembly of section |\Z)',
        disassembly, re.M | re.S)
    if match is None:
        raise RuntimeError('Missing ARM probe: ' + name)
    return match[1]

def normalized_instructions(disassembly, name):
    result = []
    for line in function_body(disassembly, name).splitlines():
        match = re.match(r'\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{4}\s+)+([a-z][a-z0-9.]*)\s*(.*)', line)
        if not match:
            continue
        operation = match[1].removesuffix('.w').removesuffix('.n')
        operands = re.split(r'\s*[;@]', match[2], maxsplit=1)[0].strip()
        result.append((operation, operands))
    return result

def slot_instructions(disassembly, name):
    instructions = normalized_instructions(disassembly, name)
    end = len(instructions)
    while end and instructions[end - 1][0] == 'nop':
        end -= 1
    if end == len(instructions) or not end:
        return instructions
    operation, operands = instructions[end - 1]
    terminal = operation == 'b' or (operation == 'bx' and operands == 'lr') or (
        operation == 'pop' and re.search(r'\bpc\b', operands) is not None)
    if not terminal:
        return instructions
    addresses = [int(match[1], 16) for line in function_body(disassembly, name).splitlines()
        if (match := re.match(r'\s*([0-9a-fA-F]+):\s+(?:[0-9a-fA-F]{4}\s+)+[a-z][a-z0-9.]*\s*', line))]
    padding = set(addresses[end:])
    # A suffix after a terminal transfer has no fallthrough. Keep it if any
    # explicit branch/call in this body targets one of those nop addresses.
    for operation, operands in instructions:
        if operation.startswith('b') or operation in ('cbz', 'cbnz'):
            target = re.search(r'\b([0-9a-fA-F]+) <', operands)
            if target and int(target[1], 16) in padding:
                return instructions
    return instructions[:end]

def comparable_slot_instructions(disassembly, name):
    body = function_body(disassembly, name)
    instructions = slot_instructions(disassembly, name)
    start = int(re.search(rf'^([0-9a-fA-F]+) <{re.escape(name)}>:', disassembly, re.M)[1], 16)
    addresses = [int(match[1], 16) for line in body.splitlines()
        if (match := re.match(r'\s*([0-9a-fA-F]+):\s+(?:[0-9a-fA-F]{4}\s+)+[a-z][a-z0-9.]*\s*', line))]
    ordinals = {address: index for index, address in enumerate(addresses[:len(instructions)])}
    relocations = {int(match[1], 16): (match[2], match[3]) for match in re.finditer(
        r'^\s*([0-9a-fA-F]+):\s+R_ARM_(THM_CALL|THM_JUMP24)\s+([^\n]+)', body, re.M)}
    result = []
    for address, (operation, operands) in zip(addresses, instructions):
        target = re.search(r'\b([0-9a-fA-F]+) <(.+)>$', operands)
        if target and (operation.startswith('b') or operation in ('cbz', 'cbnz')):
            # Relocatable call placeholders may spell this function's own
            # instruction zero. A relocation at the call site takes precedence.
            if address in relocations:
                kind, symbol = relocations[address]
                normalized = '<relocation:' + kind + ' ' + symbol + '>'
            else:
                local = re.fullmatch(re.escape(name) + r'(?:\+0x([0-9a-fA-F]+))?', target[2])
                if local:
                    destination = int(target[1], 16)
                    if destination != start + int(local[1] or '0', 16) or destination not in ordinals:
                        raise RuntimeError(name + ': local branch target is not a matching instruction address')
                    normalized = '<instruction:' + str(ordinals[destination]) + '>'
                else:
                    normalized = '<external:' + target[2] + '>'
            operands = operands[:target.start()] + normalized
        operands = re.sub(r'\[pc, #\d+\]', '[pc, #literal]', operands)
        result.append((operation, operands))
    return result

def check_slot_comparisons(disassembly, optimization):
    current = {}
    for kind in ('function', 'context', 'borrowed', 'owned', 'owner'):
        direct = slot_instructions(disassembly, 'slot_' + kind + '_direct')
        table = slot_instructions(disassembly, 'slot_' + kind + '_table')
        direct_body = function_body(disassembly, 'slot_' + kind + '_direct')
        table_body = function_body(disassembly, 'slot_' + kind + '_table')
        def references(body):
            words = re.findall(r'\.word\s+(0x[0-9a-fA-F]+)', body)
            relocations = re.findall(r'R_ARM_(?:ABS32|REL32|THM_CALL|THM_JUMP24)\s+([^\n]+)', body)
            return sorted(words), sorted(relocations)
        if references(direct_body) != references(table_body):
            raise RuntimeError(optimization + ': native ' + kind + ' dispatch changed literal or relocation targets')
        if kind == 'owner':
            direct_calls = re.findall(r'R_ARM_THM_(?:CALL|JUMP24)\s+([^\n]+)', direct_body)
            table_calls = re.findall(r'R_ARM_THM_(?:CALL|JUMP24)\s+([^\n]+)', table_body)
            if len(table) > len(direct) or direct_calls != table_calls or direct_calls != ['Owner::read() const']:
                raise RuntimeError(optimization + ': owner table gained work or changed its direct method call')
        elif comparable_slot_instructions(disassembly, 'slot_' + kind + '_direct') != comparable_slot_instructions(
                disassembly, 'slot_' + kind + '_table'):
            raise RuntimeError(optimization + ': native ' + kind + ' table dispatch differs from selected-target call')
        current[kind] = dict(direct_instructions=len(direct), table_instructions=len(table),
            gate='no added instructions and same method relocation' if kind == 'owner' else 'identical normalized instructions')
    return current

def check_id_boundaries(disassembly):
    for operation in ('local', 'field', 'command', 'group', 'index'):
        name = 'id_boundary_' + operation + '_u64'
        body = function_body(disassembly, name)
        high_word = re.search(r'\bcbn?z\s+r1,', body)
        if not high_word:
            raise RuntimeError(name + ': missing high-word rejection')
        if re.search(r'\b(?:ldr|ldrd|lsrs|uxth|bl|blx)\b', body[:high_word.start()]):
            raise RuntimeError(name + ': ID used before width check')
    for operation, extraction in (('group', 'lsrs'), ('index', 'uxth')):
        name = 'id_boundary_' + operation + '_u32'
        instructions = [(op, args) for op, args in normalized_instructions(disassembly, name) if op != 'nop']
        if len(instructions) != 2 or instructions[0][0] != extraction or instructions[1] != ('bx', 'lr'):
            raise RuntimeError(name + ': native extraction gained work')

# DescriptorSizeProbe.cpp: the models, the two ways to serve a descriptor, and
# the type names each model's image must carry with names on and lack with them off.
SIZE_MODELS = {0: 'nucleo', 1: 'tree', 2: 'flat'}
SIZE_FORMS = {0: 'packed', 1: 'streaming'}
SIZE_NAMES = {0: (b'nucleo_demo::Voltage', b'nucleo_demo::Status', b'nucleo_demo::Probe', b'nucleo_demo::Mode'),
              1: (b'adc::Config', b'adc::Status', b'adc::Reference', b'adc::Offset', b'metrology::Config',
                  b'metrology::Window')}
SIZE_NAMES[2] = SIZE_NAMES[1]

def tool_beside(compiler, name):
    return str(Path(compiler).with_name('arm-none-eabi-' + name + ('.exe' if os.name == 'nt' else '')))

def descriptor_size_report(flags, runner, output, compiler):
    """Link descriptor_probe for every model, form and names setting; report and gate its sizes."""
    size_tool, objcopy, nm = (tool_beside(compiler, name) for name in ('size', 'objcopy', 'nm'))
    report = {}
    for optimization in ('O2', 'Os'):
        for model, model_name in SIZE_MODELS.items():
            for form, form_name in SIZE_FORMS.items():
                for names in (1, 0):
                    label = f'descriptor-size-{model_name}-{form_name}-names{names}-{optimization}'
                    elf = output / (label + '.elf')
                    runner.run([*flags, '-' + optimization, '-ffunction-sections', '-fdata-sections',
                                f'-DMODEL={model}', f'-DFORM={form}', f'-DTELEMETRY_TYPE_NAMES={names}',
                                HERE / 'regression/DescriptorSizeProbe.cpp', '-nostdlib', '-Wl,-e,descriptor_probe',
                                '-Wl,--gc-sections', '-lc', '-lgcc', '-o', elf], label)
                    sections = dict(re.findall(r'^(\.\w+)\s+(\d+)', runner.run([size_tool, '-A', elf], label + '-size'),
                                               re.M))
                    image = output / (label + '.bin')
                    runner.run([objcopy, '-O', 'binary', elf, image], label + '-binary')
                    loaded = image.read_bytes()
                    present = [name for name in SIZE_NAMES[model] if name in loaded]
                    if names and len(present) != len(SIZE_NAMES[model]):
                        raise RuntimeError(label + ': a type name is missing from the image')
                    if not names and present:
                        raise RuntimeError(label + ': names off left ' + b', '.join(present).decode() + ' in the image')
                    entry = {name: int(sections.get(name, 0)) for name in ('.text', '.rodata', '.data')}
                    if form == 0:
                        symbols = runner.run([nm, '-S', elf], label + '-symbols')
                        # A const namespace-scope object has internal linkage: _ZL17descriptor_packed.
                        packed = re.search(r'^[0-9a-f]+ ([0-9a-f]+) [rR] (?:_ZL\d+)?descriptor_packed$',
                                           symbols, re.M)
                        if not packed:
                            raise RuntimeError(label + ': no packed descriptor symbol')
                        entry['descriptor'] = int(packed[1], 16)
                    report.setdefault(optimization, {}).setdefault(f'{model_name}-{form_name}', {})[
                        'names' if names else 'stripped'] = entry
    for optimization, rows in report.items():
        for row, entries in rows.items():
            named, stripped = entries['names'], entries['stripped']
            print(f'descriptor {optimization} {row}: ' + ', '.join(
                f'{name} {named[name]} B (names off {stripped[name]} B, {named[name] - stripped[name]:+d} B)'
                for name in ('.text', '.rodata', '.data')) + (
                f'; descriptor {named["descriptor"]} B (names off {stripped["descriptor"]} B)'
                if 'descriptor' in named else ''), flush=True)
    return report

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('ARM_CXX', 'arm-none-eabi-g++'))
    parser.add_argument('--objdump')
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--null-checks', action='store_true')
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if compiler is None:
        raise RuntimeError('ARM compiler not found: ' + args.cxx)
    objdump = args.objdump or str(Path(compiler).with_name('arm-none-eabi-objdump' + ('.exe' if os.name == 'nt' else '')))
    root, output = args.source_root.resolve(), args.build_dir.resolve()
    runner = Commands(root, output)
    flags = [compiler, '-std=c++20', '-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard',
        '-fno-exceptions', '-fno-rtti', '-Wall', '-Wextra', '-Werror', '-pedantic-errors', '-fdiagnostics-color=never',
        '-I' + str(root / 'lib'), '-I' + str(root / 'lib/boost_pfr/include'), '-I' + str(root / 'lib/magic_enum')]
    if args.null_checks:
        flags.append('-fno-delete-null-pointer-checks')
    version = runner.run([compiler, '--version'], 'compiler-version')
    target = runner.run([compiler, '-dumpmachine'], 'compiler-target').strip()
    if target != 'arm-none-eabi':
        raise RuntimeError('Unexpected compiler target: ' + target)
    probes = 0
    slot_comparisons = {}
    for optimization in ('O2', 'Os'):
        for suite in (*POSITIVES, 'DebugLevelCheck'):
            runner.run([*flags, '-' + optimization, '-fstack-usage', '-c', HERE / 'regression' / (suite + '.cpp'),
                '-o', output / (suite + '-' + optimization + '.o')], suite + '-' + optimization + '-compile')
        obj = output / ('IdBoundaryCodegen-' + optimization + '.o')
        runner.run([*flags, '-' + optimization, '-c', HERE / 'regression/IdBoundaryCodegen.cpp', '-o', obj], 'id-codegen-' + optimization)
        assembly = runner.run([objdump, '-drC', obj], 'id-assembly-' + optimization)
        check_id_boundaries(assembly)
        changed, count = re.subn(r'(\bcbn?z\s+)r1,', r'\g<1>r0,', assembly, count=1)
        if count != 1:
            raise RuntimeError('Missing ID gate mutation control')
        try:
            check_id_boundaries(changed)
        except RuntimeError:
            pass
        else:
            raise RuntimeError('ID gate accepted changed high-word register')
        probes += 1
        slot_obj = output / ('SlotCodegen-' + optimization + '.o')
        runner.run([*flags, '-' + optimization, '-fno-ipa-icf', '-c', HERE / 'regression/SlotCodegen.cpp', '-o', slot_obj], 'slot-codegen-' + optimization)
        slot_asm = runner.run([objdump, '-drC', slot_obj], 'slot-assembly-' + optimization)
        slot_comparisons[optimization] = check_slot_comparisons(slot_asm, optimization)
    runner.run([*flags, '-Og', '-c', HERE / 'regression/DebugLevelCheck.cpp', '-o', output / 'DebugLevelCheck-Og.o'], 'debug-Og-compile')
    syntax_contracts(flags, runner.run, HERE)
    for case in range(1, 5):
        runner.run([*flags, '-DCASE=' + str(case), '-fsyntax-only', HERE / 'regression/IdNameCollisionCheck.cpp'], 'id-name-' + str(case))
    headers = header_and_fp_checks(flags, runner.run, root, output, version)
    descriptor_sizes = descriptor_size_report(flags, runner, output, compiler)
    runner.write_summary(headers=headers, compiler_version=version.strip(), source_root=str(root), standard='c++20',
        codegen_probes=probes, mutation_controls=probes, slot_comparisons=slot_comparisons, null_checks=args.null_checks,
        descriptor_sizes=descriptor_sizes,
        limitation='Offline object and diagnostic checks; no target execution or whole-call-chain stack claim')
    print('Shared Cortex-M7 checks passed:', runner.counts, 'codegen probes:', probes, flush=True)

if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as error:
        sys.exit(str(error))
