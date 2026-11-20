#!/usr/bin/env python3
"""Repeat generated SDK, native API, cross-language and offline cost checks.

All outputs belong in an external build directory. Node and TypeScript are
explicit tools, never downloaded by this runner. ARM numbers describe offline
compiler output; benchmark times describe this host, not an STM32 measurement.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(ROOT), str(HERE)]


def source_snapshot():
    """Hash execution inputs; documentation edits do not change this code receipt."""
    roots = ("tools/telemetry_codegen", "tests/codegen", "tests/structured/descriptor", "web",
             "examples/structured_client", "examples/codegen_cpp", "examples/codegen_encoded",
             "examples/codegen_rpc", "examples/codegen_web", "lib/telemetry", "lib/resource",
             "lib/client", "lib/common", "lib/wire", "lib/integration",
             "lib/delegate", "lib/boost_pfr/include", "lib/magic_enum")
    extensions = {".py", ".cpp", ".h", ".hpp", ".js", ".mjs", ".mts", ".ts", ".hex", ".json"}
    return {str(path.relative_to(ROOT)).replace("\\", "/"): hashlib.sha256(path.read_bytes()).hexdigest()
            for root in roots for path in sorted((ROOT / root).rglob("*"))
            if path.is_file() and path.suffix in extensions and "__pycache__" not in path.parts}


def tool_snapshot(args):
    """Record selected executable/TypeScript bytes, not a hermetic toolchain closure."""
    selected = [args.cxx, args.node, args.tsc, sys.executable, args.arm_cxx, args.browser_python,
                shutil.which("arm-none-eabi-size"), shutil.which("arm-none-eabi-nm")]
    paths = set()
    for command in selected:
        if command:
            path = Path(shutil.which(command) or command)
            if path.is_file():
                paths.add(path.resolve())
    tsc = Path(args.tsc)
    if tsc.is_file():
        lib = tsc.resolve().parent.parent / "lib"
        paths.update(path for path in lib.glob("*") if path.is_file() and
                     (path.suffix == ".ts" or path.name in ("tsc.js", "_tsc.js", "typescript.js")))
    return {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(paths)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--node", default=os.environ.get("NODE", "node"))
    parser.add_argument("--tsc", default=os.environ.get("TSC", "tsc"))
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--arm-cxx", help="also measure Cortex-M7 standalone/native objects")
    parser.add_argument("--browser-python", help="Python with Playwright and Chromium installed")
    parser.add_argument("--large-arm-only", action="store_true",
                        help="append only the large DTO ARM gate to an already passed build")
    args = parser.parse_args()
    build = Path(args.build_dir).resolve()
    if build == ROOT or ROOT in build.parents:
        parser.error("use an external build directory")
    build.mkdir(parents=True, exist_ok=True)
    (build / "package.json").write_text('{"type":"module"}\n')
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    report = dict(commands=[], executions=[], negative_controls=[], measurements={}, passed=False)
    if args.large_arm_only:
        if not args.arm_cxx:
            parser.error("--large-arm-only requires --arm-cxx")
        report = json.loads((build / "summary.json").read_text())
        if report.get("passed") is not True:
            parser.error("--large-arm-only requires a previously passed build")
        if any(item["name"].startswith("arm-large-stack-") for item in report["executions"]):
            parser.error("large ARM gates already recorded; use a fresh build for a repeat")
        report["passed"] = False
        report.setdefault("supplements", []).append(dict(mode="large-arm-only",
            started_at_utc=datetime.now(timezone.utc).isoformat(),
            original_checks=report["checks"]))

    # Load legitimate supplements first, then invalidate before input/tool hashing.
    (build / "summary.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(command, name, *, cwd=ROOT, expected_failure=False):
        command = list(map(str, command))
        started_at_utc = datetime.now(timezone.utc).isoformat()
        start = time.monotonic()
        result = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=600)
        elapsed = time.monotonic() - start
        (build / (name + ".log")).write_text(result.stdout, encoding="utf-8")
        report["commands"].append(dict(name=name, command=command, seconds=elapsed,
                                       returncode=result.returncode,
                                       expected_failure=expected_failure,
                                       started_at_utc=started_at_utc))
        if expected_failure:
            if result.returncode == 0:
                raise RuntimeError("intentional runtime mutation was not rejected: " + name)
            print(f"{name}: runtime refusal verified (exit {result.returncode})", flush=True)
        elif result.returncode:
            print(result.stdout, file=sys.stderr)
            raise RuntimeError(f"{name} failed with {result.returncode}")
        else:
            print(name + ": " + result.stdout.strip().split("\n")[-1], flush=True)
        return result.stdout

    def refused(command, name, pattern, *, expected_returncode=1, **evidence):
        output = run(command, name, expected_failure=True)
        if report["commands"][-1]["returncode"] != expected_returncode or not re.search(pattern, output):
            raise RuntimeError("runtime refusal did not reach its expected check: " + name)
        report["negative_controls"].append(dict(name=name,
            returncode=report["commands"][-1]["returncode"], **evidence))
        report["executions"].append(dict(name=name, checks=1))

    def mutate_copy(path, before, after):
        original_bytes = path.read_bytes()
        original = original_bytes.decode("utf-8")
        if original.count(before) != 1:
            raise RuntimeError("negative control requires one exact mutation: " + str(path))
        changed = original.replace(before, after, 1)
        changed_bytes = changed.encode("utf-8")
        path.write_bytes(changed_bytes)
        return dict(path=str(path), original_sha256=hashlib.sha256(original_bytes).hexdigest(),
                    mutated_sha256=hashlib.sha256(changed_bytes).hexdigest(),
                    before=before, after=after)

    cli = [sys.executable, "-m", "tools.telemetry_codegen"]
    flags = [args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror"]
    if args.sanitize:
        flags += ["-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    includes = ["-Ilib", "-Ilib/boost_pfr/include", "-Ilib/magic_enum"]
    abi = ROOT / "lib/telemetry/abi/StructuredAbi.cpp"
    native_sources = [abi, ROOT / "lib/telemetry/model/Adapter.cpp",
                      ROOT / "lib/resource/protocol/Protocol.cpp",
                      ROOT / "lib/integration/telemetry/v3/RpcMailbox.cpp",
                      ROOT / "lib/client/rpc/RpcClient.cpp"]

    def compile_program(source, name, extra=(), sdk=None):
        executable = build / (name + (".exe" if os.name == "nt" else ""))
        command = [*flags, *includes]
        if sdk is not None:
            command += ["-I" + str(sdk / "cpp")]
        run([*command, source, *extra, "-o", executable], "compile-" + name)
        return executable

    def counted(command, name):
        output = run(command, name)
        matches = re.findall(r"(?m)^[^\r\n]*\bchecks=(\d+)\s+failures=0\s*$", output)
        matches += re.findall(r"(?m)^generated web[^\r\n]*: (\d+) checks passed\s*$", output)
        if name == "cpp-encoded-rpc":
            floor = 400
        elif name == "cpp-standalone-native":
            floor = 95
        elif name.startswith("cpp-standalone"):
            floor = 80
        elif name.startswith("web-native-encoded-rpc"):
            floor = 155  # Includes the separately compiled injected-transport client.
        elif name.startswith("browser-"):
            floor = 84
        elif name.startswith("wire-editing-"):
            floor = 250
        elif name.startswith("names-parity-"):
            floor = 40  # Every type name and endpoint path of the fixture.
        elif name.startswith(("names-tree-", "names-aliases-")):
            floor = 5  # One per endpoint; the fixtures have 5 to 7 endpoints.
        else:
            floor = next((count for fixture, count in
                (("native", 1650), ("empty", 175), ("mixed", 850), ("edge", 1400), ("hostile", 600))
                if name.startswith("web-" + fixture + "-")), 20)
        if len(matches) != 1 or int(matches[0]) < floor:
            raise RuntimeError("missing positive counted test result: " + name)
        report["executions"].append(dict(name=name, checks=int(matches[0])))

    def large_arm():
        focused = build / "focused-cpp"
        source = HERE / "LargeProbe.cpp"
        inputs = [Path(__file__).resolve(), source, *sorted((focused / "cpp").rglob("*.hpp"))]
        if len(inputs) < 3:
            raise RuntimeError("large ARM gate requires the generated focused SDK")
        evidence = dict(wire_bytes=10001, frame_limit_bytes=256,
            stack_scope="individual compiler-reported frames, not cumulative call-chain or MCU measurement",
            inputs_sha256={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs},
            arm={})
        report["measurements"]["large_generated"] = evidence
        size, nm = shutil.which("arm-none-eabi-size"), shutil.which("arm-none-eabi-nm")
        if not size or not nm:
            raise RuntimeError("ARM size/nm tools required with --arm-cxx")
        for optimization in ("O2", "Os", "Og"):
            stem = "arm-large-" + optimization
            obj, elf = build / (stem + ".o"), build / (stem + ".elf")
            target = [args.arm_cxx, "-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard"]
            run([*target, "-std=c++20", "-" + optimization, "-fno-exceptions", "-fno-rtti",
                 "-ffunction-sections", "-fdata-sections", "-fstack-usage", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(focused / "cpp"), "-c", source, "-o", obj], "compile-" + stem)
            run([*target, "-nostdlib", "-Wl,-e,codegen_large_encode",
                 "-Wl,--undefined=codegen_large_decode", "-Wl,--gc-sections", "-Wl,--build-id=none",
                 obj, "-lgcc", "-lc", "-lnosys", "-o", elf], "link-" + stem)
            symbols = run([nm, "-C", elf], "symbols-" + stem)
            if re.search(r"\b(malloc|calloc|realloc|operator new|operator delete)\b", symbols):
                raise RuntimeError("allocation reference in large ARM codec")
            stack_usage = obj.with_suffix(".su").read_text()
            frames = []
            for line in stack_usage.splitlines():
                fields = line.rsplit("\t", 2)
                if len(fields) != 3 or not fields[1].isdigit() or fields[2] not in ("static", "dynamic,bounded"):
                    raise RuntimeError("unbounded/unknown ARM stack-usage record: " + line)
                frame = dict(function=fields[0], bytes=int(fields[1]), kind=fields[2])
                if frame["bytes"] > 256:
                    raise RuntimeError("large ARM individual stack frame exceeds 256 bytes: " + line)
                frames.append(frame)
            if not frames or not any("codegen_large_encode" in frame["function"] for frame in frames) or not any(
                    "codegen_large_decode" in frame["function"] for frame in frames):
                raise RuntimeError("large ARM stack report lacks both public roots")
            evidence["arm"][optimization] = dict(
                sections=run([size, "-A", elf], "sections-" + stem),
                stack_usage=stack_usage, frames=frames, maximum_individual_frame_bytes=max(f["bytes"] for f in frames),
                object_bytes=obj.stat().st_size, linked_bytes=elf.stat().st_size)
            report["executions"].append(dict(name="arm-large-stack-" + optimization, checks=1))

    snapshot = dict(started_at_utc=datetime.now(timezone.utc).isoformat(),
                    source_sha256_start=source_snapshot(), tool_sha256_start=tool_snapshot(args))
    report.setdefault("input_snapshots", []).append(snapshot)

    def verify_snapshot():
        snapshot["source_sha256_end"] = source_snapshot()
        snapshot["tool_sha256_end"] = tool_snapshot(args)
        snapshot["completed_at_utc"] = datetime.now(timezone.utc).isoformat()
        snapshot["unchanged"] = (snapshot["source_sha256_start"] == snapshot["source_sha256_end"] and
                                 snapshot["tool_sha256_start"] == snapshot["tool_sha256_end"])
        if not snapshot["unchanged"]:
            raise RuntimeError("source or selected tool input changed during qualification")

    try:
        if args.large_arm_only:
            large_arm()
            verify_snapshot()
            report["checks"] = sum(item["checks"] for item in report["executions"])
            report["supplements"][-1]["completed_at_utc"] = datetime.now(timezone.utc).isoformat()
            report["passed"] = True
            print(f"codegen supplemental checks={report['checks']} failures=0")
            return
        run([args.cxx, "--version"], "compiler-version")
        run([args.node, "--version"], "node-version")
        tsc = [args.node, args.tsc] if args.tsc.endswith((".js", "/tsc", "\\tsc")) else [args.tsc]
        run([*tsc, "--version"], "typescript-version")
        unit_output = run([sys.executable, "-m", "unittest", "discover", "-s", str(HERE),
                           "-p", "test_*.py", "-v"], "python-unit")
        unit_count = re.findall(r"(?m)^Ran (\d+) tests? in ", unit_output)
        if len(unit_count) != 1 or int(unit_count[0]) < 27 or not re.search(r"(?m)^OK(?:\s|$)", unit_output):
            raise RuntimeError("insufficient successful Python unit tests")
        report["executions"].append(dict(name="python-unit", checks=int(unit_count[0])))
        producer = compile_program(HERE / "Producer.cpp", "producer", [abi])
        native = build / "native"
        run([producer, native], "native-export")
        descriptor = native / "descriptor.bin"
        sdk = build / "generated"
        run([*cli, "inspect", descriptor], "inspect")
        run([*cli, "generate", descriptor, "--output", sdk], "generate")
        first = {str(p.relative_to(sdk)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in sdk.rglob("*") if p.is_file()}
        run([*cli, "generate", descriptor, "--output", sdk], "generate-repeat")
        assert first == {str(p.relative_to(sdk)): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sdk.rglob("*") if p.is_file()}, "nondeterministic generation"
        run([*cli, "generate", descriptor, "--output", sdk, "--check"], "generate-check")
        (native / "descriptor.hex").write_text(descriptor.read_bytes().hex())
        bridge = compile_program(HERE / "Bridge.cpp", "bridge", native_sources)
        integration = compile_program(HERE / "Integration.cpp", "integration", native_sources, sdk)
        counted([integration, native], "cpp-encoded-rpc")

        # Real Resource LIST/STAT/chunked READ, without an RPC client or parser clone.
        from tools.telemetry_codegen.resource import fetch
        with subprocess.Popen([str(bridge), "resource"], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True, env=env) as server:
            def exchange(packet):
                server.stdin.write(packet.hex() + "\n")
                server.stdin.flush()
                return bytes.fromhex(server.stdout.readline().strip())
            fetched = fetch(exchange)
            assert fetched == descriptor.read_bytes(), "native Resource descriptor mismatch"
            server.stdin.close()
            if server.wait(timeout=10) != 0:
                raise RuntimeError("Resource bridge failed")
        report["executions"].append(dict(name="native-resource-fetch", checks=1))

        focused = build / "focused-cpp"
        importlib.import_module("test_cpp").write_focused(focused)
        standalone = build / "standalone"
        odr = build / "Odr.cpp"
        odr.write_text('#include <sdk/device/Codec.hpp>\n'
                       '#ifdef CODEGEN_WITH_TELEMETRY\n#include <sdk/device/TelemetryBindings.hpp>\n#endif\n'
                       'const void* codegen_odr() noexcept { return &sdk::device::schema::fingerprint; }\n')
        # Deliberately no Telemetry include paths in this compile.
        run([*flags, "-I" + str(focused / "cpp"), HERE / "Standalone.cpp", odr, "-o", standalone],
            "compile-standalone")
        counted([standalone], "cpp-standalone")
        standalone_native = build / "standalone-native"
        run([*flags, *includes, "-DCODEGEN_WITH_TELEMETRY", "-I" + str(focused / "cpp"),
             HERE / "Standalone.cpp", odr, "-o", standalone_native], "compile-standalone-native")
        counted([standalone_native], "cpp-standalone-native")

        noheap = build / ("standalone-noheap" + (".exe" if os.name == "nt" else ""))
        run([*flags, "-I" + str(focused / "cpp"), HERE / "NoHeap.cpp", "-o", noheap],
            "compile-noheap")
        counted([noheap], "cpp-standalone-noheap")
        heap_control = build / ("heap-control" + (".exe" if os.name == "nt" else ""))
        run([*flags, "-I" + str(focused / "cpp"), "-DCODEGEN_HEAP_CONTROL",
             HERE / "NoHeap.cpp", "-o", heap_control], "compile-heap-control")
        refused([heap_control], "negative-heap-allocation", r"codegen heap allocation refused",
                expected_returncode=86)

        # Compile succeeds; canonical-byte assertions must reject this Writer.
        mutated_cpp = build / "negative-cpp"
        shutil.copytree(focused, mutated_cpp, dirs_exist_ok=True)
        mutation = mutate_copy(mutated_cpp / "cpp/sdk/device/Codec.hpp",
            "std::memcpy(bytes.data() + cursor, &value, sizeof(U));",
            "for (std::size_t i = 0; i < sizeof(U); ++i) "
            "bytes[cursor + i] = std::byte(static_cast<unsigned char>(value >> (8 * (sizeof(U) - 1 - i))));")
        mutation["meaning"] = "reverse scalar bytes in the executed little-endian Writer fast path"
        mutated_program = build / ("standalone-wrong-endian" + (".exe" if os.name == "nt" else ""))
        run([*flags, "-I" + str(mutated_cpp / "cpp"), HERE / "Standalone.cpp", odr,
             "-o", mutated_program], "compile-negative-cpp-endian")
        refused([mutated_program], "negative-cpp-endian",
                r"codegen standalone checks=[1-9]\d* failures=[1-9]\d*", **mutation)

        from tools.telemetry_codegen.cli import publish, render
        from tools.telemetry_codegen.schema import load_schema
        from test_web import consumer_source, hostile_descriptor
        import names
        fixtures = {"native": descriptor.read_bytes(), "hostile": hostile_descriptor()}
        for name in ("empty", "mixed", "edge"):
            fixtures[name] = bytes.fromhex((ROOT / "tests/structured/descriptor" / (name + ".hex")).read_text())
        # Descriptor v3.1: type names (flat), the group tree with and without names,
        # and attributes, which host outputs never read.
        for name in ("named", "tree", "tree_unnamed", "attrs"):
            fixtures[name] = names.fixture(name)

        # Compile the actual injected-transport example, then execute it in each
        # generated runtime's native Bridge session (IDs1..5 belong to that session).
        example = build / "web-example"
        example.mkdir(exist_ok=True)
        shutil.copy2(ROOT / "examples/codegen_web/client.ts", example / "client.ts")
        for name in ("transport.mjs", "transport.d.mts"):
            # Relocate the thin compatibility adapter, preserving the canonical runtime.
            text = (ROOT / "examples/codegen_web" / name).read_text().replace(
                "../../lib/client/web/Client.js", "./runtime/Client.js")
            (example / name).write_text(text)
        (example / "runtime").mkdir(exist_ok=True)
        for name in ("Client.js", "Client.d.ts"):
            shutil.copy2(ROOT / "lib/client/web" / name, example / "runtime" / name)
        publish(render(load_schema(descriptor.read_bytes()), ("js",)), example / "generated")
        example_compiled = example / "compiled"
        run([*tsc, "--strict", "--target", "ES2022", "--module", "NodeNext",
             "--moduleResolution", "NodeNext", "--outDir", example_compiled,
             example / "client.ts"], "compile-example-ts")
        shutil.copy2(example / "transport.mjs", example_compiled / "transport.mjs")
        (example_compiled / "runtime").mkdir(exist_ok=True)
        shutil.copy2(example / "runtime/Client.js", example_compiled / "runtime/Client.js")
        example_runtime = example_compiled / "generated/js"
        example_runtime.mkdir(parents=True, exist_ok=True)
        shutil.copy2(example / "generated/js/device.js", example_runtime / "device.js")

        for name, data in fixtures.items():
            target = build / ("web-" + name)
            schema = load_schema(data)
            publish(render(schema, ("js", "ts")), target)
            consumer = target / "Consumer.ts"
            consumer.write_text(consumer_source(schema))
            run([*tsc, "--strict", "--target", "ES2022", "--module", "ES2022",
                 "--moduleResolution", "node", "--noEmit", consumer], "type-consumer-" + name)
            hex_path = build / (name + ".hex")
            hex_path.write_text(data.hex())
            compiled = build / ("ts-" + name)
            run([*tsc, "--strict", "--target", "ES2022", "--module", "ES2022",
                 "--moduleResolution", "node", "--declaration", "--outDir", compiled,
                 target / "ts/device.ts"], "compile-ts-" + name)
            for kind, module in (("js", target / "js/device.js"), ("ts", compiled / "device.js")):
                counted([args.node, HERE / "Check.mjs", module, hex_path], "web-" + name + "-" + kind)
                if name == "native":
                    counted([args.node, HERE / "WebIntegration.mjs", module, native, bridge,
                             example_compiled / "client.js"],
                            "web-native-encoded-rpc-" + kind)
                    if args.browser_python:
                        counted([args.browser_python, HERE / "Browser.py", "--module", module,
                                 "--fixtures", native], "browser-" + kind)

        # Descriptor v3.1 names and group tree (tests/codegen/names.py): a native Model
        # rebuilt from the generated DTOs re-emits its input byte for byte, named and
        # unnamed; Dynamic (RuntimeSchema) and Static (metadata, typeName<T>()) agree
        # on every name and path; flat-era code compiles against the tree's SDK.
        from tools.telemetry_codegen.generators.cpp import _reachable
        names_build = build / "names"
        rebuild_inputs = {name: names.fixture(name) for name in
                          ("named", "tree", "tree_unnamed", "flat_roots", "tree_flat_roots",
                           "mixed", "edge", "empty")}
        rebuild_inputs["native"] = descriptor.read_bytes()
        for name, data in rebuild_inputs.items():
            schema = load_schema(data)
            target = names_build / name
            publish(render(schema, ("cpp", "ts"), client_runtime=True, structural_metadata=True), target)
            (names_build / (name + ".bin")).write_bytes(data)
            source = names_build / (name + "-Rebuild.cpp")
            source.write_text(names.rebuild_source(schema), encoding="utf-8")
            program = compile_program(source, "names-rebuild-" + name, [abi], target)
            emitted = names_build / (name + "-rebuilt.bin")
            run([program, emitted], "names-rebuild-run-" + name)
            if emitted.read_bytes() != data:
                raise RuntimeError("native model rebuilt from generated DTOs changed the descriptor: " + name)
            report["executions"].append(dict(name="names-rebuild-" + name, checks=1))
            if schema.minor >= 1:
                source = names_build / (name + "-Parity.cpp")
                source.write_text(names.parity_source(schema, _reachable(schema)), encoding="utf-8")
                program = compile_program(source, "names-parity-" + name, (), target)
                counted([program, names_build / (name + ".bin")], "names-parity-" + name)
            if schema.groups is not None:
                source = names_build / (name + "-Tree.cpp")
                source.write_text(names.tree_source(schema), encoding="utf-8")
                program = compile_program(source, "names-tree-" + name, (), target)
                counted([program], "names-tree-" + name)
                (target / "Usage.ts").write_text(names.usage_typescript(schema), encoding="utf-8")
                run([*tsc, "--strict", "--target", "ES2022", "--module", "ES2022", "--moduleResolution", "node",
                     "--noEmit", target / "Usage.ts"], "type-names-usage-" + name)
                report["executions"].append(dict(name="type-names-usage-" + name, checks=1))
        flat_schema = load_schema(rebuild_inputs["flat_roots"])
        alias_cpp = names_build / "FlatAliases.cpp"
        alias_cpp.write_text(names.alias_source(flat_schema), encoding="utf-8")
        for name in ("flat_roots", "tree_flat_roots"):
            program = compile_program(alias_cpp, "names-aliases-" + name, (), names_build / name)
            counted([program], "names-aliases-" + name)
            (names_build / name / "Aliases.ts").write_text(names.alias_typescript(flat_schema), encoding="utf-8")
            run([*tsc, "--strict", "--target", "ES2022", "--module", "ES2022", "--moduleResolution", "node",
                 "--noEmit", names_build / name / "Aliases.ts"], "type-names-aliases-" + name)
            report["executions"].append(dict(name="type-names-aliases-" + name, checks=1))
        # Attributes: the codecs build; TelemetryBindings.hpp is refused, naming every tag.
        attributed = names_build / "attrs"
        publish(render(load_schema(names.fixture("attrs")), ("cpp",)), attributed)
        for header, expect in (("Codec", True), ("TelemetryBindings", False), ("RpcBindings", False)):
            probe = names_build / ("Attrs" + header + ".cpp")
            probe.write_text(f"#include <device_api/{header}.hpp>\nint main() {{ return 0; }}\n")
            command = [*flags, *includes, "-I" + str(attributed / "cpp"), "-fsyntax-only", probe]
            if expect:
                run(command, "compile-names-attrs-" + header)
                report["executions"].append(dict(name="compile-names-attrs-" + header, checks=1))
            else:
                refused(command, "negative-names-attrs-" + header,
                        r"TelemetryBindings\.hpp is refused: the descriptor carries attribute tags 0x3e "
                        r"\(SDK advisory, first on type 13\), 0x3f .*0x40 \(application, first on type 15\)")

        # Wire editing is opt-in: isolated outputs leave the ordinary modules above
        # intact. Execute both JS and strict-compiled TS against native Descriptor.
        wire_editing = build / "wire-editing"
        run([*cli, "generate", descriptor, "--targets", "js,ts", "--structural-metadata",
             "--wire-editing", "--output", wire_editing], "generate-wire-editing")
        wire_consumer = wire_editing / "Consumer.ts"
        wire_consumer.write_text(consumer_source(load_schema(descriptor.read_bytes())) +
            "function preserving<T>(codec: J.ValueCodec<T>, value: T, seed: J.ByteInput): Uint8Array {\n"
            "  const encoded: Uint8Array = codec.encodePreserving(value, seed);\n"
            "  // @ts-expect-error the original payload must be bytes\n"
            "  codec.encodePreserving(value, 'invalid');\n"
            "  return encoded;\n}\n"
            "import * as Legacy from '../web-native/js/device.js';\n"
            "// @ts-expect-error ordinary generation has no wire-editing method\n"
            "Legacy.codecs.Type_10.encodePreserving(NaN, new Uint8Array(4));\n")
        run([*tsc, "--strict", "--target", "ES2022", "--module", "ES2022",
             "--moduleResolution", "node", "--noEmit", wire_consumer], "type-wire-editing")
        wire_compiled = build / "ts-wire-editing"
        run([*tsc, "--strict", "--target", "ES2022", "--module", "ES2022",
             "--moduleResolution", "node", "--declaration", "--outDir", wire_compiled,
             wire_editing / "ts/device.ts", wire_editing / "ts/device.metadata.ts"],
            "compile-ts-wire-editing")
        for kind, module, metadata in (
                ("js", wire_editing / "js/device.js", wire_editing / "js/device.metadata.js"),
                ("ts", wire_compiled / "device.js", wire_compiled / "device.metadata.js")):
            counted([args.node, HERE / "WireEditing.mjs", module, build / "native.hex", metadata,
                     build / "web-native/js/device.js"], "wire-editing-" + kind)

        # A copied opt-in codec still loads, but losing one IEEE byte must fail
        # an independently constructed raw-bit assertion in the maintained test.
        mutated_wire = build / "negative-wire-editing"
        mutated_wire.mkdir(exist_ok=True)
        shutil.copy2(wire_editing / "js/device.js", mutated_wire / "device.js")
        mutation = mutate_copy(mutated_wire / "device.js",
            "for (let i = 0; i < 4; ++i) view.setUint8(offset + i, original.getUint8(offset + i));",
            "for (let i = 0; i < 4; ++i) view.setUint8(offset + i, 0);")
        refused([args.node, HERE / "WireEditing.mjs", mutated_wire / "device.js", build / "native.hex",
                 wire_editing / "js/device.metadata.js", build / "web-native/js/device.js"],
                "negative-wire-editing-bits", r"AssertionError.*ERR_ASSERTION", **mutation)

        # Node imports successfully; independent canonical-byte comparisons must
        # reject the deliberately changed U16 writer in this copied module.
        mutated_js = build / "negative-js"
        mutated_js.mkdir(exist_ok=True)
        shutil.copy2(build / "web-native/js/device.js", mutated_js / "device.js")
        mutation = mutate_copy(mutated_js / "device.js",
            "view.setUint16(offset, value, true);", "view.setUint16(offset, value, false);")
        refused([args.node, HERE / "Check.mjs", mutated_js / "device.js", build / "native.hex"],
                "negative-js-endian", r"AssertionError.*ERR_ASSERTION", **mutation)

        measurements = report["measurements"]
        for kind in ("generated", "native"):
            defines = ["-DCODEGEN_NATIVE"] if kind == "native" else []
            program = build / ("benchmark-" + kind)
            start = time.monotonic()
            run([*flags, *includes, "-I" + str(sdk / "cpp"), *defines,
                 "-DCODEGEN_BENCHMARK", HERE / "Probe.cpp", "-o", program], "compile-benchmark-" + kind)
            entry = dict(compile_seconds=time.monotonic() - start, binary_bytes=program.stat().st_size)
            entry["host_samples"] = [json.loads(run([program], f"benchmark-{kind}-{i}")) for i in range(3)]
            measurements[kind] = entry
            if args.arm_cxx:
                size = shutil.which("arm-none-eabi-size")
                nm = shutil.which("arm-none-eabi-nm")
                if not size or not nm:
                    raise RuntimeError("ARM size/nm tools required with --arm-cxx")
                entry["arm"] = {}
                for optimization in ("O2", "Os", "Og"):
                    stem = "arm-" + kind + "-" + optimization
                    obj, elf = build / (stem + ".o"), build / (stem + ".elf")
                    target = [args.arm_cxx, "-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard"]
                    run([*target, "-std=c++20", "-" + optimization, "-fno-exceptions", "-fno-rtti",
                         "-ffunction-sections", "-fdata-sections", "-fstack-usage", "-Wall", "-Wextra", "-Werror",
                         *includes, "-I" + str(sdk / "cpp"), *defines, "-c", HERE / "Probe.cpp", "-o", obj],
                        "compile-" + stem)
                    run([*target, "-nostdlib", "-Wl,-e,codegen_encode", "-Wl,--undefined=codegen_decode",
                         "-Wl,--gc-sections", "-Wl,--build-id=none", obj, "-lgcc", "-lc", "-lnosys", "-o", elf],
                        "link-" + stem)
                    symbols = run([nm, "-C", elf], "symbols-" + stem)
                    if re.search(r"\b(malloc|calloc|realloc|operator new|operator delete)\b", symbols):
                        raise RuntimeError("allocation reference in ARM codec")
                    entry["arm"][optimization] = dict(
                        sections=run([size, "-A", elf], "sections-" + stem),
                        stack_usage=obj.with_suffix(".su").read_text(), object_bytes=obj.stat().st_size,
                        linked_bytes=elf.stat().st_size)
        if args.arm_cxx:
            large_arm()
        verify_snapshot()
        report["checks"] = sum(item["checks"] for item in report["executions"])
        report["passed"] = True
        print(f"codegen checks={report['checks']} failures=0")
    finally:
        (build / "summary.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
