#!/usr/bin/env python3
"""Endpoint handles by path: the generated (Static) and Dynamic clients, C++ and JS.

One native group-tree Model (tests/structured/group_tree) serves every client.
The same paths give the same values and the same refusals through the generated
device and DynamicClient::endpoint(path), in C++ (in process) and in JS (through
Bridge.cpp); the nested form and the path forms give the same handle. Strict TS
checks the path forms at compile time, the generated C++ refuses a wrong path or
value type at build time naming the path, and its path table stays out of the image.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
EXE = ".exe" if os.name == "nt" else ""

PROBES = {
    "unknown-path": ('(void)device.endpoint<"adc/channel9/Gain">();',
                     r'No endpoint at path P[^\n]*|Path<18>\{"adc/channel9/Gain"\}'),
    "read-of-command": ('(void)device.read<"adc/Reset">(tree_api::client::handles::Ignore{});',
                        r'read<P>\(\) takes a Field'),
    "call-of-field": ('(void)device.call<"adc/Config">(tree_api::client::handles::Ignore{});',
                      r'call<P>\(\) takes a Command or Service'),
    "wrong-value-type": ('(void)device.write<"adc/channel1/Gain">(tree_api::types::adc::Config{});',
                         r'"adc/channel1/Gain"'),
}
PROBE = """#include <tree_api/ClientBindings.hpp>
using Client = tree_api::client::DeviceClient<>;
void probe(Client& client) noexcept
{
	tree_api::client::Device<Client> device{client};
	@LINE@
}
"""
PATHS = """// The path forms keep no path text in the image: they resolve at compile time.
#include <tree_api/ClientBindings.hpp>
#include <integration/encoded/Backend.hpp>
#include <cstdio>
namespace cl = ::telemetry::client;
std::uint32_t now = 0;
int main()
{
	std::array<std::byte, 256> tx{}, rx{};
	const cl::Clock clock{nullptr, +[](void*) noexcept { return now; }};
	::telemetry::integration::encoded::EncodedBackend backend{tx, rx, clock};
	tree_api::client::DeviceClient<> client{backend, clock};
	tree_api::client::Device<tree_api::client::DeviceClient<>> device{client};
	const auto gain = device.endpoint<"adc/channel1/Gain">();
	const auto sent = device.write<"adc/channel1/Gain">(1.5f).status;
	std::printf("path-free image probe %u %d\\n", static_cast<unsigned>(gain.id()), static_cast<int>(sent));
	return gain == device.adc.channel1.Gain ? 0 : 1;
}
"""


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--node", default=os.environ.get("NODE", "node"))
    parser.add_argument("--tsc", required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    if build == ROOT or ROOT in build.parents and "build" not in build.relative_to(ROOT).parts[:1]:
        parser.error("use an external build directory or one under the ignored build/")
    build.mkdir(parents=True, exist_ok=True)
    (build / "package.json").write_text('{"type":"module"}\n')
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    report = dict(started_utc=datetime.now(timezone.utc).isoformat(), commands=[], executions=[],
                  negative_controls=[], passed=False)

    def run(command, name, expect_failure=False):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace", timeout=900)
        (build / (name + ".log")).write_text(result.stdout, encoding="utf-8")
        report["commands"].append(dict(name=name, command=command, returncode=result.returncode))
        if expect_failure != (result.returncode != 0):
            print(result.stdout[-6000:], file=sys.stderr)
            raise RuntimeError(name + (" was not refused" if expect_failure else " failed"))
        print(name + ": " + ("refused as intended" if expect_failure else
                             (result.stdout.strip().splitlines() or [""])[-1]), flush=True)
        return result.stdout

    def counted(command, name, floor):
        output = run(command, name)
        found = re.findall(r"(?m)^handles parity checks=(\d+) failures=0\s*$", output)
        if len(found) != 1 or int(found[0]) < floor:
            raise RuntimeError("missing counted result: " + name)
        report["executions"].append(dict(name=name, checks=int(found[0])))
        return sorted(line for line in output.splitlines() if line.startswith("error "))

    flags = [args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror"]
    if args.sanitize:
        flags += ["-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    includes = ["-I" + str(ROOT / "lib"), "-I" + str(ROOT / "lib/boost_pfr/include"),
                "-I" + str(ROOT / "lib/magic_enum")]
    native = [ROOT / "lib/telemetry/abi/StructuredAbi.cpp", ROOT / "lib/telemetry/model/Adapter.cpp",
              ROOT / "lib/integration/encoded/Dispatcher.cpp"]
    try:
        run([args.cxx, "--version"], "compiler-version")
        bridge = build / ("bridge" + EXE)
        run([*flags, *includes, HERE / "Bridge.cpp", *native, "-o", bridge], "compile-bridge")
        descriptor = build / "tree.bin"
        run([bridge, "--descriptor", descriptor], "native-descriptor")
        golden = bytes.fromhex((ROOT / "tests/structured/descriptor/tree.hex").read_text())
        if descriptor.read_bytes() != golden:
            raise RuntimeError("the Model's descriptor is not tree.hex")
        sdk = build / "sdk"
        run([sys.executable, "-B", "-m", "tools.telemetry_codegen", "generate", descriptor,
             "--targets", "cpp,js,ts", "--client-runtime", "--structural-metadata",
             "--namespace", "tree_api", "--output", sdk], "generate")
        sdk_include = "-I" + str(sdk / "cpp")

        parity = build / ("parity" + EXE)
        run([*flags, *includes, sdk_include, HERE / "Parity.cpp", *native, "-o", parity], "compile-parity")
        cpp_errors = counted([parity], "cpp-parity", 200)

        tsc = [args.node, args.tsc] if args.tsc.endswith((".js", "/tsc", "\\tsc")) else [args.tsc]
        (build / "Usage.ts").write_text((HERE / "Usage.ts").read_text(encoding="utf-8"), encoding="utf-8")
        run([*tsc, "--strict", "--target", "ES2022", "--module", "ES2022", "--moduleResolution", "node",
             "--noEmit", build / "Usage.ts"], "type-path-forms")
        report["executions"].append(dict(name="type-path-forms", checks=1))
        compiled = build / "compiled"
        run([*tsc, "--strict", "--target", "ES2022", "--module", "ES2022", "--moduleResolution", "node",
             "--declaration", "--outDir", compiled, sdk / "ts/handles.ts"], "compile-ts-handles")
        dynamic = ROOT / "web/DynamicClient.js"
        js_errors = counted([args.node, HERE / "Parity.mjs", dynamic, sdk / "js/handles.js",
                             sdk / "js/device.js", sdk / "js/client.js", descriptor, bridge], "js-parity", 60)
        ts_errors = counted([args.node, HERE / "Parity.mjs", dynamic, compiled / "handles.js",
                             compiled / "device.js", compiled / "client.js", descriptor, bridge], "ts-parity", 60)
        if not (cpp_errors == js_errors == ts_errors) or len(cpp_errors) != 9:
            raise RuntimeError("the refusals differ between C++ and JS:\n" +
                               "\n".join(cpp_errors) + "\n--\n" + "\n".join(js_errors))
        report["executions"].append(dict(name="refusal-wording-parity", checks=len(cpp_errors)))

        for name, (line, pattern) in PROBES.items():
            source = build / ("Probe-" + name + ".cpp")
            source.write_text(PROBE.replace("@LINE@", line))
            output = run([*flags, *includes, sdk_include, "-fsyntax-only", source],
                         "negative-" + name, expect_failure=True)
            if not re.search(pattern, output):
                raise RuntimeError("negative-" + name + " failed for another reason")
            report["negative_controls"].append(dict(name=name, pattern=pattern))
            report["executions"].append(dict(name="negative-" + name, checks=1))

        paths = build / "Paths.cpp"
        paths.write_text(PATHS)
        image = build / ("paths" + EXE)
        run([*flags, *includes, sdk_include, paths, *native, "-o", image], "compile-path-free-image")
        run([image], "run-path-free-image")
        content = image.read_bytes()
        if b"path-free image probe" not in content:
            raise RuntimeError("the image scan cannot see the probe's own text")
        if b"adc/channel1/Gain" in content or b"metrology/Config" in content:
            raise RuntimeError("a path string reached the image of the path forms")
        report["executions"].append(dict(name="path-free-image", checks=2))

        report["checks"] = sum(item["checks"] for item in report["executions"])
        report["passed"] = True
        report["inputs_sha256"] = {str(path.relative_to(ROOT)).replace("\\", "/"): digest(path)
                                   for path in sorted(HERE.iterdir()) if path.is_file()}
        print(f"client handles checks={report['checks']} failures=0")
    finally:
        (build / "summary.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
