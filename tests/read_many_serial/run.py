#!/usr/bin/env python3
"""Build the serial ReadMany example and execute real PTY exchanges, without a board. MIT."""
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

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources(cobs):
    result = {}
    for base, label, directories in (
        (ROOT, "", ("lib", "cmake", "examples/read_many_uart",
                     "examples/nucleo_h7s3l8", "tests/read_many_serial")),
        (cobs, "COBS/", ("src",)),
    ):
        for directory in directories:
            for parent, names, files in os.walk(base / directory):
                names[:] = [name for name in names if name not in (".git", "__pycache__", "build", "out")]
                for name in files:
                    path = Path(parent) / name
                    if path.suffix in (".cpp", ".hpp", ".h", ".py", ".txt", ".cmake"):
                        result[label + path.relative_to(base).as_posix()] = digest(path)
    result["CMakeLists.txt"] = digest(ROOT / "CMakeLists.txt")
    return dict(sorted(result.items()))


def counted_result(text, label, minimum, pty=None):
    if pty is None:
        matches = re.findall(r'^ReadMany bootstrap checks=(\d+) passed, hardware=(\d+)\s*$', text, re.M)
        expected = 0
    else:
        matches = re.findall(r'^ReadMany Serial checks=(\d+) pty=(\d+)\s*$', text, re.M)
        expected = int(pty)
    if (len(matches) != 1 or int(matches[0][0]) < minimum or int(matches[0][0]) <= 0
            or int(matches[0][1]) != expected):
        raise ValueError('Missing/invalid counted result: ' + label)
    return int(matches[0][0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--cobs-root", type=Path, required=True)
    parser.add_argument("--qt-prefix", type=Path, required=True)
    parser.add_argument("--serial-prefix", type=Path)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    output, cobs = args.build_dir.resolve(), args.cobs_root.resolve()
    for source in (ROOT, cobs):
        if output == source or source in output.parents or output in source.parents:
            parser.error("Use a dedicated build directory outside Telemetry and COBS sources")
    if output.exists() and any(output.iterdir()):
        parser.error("The build directory must be fresh")
    output.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, hardware=False, commands=[], source_sha256_before=sources(cobs))
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    prefixes = [args.qt_prefix.resolve()] + ([args.serial_prefix.resolve()] if args.serial_prefix else [])
    environment["PATH"] = os.pathsep.join([str(Path(shutil.which(args.cxx) or args.cxx).parent)] +
        [str(prefix / "bin") for prefix in prefixes] + [environment.get("PATH", "")])
    if args.sanitize:
        environment.update(ASAN_OPTIONS="detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def run(name, command, expected=0):
        command = list(map(str, command))
        start = time.monotonic()
        result = subprocess.run(command, cwd=ROOT, env=environment, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
        log = output / (name + ".log")
        log.write_text(result.stdout, encoding="utf-8")
        report["commands"].append(dict(name=name, command=command, exit=result.returncode,
            seconds=time.monotonic()-start, log=log.name, sha256=digest(log)))
        print(f"{name}: exit {result.returncode}", flush=True)
        if result.returncode != expected:
            raise RuntimeError(result.stdout[-16000:])
        return result.stdout

    try:
        build = output / "build"
        flags = "-Wall -Wextra -Werror"
        configure = [args.cmake, "-S", ROOT / "tests/read_many_serial", "-B", build, "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=" + args.cxx,
            "-DCMAKE_MAKE_PROGRAM=" + str(Path(shutil.which(args.ninja) or args.ninja).absolute()),
            "-DCMAKE_PREFIX_PATH=" + ";".join(map(str, prefixes)), "-DCOBS_ROOT=" + str(cobs)]
        if args.serial_prefix:
            configure += ["-DQt6SerialPort_DIR=" + str(args.serial_prefix.resolve() / "lib/cmake/Qt6SerialPort")]
        if args.sanitize:
            flags += " -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
            configure += ["-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG", "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined"]
        run("configure", configure + ["-DCMAKE_CXX_FLAGS=" + flags])
        run("build", [args.cmake, "--build", build, "-j", "2"])
        suffix = ".exe" if os.name == "nt" else ""

        def executable(name):
            candidates = list(build.rglob(name + suffix))
            if len(candidates) != 1:
                raise ValueError("Executable is missing or ambiguous: " + name)
            return candidates[0]

        bootstrap = executable("read_many_bootstrap_check")
        serial = executable("read_many_serial_check")
        cli = executable("telemetry-read-many-uart")
        # Exit zero is insufficient for a counted gate. Exercise the actual
        # subprocess path with silent, duplicate, zero and reduced reports.
        controls = 0
        for label, pattern, pty in (
                ('bootstrap', 'ReadMany bootstrap checks={} passed, hardware=0', None),
                ('serial', 'ReadMany Serial checks={} pty=' + str(int(os.name != 'nt')), os.name != 'nt')):
            good = pattern.format(2)
            for name, text in (
                    ('missing', ''), ('multiple', good + '\n' + good),
                    ('multiple-same-line', good + ' ' + good),
                    ('zero', pattern.format(0)), ('below-floor', pattern.format(1)),
                    ('wrong-profile', good[:-1] + ('1' if good[-1] == '0' else '0'))):
                emitted = run('report-control-' + label + '-' + name,
                    [sys.executable, '-B', '-c', 'import sys; sys.stdout.write(sys.argv[1])', text])
                try:
                    counted_result(emitted, label, 2, pty)
                except ValueError:
                    controls += 1
                else:
                    raise ValueError('Counted-report control accepted: ' + label + '/' + name)
            if counted_result(good + '\n', label, 2, pty) != 2:
                raise ValueError('Counted-report positive control failed: ' + label)
        report['counted_report_controls'] = dict(expected_rejections=controls, positive=2)
        bootstrap_output = run("bootstrap", [bootstrap])
        bootstrap_checks = counted_result(bootstrap_output, 'bootstrap', 4593)
        runtime = run("runtime", [serial, cli, output])
        pty = os.name != 'nt'
        serial_checks = counted_result(runtime, 'serial', 220 if pty else 1, pty)
        report["executions"] = dict(bootstrap_checks=bootstrap_checks, checks=serial_checks, pty=pty)
        run("invalid-arguments", [cli, "--report", output / "invalid.json"], expected=1)
        if json.loads((output / "invalid.json").read_text())["passed"]:
            raise ValueError("Invalid CLI arguments reported success")
        for name, values in (("negative", ["-1"]), ("excess", ["1801"]),
                             ("text", ["invalid"]), ("missing", []),
                             ("duplicate", ["1", "--load-seconds", "1"])):
            path = output / ("invalid-load-" + name + ".json")
            run("invalid-load-" + name,
                [cli, "--serial", "OFFLINE-NOT-OPENED", "--report", path,
                 "--load-seconds", *values], expected=1)
            invalid = json.loads(path.read_text())
            if invalid["passed"] or "--load-seconds requires" not in invalid.get("error", ""):
                raise ValueError("Invalid load arguments reached serial bootstrap: " + name)
        report["binary_sha256"] = {path.relative_to(output).as_posix(): digest(path) for path in (bootstrap, serial, cli)}
        report["passed"] = True
    finally:
        report["source_sha256_after"] = sources(cobs)
        if report["source_sha256_before"] != report["source_sha256_after"]:
            report["passed"] = False
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not report["passed"]:
        raise RuntimeError("Source inputs changed during qualification")


if __name__ == "__main__":
    main()
