#!/usr/bin/env python3
"""Compile and execute the complete C++20 user-guide examples.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
ARM mode compiles and links only; it never connects to a board.
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


ROOT = Path(__file__).resolve().parents[2]
EXAMPLES = ROOT / "examples/user_guide"
SUPPORT = {
    "NativeOnly": [],
    "QuickStart": ["lib/telemetry/abi/StructuredAbi.cpp", "lib/telemetry/model/Adapter.cpp"],
    "Native": ["lib/telemetry/abi/StructuredAbi.cpp", "lib/telemetry/model/Adapter.cpp"],
    "Resources": ["lib/resource/protocol/Protocol.cpp"],
    "ResourceClient": ["lib/resource/protocol/Protocol.cpp"],
    "Ergonomics": ["lib/telemetry/abi/StructuredAbi.cpp"],
    "FlatNative": ["lib/telemetry/abi/StructuredAbi.cpp"],
    "Codec": [],
    "Encoded": ["lib/telemetry/abi/StructuredAbi.cpp", "lib/telemetry/model/Adapter.cpp"],
}
PROJECT = ROOT / "examples/device_integration"
PROJECT_SOURCES = [PROJECT / name for name in ("main.cpp", "Device.cpp", "Api.cpp")]
PROJECT_HEADERS = [PROJECT / name for name in ("Device.hpp", "Api.hpp")]
# The branch-file layout: a header-only device whose SDK side needs no compiled source.
TREE = ROOT / "examples/group_tree"
TREE_SOURCES = [TREE / "main.cpp"]
TREE_HEADERS = [TREE / name for name in ("Telemetry.hpp", "adc/Adc.hpp", "adc/AdcTelemetry.hpp",
                                         "metrology/Meter.hpp", "metrology/MetrologyTelemetry.hpp")]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_quick_start_parity():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    start, end = "<!-- quickstart:begin -->", "<!-- quickstart:end -->"
    if readme.count(start) != 1 or readme.count(end) != 1:
        raise RuntimeError("Expected exactly one marked README quick start")
    marked = readme.split(start, 1)[1].split(end, 1)[0]
    block = re.fullmatch(r"\s*```cpp\n(.*?)\n```\s*", marked, re.S)
    source = (EXAMPLES / "QuickStart.cpp").read_text(encoding="utf-8")
    if not block or block[1] + "\n" != source:
        raise RuntimeError("README quick start differs from QuickStart.cpp")
    return hashlib.sha256(source.encode("utf-8")).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--null-checks", action="store_true")
    parser.add_argument("--arm", action="store_true")
    args = parser.parse_args()
    if args.arm and args.sanitize:
        parser.error("--sanitize is a host execution mode")
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Compiler discovery can fail before the detailed report's try/finally.
    (output / "summary.json").write_text('{"completed": false}\n', encoding="utf-8")
    # Preserve the C++ driver spelling when clang++ is a symlink to clang.
    compiler = Path(shutil.which(args.cxx) or args.cxx).absolute()
    environment = os.environ.copy()
    environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=1:detect_stack_use_after_return=1")
    environment.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    report = {
        "compiler": str(compiler), "compiler_sha256": sha256(compiler),
        "execution": "compile/link only" if args.arm else "host",
        "sanitize": args.sanitize, "null_checks": args.null_checks,
        "assertions_enabled": False, "commands": 0, "builds": 0,
        "executed_examples": 0, "conditions": 0, "examples": {},
        "completed": False,
    }

    def run(command, label, *, input_text=None):
        command = list(map(str, command))
        started = time.monotonic()
        result = subprocess.run(command, cwd=ROOT, env=environment, input=input_text,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                encoding="utf-8", errors="replace", timeout=180)
        (output / (label + ".log")).write_text(
            f"COMMAND {command!r}\nEXIT {result.returncode}\n"
            f"SECONDS {time.monotonic() - started:.6f}\n{result.stdout}", encoding="utf-8")
        report["commands"] += 1
        if result.returncode:
            raise RuntimeError(label + ": command failed\n" + result.stdout)
        print(label + ": pass", flush=True)
        return result.stdout

    flags = [compiler, "-std=c++20", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
             "-pedantic-errors", "-fdiagnostics-color=never", "-Ilib",
             "-Ilib/boost_pfr/include", "-Ilib/magic_enum"]
    if args.null_checks:
        flags += ["-fno-delete-null-pointer-checks"]
    if args.sanitize:
        flags += ["-g", "-fno-omit-frame-pointer",
                  "-fsanitize=address,undefined,float-cast-overflow",
                  "-fsanitize-address-use-after-scope", "-fno-sanitize-recover=all"]
    if args.arm:
        flags += ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard",
                  "-fno-exceptions", "-fno-rtti", "-ffunction-sections", "-fdata-sections"]

    try:
        if {path.stem for path in EXAMPLES.glob("*.cpp")} != set(SUPPORT):
            raise RuntimeError("User-guide example coverage changed")
        report["readme_quickstart_lf_sha256"] = check_quick_start_parity()
        report["compiler_version"] = run([compiler, "--version"], "compiler").strip()
        macros = run([compiler, "-std=c++20", "-UNDEBUG", "-dM", "-E", "-x", "c++", "-"],
                     "assertions-enabled", input_text="#include <cassert>\n")
        if re.search(r"^#define NDEBUG\b", macros, re.M) or not re.search(r"^#define assert\(", macros, re.M):
            raise RuntimeError("Runtime assertions are disabled")
        report["assertions_enabled"] = True
        programs = {name: ([EXAMPLES / (name + ".cpp")],
                           [ROOT / relative for relative in support], [])
                    for name, support in SUPPORT.items()}
        if {path.name for path in PROJECT.glob("*.cpp")} != {path.name for path in PROJECT_SOURCES}:
            raise RuntimeError("Device integration source coverage changed")
        programs["DeviceIntegration"] = (
            PROJECT_SOURCES,
            [ROOT / relative for relative in
             ("lib/telemetry/abi/StructuredAbi.cpp", "lib/telemetry/model/Adapter.cpp",
              "lib/resource/protocol/Protocol.cpp")],
            [*PROJECT_HEADERS, PROJECT / "CMakeLists.txt"])
        if ({path.relative_to(TREE) for path in TREE.rglob("*.[ch]pp")} !=
                {path.relative_to(TREE) for path in [*TREE_SOURCES, *TREE_HEADERS]}):
            raise RuntimeError("Group tree source coverage changed")
        programs["GroupTree"] = (TREE_SOURCES, [], [*TREE_HEADERS, TREE / "CMakeLists.txt"])
        for name, (sources, support, metadata) in programs.items():
            inputs = [*sources, *support]
            entry = {"input_sha256": {str(path.relative_to(ROOT)): sha256(path)
                                      for path in [*inputs, *metadata]},
                     "builds": [], "executed": False, "conditions": 0}
            report["examples"][name] = entry
            for optimization in (("O2", "Os") if args.arm else ("O1" if args.sanitize else "O2",)):
                program = output / (optimization + "-" + name +
                                    (".elf" if args.arm else ".exe" if os.name == "nt" else ""))
                link_flags = ["--specs=nano.specs", "--specs=nosys.specs", "-Wl,--gc-sections"] if args.arm else []
                run([*flags, "-" + optimization, *inputs, *link_flags, "-o", program],
                    "build-" + optimization + "-" + name)
                report["builds"] += 1
                entry["builds"].append({"optimization": optimization, "image_sha256": sha256(program)})
                if args.arm:
                    continue
                result = run([program], "execute-" + name)
                if name == "QuickStart":
                    if result.strip():
                        raise RuntimeError("QuickStart emitted unexpected output")
                    entry["assertions"] = "enabled; individual runtime assertion counts are not reported"
                elif name == "Native":
                    if result.strip() != "Native API guide: examples passed":
                        raise RuntimeError("Native example did not report completion")
                    entry["assertions"] = "enabled; individual runtime assertion counts are not reported"
                elif name == "ResourceClient":
                    if not result.rstrip().endswith("Resource client example passed"):
                        raise RuntimeError("Resource client example did not report completion")
                    entry["assertions"] = "enabled; error paths return nonzero"
                else:
                    prefix = {"Resources": "Resource", "Encoded": "Encoded",
                              "DeviceIntegration": "Device integration", "Ergonomics": "Ergonomics",
                              "FlatNative": "Flat native", "Codec": "Codec",
                              "NativeOnly": "Native only", "GroupTree": "Group tree"}[name]
                    rows = re.findall(r"^" + prefix + r" guide: (\d+) checks passed$", result, re.M)
                    if len(rows) != 1 or int(rows[0]) <= 0:
                        raise RuntimeError(name + ": expected one positive counted report")
                    entry["conditions"] = int(rows[0])
                    report["conditions"] += entry["conditions"]
                entry["executed"] = True
                report["executed_examples"] += 1
        report["completed"] = True
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in
                      ("commands", "builds", "executed_examples", "conditions", "execution", "assertions_enabled")}))


if __name__ == "__main__":
    main()
