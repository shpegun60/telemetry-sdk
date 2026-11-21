#!/usr/bin/env python3
"""Hold "Reading a Field without the SDK" to the bytes the real server sends.

Builds main.cpp (the group_tree example's Model behind the TCP server and the
encoded profile), runs it, and compares every hex block it prints with the
blocks marked <!-- bytes:NAME --> in doc/user/ReadingAFieldByHand.md and
doc/WireV3.md. A block missing from the program or from the walkthrough, a
byte that differs, or a fingerprint the walkthrough does not state fails.
No socket, board or network is used.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WALKTHROUGH = ROOT / 'doc/user/ReadingAFieldByHand.md'
WIRE = ROOT / 'doc/WireV3.md'
SOURCES = ['lib/telemetry/model/Adapter.cpp', 'lib/telemetry/abi/StructuredAbi.cpp',
           'lib/integration/encoded/Dispatcher.cpp', 'lib/integration/encoded/ReadManyDispatcher.cpp',
           'lib/integration/encoded/BatchDispatcher.cpp', 'lib/telemetry/encoded/ReadMany.cpp',
           'lib/telemetry/encoded/Batch.cpp', 'lib/resource/protocol/Protocol.cpp']
# Every block the walkthrough must show; config-value belongs to WireV3.md.
REQUIRED = {
    WALKTHROUGH: {'list-request', 'list-reply', 'read-request', 'read-reply-envelope',
                  'descriptor-header', 'gain-record', 'session-request', 'session-reply',
                  'field-read-request', 'field-read-reply'},
    WIRE: {'config-value'},
}
MARKED = re.compile(r'<!-- bytes:([a-z-]+) -->\s*\n```text\n(.*?)```', re.S)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hex_bytes(text, where):
    """Two-digit hex tokens before an optional '; comment' on each line."""
    values = []
    for line in text.splitlines():
        for token in line.split(';', 1)[0].split():
            if not re.fullmatch(r'[0-9a-f]{2}', token):
                raise RuntimeError(f'{where}: not a lowercase hex byte: {token!r}')
            values.append(int(token, 16))
    return values


def program_blocks(output):
    blocks = {}
    for name, body in re.findall(r'^BLOCK (\S+)\n(.*?)^END$', output, re.S | re.M):
        if name in blocks:
            raise RuntimeError('The program printed block twice: ' + name)
        blocks[name] = hex_bytes(body, 'program block ' + name)
    return blocks


def document_blocks(path):
    text = path.read_text(encoding='utf-8')
    blocks = {}
    for name, body in MARKED.findall(text):
        if name in blocks:
            raise RuntimeError(f'{path.name}: block marked twice: {name}')
        blocks[name] = hex_bytes(body, f'{path.name} block {name}')
    return text, blocks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    out = args.build_dir.resolve()
    if out == ROOT or (ROOT in out.parents and (ROOT / 'build') not in out.parents):
        parser.error('Use an output directory outside the checkout or under its ignored build/')
    out.mkdir(parents=True, exist_ok=True)
    compiler = Path(shutil.which(args.cxx) or args.cxx).absolute()
    report = {'compiler': str(compiler), 'sanitize': args.sanitize, 'inputs': {},
              'blocks': {}, 'completed': False}
    environment = os.environ.copy()
    environment['PATH'] = str(compiler.parent) + os.pathsep + environment.get('PATH', '')
    environment.setdefault('UBSAN_OPTIONS', 'halt_on_error=1')

    def run(command, label):
        started = time.monotonic()
        result = subprocess.run(list(map(str, command)), cwd=ROOT, env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                encoding='utf-8', errors='replace', timeout=600)
        (out / (label + '.log')).write_text(
            f'COMMAND {list(map(str, command))!r}\nEXIT {result.returncode}\n'
            f'SECONDS {time.monotonic() - started:.3f}\n{result.stdout}', encoding='utf-8')
        if result.returncode:
            raise RuntimeError(label + ' failed\n' + result.stdout)
        print(label + ': pass', flush=True)
        return result.stdout

    try:
        inputs = [HERE / 'main.cpp', WALKTHROUGH, WIRE,
                  *sorted((ROOT / 'examples/group_tree').rglob('*.hpp')),
                  *(ROOT / source for source in SOURCES)]
        report['inputs'] = {path.relative_to(ROOT).as_posix(): sha256(path) for path in inputs}
        flags = [compiler, '-std=c++20', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                 '-pedantic-errors', '-Ilib', '-Ilib/boost_pfr/include', '-Ilib/magic_enum']
        flags += (['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                   '-fno-sanitize-recover=all'] if args.sanitize else ['-O2'])
        program = out / ('field_by_hand' + ('.exe' if os.name == 'nt' else ''))
        run([*flags, HERE / 'main.cpp', *SOURCES, '-o', program], 'build')
        output = run([program], 'execute')
        if not re.search(r'^Field by hand: \d+ checks, 0 failures$', output, re.M):
            raise RuntimeError('The program did not report its checks')
        printed = program_blocks(output)
        fingerprint = re.search(r'^FINGERPRINT (0x[0-9a-f]{16})$', output, re.M)
        if not fingerprint:
            raise RuntimeError('The program printed no fingerprint')
        for path, required in REQUIRED.items():
            text, marked = document_blocks(path)
            missing = required - set(marked)
            if missing:
                raise RuntimeError(f'{path.name} lacks the blocks {sorted(missing)}')
            for name, values in marked.items():
                if name not in printed:
                    raise RuntimeError(f'{path.name} marks {name}, which the program does not print')
                if values != printed[name]:
                    at = next((i for i, (a, b) in enumerate(zip(values, printed[name])) if a != b),
                              min(len(values), len(printed[name])))
                    raise RuntimeError(f'{path.name} block {name} differs from the server at byte {at}: '
                                       f'{len(values)} bytes written, {len(printed[name])} sent')
                report['blocks'][f'{path.name}:{name}'] = len(values)
            if path == WALKTHROUGH and fingerprint[1] not in text:
                raise RuntimeError(f'{path.name} does not state the fingerprint {fingerprint[1]}')
        report['fingerprint'] = fingerprint[1]
        report['completed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"Field by hand: {len(report['blocks'])} blocks match the server's bytes")


if __name__ == '__main__':
    main()
