#!/usr/bin/env python3
"""Build Stage 14 H7S images; access a selected device only with --run.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
The adjacent offline mcu/run.py remains independent of this runner.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import runpy
import shutil
import statistics
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
QUALIFICATION = ROOT / 'tests/structured/qualification'
ARM = ['-mcpu=cortex-m7', '-mthumb', '-mfpu=fpv5-d16', '-mfloat-abi=hard',
       '-fno-exceptions', '-fno-rtti']
SCOPE = 'H7S execution and DWT/PSP measurements'
LIBRARY = ('lib/telemetry/model/Adapter.cpp',
           'lib/telemetry/abi/StructuredAbi.cpp',
           'lib/integration/telemetry/v3/detail/Values.cpp')
CHECK_FLASH_SECTIONS = runpy.run_path(str(ROOT / 'tests/h7s_support/build.py'))['check_flash_sections']


def run(command, log, timeout=180):
    """Retain complete compiler/programmer output and reject nonzero exits."""
    result = subprocess.run(list(map(str, command)), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, encoding='utf-8',
                            errors='replace', timeout=timeout)
    log.write_text(result.stdout, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'{log}: exit {result.returncode}\n{result.stdout[-6000:]}')
    return result.stdout


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('Expected exactly one linker anchor: ' + old)
    return text.replace(old, new, 1)


def lf_sha(path):
    return hashlib.sha256(Path(path).read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def inputs():
    paths = {path for directory in (ROOT / 'lib', HERE.parent, QUALIFICATION)
             for path in directory.rglob('*') if path.is_file() and
             path.suffix in ('.h', '.hpp', '.cpp', '.c', '.pri', '.py', '.S')}
    paths.update(ROOT / relative for relative in (
        'tests/structured/traversal/Fixture.hpp', 'tests/structured/mcu/h7s/StackCall.S',
        'tests/h7s_support/build.py'))
    return {path.relative_to(ROOT).as_posix(): lf_sha(path) for path in sorted(paths)}


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def stack_reports(directory):
    """Keep large template names in artifacts, with compact root summaries."""
    files, frames = {}, {}
    roots = ('nativeBig', 'checkExtra', 'window', 'stackExercise', 'stackControl',
             'bigRead', 'bigCommand', 'bigService', 'consumer_typed', 'consumer_encoded',
             'bench_init', 'bench_loop')
    for path in sorted(directory.glob('*.su')):
        files[path.name] = dict(sha256=sha(path), bytes=path.stat().st_size)
        for line in path.read_text(encoding='utf-8').splitlines():
            name, frame, kind = line.rsplit('\t', 2)
            if '<lambda' in name:
                continue
            for root in roots:
                if re.search(r'(?:::| )' + re.escape(root) + r'\(', name):
                    key = path.stem + '::' + root
                    if key in frames:
                        raise RuntimeError('Duplicate stack root: ' + key)
                    frames[key] = dict(bytes=int(frame), kind=kind)
    return files, frames


def build(args, output):
    """Capture sources and a Cube copy, then link every image before --run."""
    compiler = Path(args.arm_cxx).resolve()
    if not compiler.is_file():
        raise RuntimeError('Compiler is not a file: ' + str(compiler))
    before = inputs()
    source_head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    source_status = subprocess.check_output(['git', 'status', '--porcelain', '-uall'], cwd=ROOT, text=True)
    captured = output / 'inputs'
    for relative, digest in before.items():
        target = captured / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((ROOT / relative).read_bytes().replace(b'\r\n', b'\n'))
        if lf_sha(target) != digest:
            raise RuntimeError('Source changed while capturing: ' + relative)
    cube = output / 'scaffold'
    for relative in ('Boot/Core', 'Drivers'):
        shutil.copytree(args.cube.resolve() / relative, cube / relative)
    linker_source = 'Boot/STM32H7S3L8HX_FLASH.ld'
    shutil.copy2(args.cube.resolve() / linker_source, cube / linker_source)
    scaffold = {p.relative_to(cube).as_posix(): lf_sha(p) for p in sorted(cube.rglob('*')) if p.is_file()}
    common = output / 'common'
    common.mkdir()
    compiler_version = run([compiler, '--version'], common / 'compiler.log').splitlines()[0]
    if not re.search(r'14\.3\.1(?:\s|$)', compiler_version):
        raise RuntimeError('Stage 14 requires the CubeIDE ARM 14.3.1 compiler')
    def tool(name):
        return compiler.with_name('arm-none-eabi-' + name + compiler.suffix)
    flags = [*ARM, '-ffunction-sections', '-fdata-sections', '-DUSE_HAL_DRIVER',
             '-DSTM32H7S3xx', '--specs=nano.specs']
    include = ['-I' + str(cube / 'Boot/Core/Inc'), '-I' + str(captured / 'lib'),
               '-I' + str(captured / 'lib/boost_pfr/include'), '-I' + str(captured / 'lib/magic_enum'),
               # The fixture's telemetry_config.hpp keeps the 1775-byte v3.0 descriptor.
               '-I' + str(captured / 'tests/structured/mcu/config')]
    for relative in ('Drivers/STM32H7RSxx_HAL_Driver/Inc', 'Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy',
                     'Drivers/CMSIS/Device/ST/STM32H7RSxx/Include', 'Drivers/CMSIS/Include'):
        include += ['-isystem', str(cube / relative)]
    common_objects = []
    for source in [*sorted((cube / 'Drivers/STM32H7RSxx_HAL_Driver/Src').glob('*.c')),
                   *sorted((cube / 'Boot/Core/Src').glob('*.c'))]:
        obj = common / (source.stem + '.o')
        run([tool('gcc'), *flags, *include, '-Os', '-std=gnu11', '-Wall', '-c', source, '-o', obj],
            obj.with_suffix('.log'))
        common_objects.append(obj)
    startup = common / 'startup.o'
    run([tool('gcc'), *ARM, '-x', 'assembler-with-cpp', '-c',
         cube / 'Boot/Core/Startup/startup_stm32h7s3l8hx.s', '-o', startup], common / 'startup.log')
    original = (cube / linker_source).read_text(encoding='utf-8')
    linker = common / 'benchmark.ld'
    linker.write_text(replace_once(original, '  ._user_heap_stack :',
        '  .dtcm_ids (NOLOAD) : { . = ALIGN(32); *(.dtcm_ids) . = ALIGN(32); } >DTCM\n'
        '  .mcu_probe_stack (NOLOAD) : { . = ALIGN(32); *(.mcu_probe_stack) . = ALIGN(32); } >DTCM\n'
        '  ._user_heap_stack :'), encoding='utf-8')
    library_sources = {key: value for key, value in before.items() if key.startswith('lib/')}
    images = []
    for family in ('Mixed', 'Scale'):
        sources = ['tests/structured/mcu/h7s/Benchmark.cpp', 'tests/structured/mcu/h7s/Probe.cpp',
                   'tests/structured/mcu/' + family + '.cpp', *LIBRARY]
        if family == 'Mixed':
            sources += ['tests/structured/qualification/' + name + '.cpp' for name in ('Provider', 'Typed', 'Encoded')]
        sources += ['tests/structured/mcu/h7s/StackCall.S']
        for opt in ('O2', 'Os'):
            directory = output / family / opt
            directory.mkdir(parents=True)
            cppflags = [*flags, *include, '-std=c++20', '-' + opt, '-DQUALIFICATION_ARM',
                        '-DLAYOUT_OPT=' + ('2' if opt == 'O2' else '0'), '-Wall', '-Wextra', '-Werror',
                        '-pedantic-errors', '-fno-use-cxa-atexit', '-fstack-usage']
            if family == 'Scale':
                cppflags += ['-DMCU_SCALE']
            objects = []
            for relative in sources:
                source = captured / relative
                obj = directory / (source.stem + '.o')
                source_flags = [*ARM, '-x', 'assembler-with-cpp'] if source.suffix == '.S' else cppflags
                run([compiler, *source_flags, '-c', source, '-o', obj], obj.with_suffix('.log'))
                objects.append(obj)
            elf, binary = directory / 'benchmark.elf', directory / 'benchmark.bin'
            link_flags = [*ARM, '-T' + str(linker), '--specs=nano.specs', '--specs=nosys.specs',
                          '-Wl,--gc-sections', '-Wl,-Map=' + str(directory / 'benchmark.map')]
            run([compiler, *common_objects, startup, *objects, *link_flags,
                 '-Wl,--start-group', '-lc', '-lm', '-Wl,--end-group', '-o', elf], directory / 'link.log')
            run([tool('objcopy'), '-O', 'binary', elf, binary], directory / 'binary.log')
            if not 0 < binary.stat().st_size <= 65536:
                raise RuntimeError('Image exceeds the backed-up internal Flash: ' + str(binary))
            sections = run([tool('objdump'), '-h', elf], directory / 'sections.log')
            flash_load_span = CHECK_FLASH_SECTIONS(sections, binary.stat().st_size)
            run([tool('objdump'), '-drC', elf], directory / 'benchmark.asm')
            run([tool('size'), '-A', elf], directory / 'size.log')
            run([tool('nm'), '-C', elf], directory / 'symbols.log')
            stack_usage, stack_frames = stack_reports(directory)
            image = dict(family=family, optimization=opt, elf=elf.relative_to(output).as_posix(),
                         binary=binary.relative_to(output).as_posix(), elf_sha256=sha(elf), binary_sha256=sha(binary),
                         flash_bytes=binary.stat().st_size, objects_sha256={p.name: sha(p) for p in objects},
                         flash_load_span=flash_load_span,
                         common_objects_sha256={p.name: sha(p) for p in [*common_objects, startup]},
                         library_sources=library_sources, flags=cppflags, link_flags=link_flags,
                         linker_sha256=sha(linker),
                         stack_usage=stack_usage, stack_frames=stack_frames)
            images.append(image)
            print(f'BUILT {family} {opt}: {image["flash_bytes"]}/65536 Flash bytes', flush=True)
    if inputs() != before:
        raise RuntimeError('Build inputs changed during compilation')
    receipt = dict(format_version=1, scope=SCOPE, source_head=source_head, source_dirty=bool(source_status),
                   source_status=source_status, input_lf_sha256=before, scaffold_lf_sha256=scaffold,
                   compiler=compiler_version, compiler_sha256=sha(compiler), compiler_path=str(compiler),
                   captured_at_utc=datetime.now(timezone.utc).isoformat(), completed=False,
                   restored_and_verified=False, images=images, execution='compile/link only')
    write_json(output / 'build-inputs.json', before)
    write_json(output / 'images.json', images)
    write_json(output / 'receipt.json', receipt)
    return receipt


def summaries(receipt):
    result = []
    for image in receipt['images']:
        for operation in image['measurement']['operations']:
            for profile in range(3):
                if not operation['profiles'] & (1 << profile):
                    continue
                timing = [row['cycles'] / row['calls'] for row in image['measurement']['timing']
                          if row['operation'] == operation['index'] and row['profile'] == profile]
                stack = [row['used'] for row in image['measurement']['stack']
                         if row['operation'] == operation['index'] and row['profile'] == profile]
                result.append(dict(family=image['family'], optimization=image['optimization'],
                    operation=operation['name'], profile=profile, median_cycles=statistics.median(timing),
                    minimum_cycles=min(timing), maximum_cycles=max(timing), maximum_observed_stack_bytes=max(stack)))
    return result


def measure(args, output, receipt):
    """This is the only function which imports serial or calls a programmer."""
    import serial
    check = runpy.run_path(str(HERE / 'verify.py'))
    receipt.update(execution='device', serial=args.serial, port=args.port,
                   started_at_utc=datetime.now(timezone.utc).isoformat())
    def save():
        write_json(output / 'receipt.json', receipt)
    connection = ['-c', 'port=SWD', 'sn=' + args.serial, 'mode=UR', 'reset=HWrst', 'freq=4000']
    def program(label, path, raw=False):
        info = run([args.programmer, *connection, '-w', path,
                    *(['0x08000000'] if raw else []), '-v', '-rst'], output / (label + '.log'), 60)
        if 'Download verified successfully' not in info:
            raise RuntimeError('Missing programming verification: ' + label)
    # The explicit serial selects a single adapter; no enumeration is used.
    backup = output / 'before.bin'
    info = run([args.programmer, *connection, '-u', '0x08000000', '0x10000', backup, '-rst'],
               output / 'backup.log', 60)
    boards = re.findall(r'^Board\s*:\s*(.+)$', info, re.M)
    serials = re.findall(r'^ST-LINK SN\s*:\s*(.+)$', info, re.M)
    if boards != ['NUCLEO-H7S3L8'] or serials != [args.serial] or not re.search(r'Device ID\s*:\s*0x485\b', info):
        raise RuntimeError('Selected adapter/board is not the requested NUCLEO-H7S3L8; no programming attempted')
    if not backup.is_file() or backup.stat().st_size != 65536:
        raise RuntimeError('A complete 64 KiB backup is required before programming')
    receipt.update(board=boards[0], backup_bytes=backup.stat().st_size, backup_sha256=sha(backup))
    save()
    try:
        for image in receipt['images']:
            key = image['family'] + '-' + image['optimization']
            if sha(output / image['elf']) != image['elf_sha256'] or sha(output / image['binary']) != image['binary_sha256']:
                raise RuntimeError('Compiled image changed since build: ' + key)
            program('flash-' + key, output / image['elf'])
            image['uart_lines'] = []
            with serial.Serial(args.port, 115200, timeout=0.5, write_timeout=2) as port:
                time.sleep(0.3)
                port.reset_input_buffer()
                if port.write(b'R') != 1:
                    raise RuntimeError('Short UART command write')
                port.flush()
                start = time.monotonic()
                with (output / (key + '-uart.log')).open('w', encoding='utf-8') as log:
                    while time.monotonic() - start < 180:
                        raw = port.readline()
                        if not raw:
                            continue
                        text = raw.decode('ascii').strip()
                        log.write(text + '\n'); log.flush()
                        image['uart_lines'].append(text)
                        if text == 'MCU DONE':
                            break
                        if text.startswith('MCU FAIL'):
                            raise RuntimeError('Device refused measurement: ' + text)
                    else:
                        raise TimeoutError('Measurement did not complete: ' + key)
            image['measurement'] = check['verify_image'](image)
            save()
            print('MEASURED ' + key, flush=True)
        receipt['completed'] = True
    except BaseException as error:
        receipt['error'] = str(error)
        raise
    finally:
        try:
            program('restore', backup, raw=True)
            restored = output / 'after.bin'
            run([args.programmer, *connection, '-u', '0x08000000', '0x10000', restored, '-rst'],
                output / 'readback.log', 60)
            receipt['restored_bytes'] = restored.stat().st_size
            receipt['restored_sha256'] = sha(restored)
            receipt['restored_and_verified'] = receipt['restored_bytes'] == 65536 and receipt['restored_sha256'] == receipt['backup_sha256']
            if not receipt['restored_and_verified']:
                raise RuntimeError('Restored internal Flash differs from its complete backup')
            print('Original 64 KiB image restored, read-back verified and left running.', flush=True)
        except BaseException as error:
            receipt['restore_error'] = str(error)
            raise
        finally:
            receipt['finished_at_utc'] = datetime.now(timezone.utc).isoformat()
            save()
    check['verify'](receipt, current=True, artifacts=output)
    write_json(output / 'summary.json', summaries(receipt))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cube', type=Path, required=True, help='copied Cube scaffold with bench hooks and 600 MHz clock')
    parser.add_argument('--arm-cxx', required=True, help='CubeIDE ARM 14.3.1 g++ executable')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--serial', help='explicit adapter serial, required with --run')
    parser.add_argument('--port', help='explicit UART COM port, required with --run')
    parser.add_argument('--programmer', help='explicit programmer executable, required with --run')
    args = parser.parse_args()
    if args.run and not all((args.serial, args.port, args.programmer)):
        parser.error('--run requires --serial, --port and --programmer')
    output = args.output.resolve()
    if output.exists():
        parser.error('Use a fresh output directory to preserve previous evidence')
    output.mkdir(parents=True)
    receipt = build(args, output)
    if args.run:
        measure(args, output, receipt)


if __name__ == '__main__':
    main()
