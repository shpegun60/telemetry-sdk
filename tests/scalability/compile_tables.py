#!/usr/bin/env python3
"""Generate and measure repeated-DTO table/catalog/Model compilation profiles.

The generated program owns stable endpoint objects and checks every runtime
row. Table-only and full-Model builds separate local-table instantiation from
root collection. Shared targets and distinct target specializations are
explicitly different profiles. No generated source or binary enters the repo.
Timed runs are sequential Linux builds using GNU time; no board is accessed.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import signal
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
FAMILIES = ("field", "command", "service")
TYPES = {"field": "Field", "command": "Command", "service": "Service"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def library_inputs():
    return {path.relative_to(ROOT).as_posix(): digest(path)
            for path in sorted((ROOT / "lib").rglob("*"))
            if path.is_file() and path.suffix in {".h", ".hpp", ".cpp"}}


def program(rows, family, layout, shard_rows, targets):
    active = FAMILIES if family == "mixed" else (family,)
    width = rows if layout == "single" else min(shard_rows, rows)
    groups = (rows + width - 1) // width
    declarations = []
    for name in FAMILIES:
        table_type = TYPES[name]
        if name in active:
            for group in range(groups):
                entries = []
                for i in range(group * width, min((group + 1) * width, rows)):
                    methods = {"field": ("read", "write"), "command": ("configure",),
                               "service": ("echo",)}[name]
                    selected = [f"&Device::{method}" if targets == "shared" else
                                f"&Device::{method}Distinct<{i}>" for method in methods]
                    entries.append(f'    ts::{name}<{", ".join(selected)}>("{name}_{i:04}", devices[{i}])')
                declarations.append(f"inline constexpr ts::{table_type}Table {name}Table{group}{{\n" +
                                    ",\n".join(entries) + "\n};")
            group_args = [f'ts::testing::group("{name}_{i:04}", {name}Table{i})' for i in range(groups)]
        else:
            group_args = []
        declarations.append(f"inline constexpr ts::testing::{table_type}CatalogTable {name}s{{" +
                            ",\n    ".join(group_args) + "};")
    loops = []
    for name in active:
        if name == "field":
            operation = """
        const auto written = fields.index().writeEncoded(id, input, workspace);
        check(written.dispatch == ts::DispatchStatus::Ok &&
              written.endpointStatus == ts::WriteResult::Applied);
        check(same(devices[i].current, requested));
        const auto read = fields.index().readEncoded(id, output, workspace);
        check(read.dispatch == ts::DispatchStatus::Ok && read.written == input.size());
        check(output == input);
#ifdef SCALE_FULL_MODEL
        check(model.view().fieldTypeId(id) == model.typeId<Payload>());
#endif
"""
        elif name == "command":
            operation = """
        const auto result = commands.index().executeEncoded(id, input, workspace);
        check(result.dispatch == ts::DispatchStatus::Ok &&
              result.endpointStatus == ts::CommandResult::Executed);
        check(same(devices[i].current, requested));
#ifdef SCALE_FULL_MODEL
        check(model.view().commandTypeId(id) == model.typeId<Payload>());
#endif
"""
        else:
            operation = """
        const auto result = services.index().callEncoded(id, input, output, workspace);
        check(result.dispatch == ts::DispatchStatus::Ok &&
              result.endpointStatus == ts::ServiceStatus::Ok && result.written == input.size());
        const Payload expected{requested.value + devices[i].bias, requested.tag, requested.enabled};
        std::array<std::byte, ts::wireSize<Payload>> expectedBytes{};
        check(ts::encode(expected, expectedBytes) == ts::CodecStatus::Ok);
        check(output == expectedBytes);
#ifdef SCALE_FULL_MODEL
        const auto pair = model.view().serviceTypeIds(id);
        check(pair && pair->requestTypeId == model.typeId<Payload>() &&
              pair->responseTypeId == model.typeId<Payload>());
#endif
"""
        loops.append(f"""
    for (std::uint32_t i = 0; i < rows; ++i) {{
        const auto id = ts::PackedId((i / groupWidth) << 16 | (i % groupWidth));
        const Payload requested{{i * 17u + 3u, std::uint16_t(i), (i % 2u) != 0}};
        check(ts::encode(requested, input) == ts::CodecStatus::Ok);
{operation}
        check(workspace.used() == 0);
    }}
    check({name}s.index().find(ts::PackedId(groupCount << 16)) == nullptr);
""")
    native = []
    if "field" in active:
        native.append("check(fields.read<0>().has_value());")
    if "command" in active:
        native.append("check(commands.call<0>(Payload{11, 2, true}) == ts::CommandResult::Executed);")
    if "service" in active:
        native.append("check(services.call<0>(Payload{11, 2, true}).hasValue());")
    source = """/* Generated homogeneous endpoint scaling probe; inputs live outside the repository.
 * Public Device methods read/write native Payload, configure a Command, and echo a Service.
 * Distinct helpers deliberately create separate endpoint target specializations. */
#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <array>
#include <cstdio>

namespace ts = telemetry;
struct Payload {
    std::uint32_t value;
    std::uint16_t tag;
    bool enabled;
};
struct Device {
    Payload current{};
    std::uint32_t bias = 0;
    Payload read() const noexcept { return current; }
    ts::WriteResult write(const Payload& value) noexcept {
        current = value; return ts::WriteResult::Applied;
    }
    ts::CommandResult configure(const Payload& value) noexcept {
        current = value; return ts::CommandResult::Executed;
    }
    Payload echo(const Payload& value) const noexcept {
        return {value.value + bias, value.tag, value.enabled};
    }
    template<std::size_t> Payload readDistinct() const noexcept { return read(); }
    template<std::size_t> ts::WriteResult writeDistinct(const Payload& value) noexcept {
        return write(value);
    }
    template<std::size_t> ts::CommandResult configureDistinct(const Payload& value) noexcept {
        return configure(value);
    }
    template<std::size_t> Payload echoDistinct(const Payload& value) const noexcept {
        return echo(value);
    }
};
inline Device devices[ROWS];
""".replace("ROWS", str(rows))
    source += "\n".join(declarations)
    source += """
#ifdef SCALE_FULL_MODEL
inline constexpr ts::testing::V30Model model{fields, commands, services};
static_assert(decltype(model)::Registry::typeCount == 13);
// Keep the actual registry records reachable in the measured linked image.
[[gnu::noinline]] ts::TypeRegistryView observedRegistry() noexcept {
    return model.types();
}
#endif

unsigned checks = 0, failures = 0;
void check(bool condition) noexcept { ++checks; if (!condition) ++failures; }
bool same(const Payload& a, const Payload& b) noexcept {
    return a.value == b.value && a.tag == b.tag && a.enabled == b.enabled;
}
int main() {
    constexpr std::uint32_t rows = ROWS, groupWidth = WIDTH, groupCount = GROUPS;
    ts::Workspace workspace{std::span<std::byte>{}};
    [[maybe_unused]] std::array<std::byte, ts::wireSize<Payload>> input{}, output{};
    for (std::uint32_t i = 0; i < rows; ++i) devices[i].bias = i + 5;
""".replace("ROWS", str(rows)).replace("WIDTH", str(width)).replace("GROUPS", str(groups))
    source += "\n".join(loops)
    source += "\n    " + "\n    ".join(native)
    source += r"""
#ifdef SCALE_FULL_MODEL
    const auto registry = observedRegistry();
    check(registry.count == 13);
    const auto* payloadType = registry.find(model.typeId<Payload>());
    check(payloadType && payloadType->kind == ts::TypeKind::Struct &&
          payloadType->wireBytes == 7 && payloadType->memberCount == 3);
#endif
    std::printf("{\"checks\":%u,\"failures\":%u}\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
"""
    return source


def classify_failure(log):
    if re.search(r"template instantiation depth|recursive template instantiation exceeded", log, re.I):
        return "compiler-template-depth"
    if re.search(r"expression nesting limit|bracket nesting level", log, re.I):
        return "compiler-expression-depth"
    if re.search(r"constexpr.*(limit|maximum)|constant evaluation.*(limit|step)", log, re.I):
        return "compiler-constexpr-budget"
    if re.search(r"bad_alloc|out of memory|cannot allocate memory|allocation failed", log, re.I):
        return "memory-cap"
    if re.search(r"exceeds.*(ceiling|position space)|overflows u32", log, re.I):
        return "library-contract-ceiling"
    return "unexpected-compile-failure"


def measured(command, directory, label, timeout, memory_mib):
    metrics = directory / (label + ".metrics")
    log_path = directory / (label + ".log")
    wrapped = ["/usr/bin/time", "-f", "WALL_SECONDS=%e\nMAX_RSS_KIB=%M\nEXIT_CODE=%x",
               "-o", str(metrics), *map(str, command)]

    def restrict_memory():
        bound = memory_mib * 1024 * 1024
        resource.setrlimit(resource.RLIMIT_AS, (bound, bound))

    started = time.perf_counter()
    with log_path.open("w", encoding="utf-8") as log:
        log.write("COMMAND " + repr(list(map(str, command))) + "\n")
        log.flush()
        child = subprocess.Popen(wrapped, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True, preexec_fn=restrict_memory)
        try:
            code = child.wait(timeout=timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
            code, timed_out = -signal.SIGKILL, True
    duration = time.perf_counter() - started
    stats = metrics.read_text() if metrics.exists() else ""
    values = dict(re.findall(r"^(WALL_SECONDS|MAX_RSS_KIB|EXIT_CODE)=([^\n]+)", stats, re.M))
    return {"command": list(map(str, command)), "returncode": code, "timed_out": timed_out,
            "observed_wall_seconds": duration,
            "gnu_time_wall_seconds": float(values["WALL_SECONDS"]) if "WALL_SECONDS" in values else None,
            "peak_rss_kib": int(values["MAX_RSS_KIB"]) if "MAX_RSS_KIB" in values else None,
            "log": log_path.name}


def sections(path, size):
    output = subprocess.check_output([size, "-A", str(path)], text=True)
    detailed = {name: int(value) for name, value in
                re.findall(r"^(\.\S+)\s+(\d+)\s+\d+", output, re.M)}
    return {"families": {base: sum(value for name, value in detailed.items()
                                  if name == base or name.startswith(base + "."))
                          for base in (".text", ".rodata", ".data", ".bss")},
            "all_sections": detailed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--rows", type=int, nargs="+", default=[32, 128, 256, 512, 1024])
    parser.add_argument("--families", nargs="+", choices=[*FAMILIES, "mixed"], default=["field"])
    parser.add_argument("--layouts", nargs="+", choices=["single", "sharded"], default=["single", "sharded"])
    parser.add_argument("--stages", nargs="+", choices=["tables", "model"], default=["tables", "model"])
    parser.add_argument("--targets", choices=["shared", "distinct"], default="shared")
    parser.add_argument("--shard-rows", type=int, default=32)
    parser.add_argument("--optimization", choices=["O0", "O2", "Os"], default="O2")
    parser.add_argument("--template-depth", type=int)
    parser.add_argument("--bracket-depth", type=int)
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--memory-mib", type=int, default=4096)
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--require-pass", action="store_true",
                        help="Treat every attempted nonpassing profile as a failed validation run")
    parser.add_argument("--isolation-note", default="Not asserted; local smoke timings are not comparative evidence")
    parser.add_argument("--no-skip-after-compiler-limit", action="store_true",
                        help="Attempt larger default-budget profiles even after this same path hit its compiler budget")
    args = parser.parse_args()
    if os.name != "posix" or not Path("/usr/bin/time").is_file():
        parser.error("Run in Linux/WSL with GNU /usr/bin/time")
    if any(not 1 <= rows <= 65536 for rows in args.rows) or not 1 <= args.shard_rows <= 65536:
        parser.error("rows/shard-rows must fit the 16-bit local position capacity")
    if not 1 <= args.timeout <= 240 or not 1 <= args.memory_mib <= 4096:
        parser.error("timeout must be 1..240 seconds and memory-mib must be 1..4096")
    output = args.build_dir.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error("--build-dir must be outside the repository")
    output.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which(args.cxx)
    if compiler is None:
        parser.error("Compiler was not found: " + args.cxx)
    version = subprocess.check_output([compiler, "--version"], text=True)
    if args.bracket_depth is not None and "clang" not in version.lower():
        parser.error("--bracket-depth is a Clang-only diagnostic override")
    size = shutil.which("size")
    if not size:
        parser.error("GNU size was not found")
    inputs = library_inputs()
    report = {"source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "compiler": compiler, "compiler_version": version, "compiler_sha256": digest(Path(compiler)),
              "runner_sha256": digest(Path(__file__)), "input_hashes": inputs,
              "rows_meaning": "Endpoints per active family; mixed has three times this many endpoints",
              "memory_policy": "Per-process address-space cap; GNU time reports maximum resident set",
              "isolation_note": args.isolation_note,
              "memory_mib": args.memory_mib, "timeout_seconds": args.timeout,
              "profiles": []}
    flags = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic-errors",
             "-fdiagnostics-color=never", "-ftemplate-backtrace-limit=6", "-" + args.optimization,
             "-ffunction-sections", "-fdata-sections", "-Ilib", "-Ilib/boost_pfr/include",
             "-Ilib/magic_enum"]
    if args.template_depth is not None:
        flags.append(f"-ftemplate-depth={args.template_depth}")
    if args.bracket_depth is not None:
        flags.append(f"-fbracket-depth={args.bracket_depth}")
    flags.append("-ferror-limit=2" if "clang" in version.lower() else "-fmax-errors=2")
    limited_paths = {}
    for family in args.families:
        for rows in sorted(set(args.rows)):
            for layout in args.layouts:
                stem = f"{family}-{rows}-{layout}-{args.targets}"
                source = output / (stem + ".cpp")
                source.write_text(program(rows, family, layout, args.shard_rows, args.targets), encoding="utf-8")
                for stage in args.stages:
                    label = stem + "-" + stage
                    obj, image = output / (label + ".o"), output / label
                    active = FAMILIES if family == "mixed" else (family,)
                    record = {"name": label, "endpoints_per_family": rows,
                              "total_endpoints": rows * len(active), "family": family,
                              "layout": layout, "shard_rows": rows if layout == "single" else args.shard_rows,
                              "stage": stage, "targets": args.targets,
                              "owner_profile": "Same owner class; distinct stable owner instance per row",
                              "dto_profile": "One exact Payload shared by all active families",
                              "registry_unique_types": 13 if stage == "model" else None,
                              "source_sha256": digest(source), "source": source.name,
                              "optimization": args.optimization,
                              "root_occurrences": rows * sum(2 if name == "service" else 1 for name in active),
                              "template_depth_override": args.template_depth,
                              "bracket_depth_override": args.bracket_depth}
                    report["profiles"].append(record)
                    path_key = (family, layout, stage, args.targets)
                    if not args.generate_only and path_key in limited_paths and not args.no_skip_after_compiler_limit:
                        record["status"] = "not-run-after-compiler-limit"
                        record["earlier_profile"] = limited_paths[path_key]
                        print(f"{label}: {record['status']}", flush=True)
                    elif not args.generate_only:
                        definitions = ["-DSCALE_FULL_MODEL"] if stage == "model" else []
                        build = measured([*flags, *definitions, "-c", source, "-o", obj], output,
                                         label + "-compile", args.timeout, args.memory_mib)
                        record["compile"] = build
                        if build["returncode"] != 0:
                            log = (output / build["log"]).read_text(encoding="utf-8", errors="replace")
                            record["status"] = "timeout" if build["timed_out"] else classify_failure(log)
                            if (args.template_depth is None and args.bracket_depth is None and
                                    record["status"] in {"compiler-template-depth", "compiler-expression-depth",
                                                         "compiler-constexpr-budget"}):
                                limited_paths[path_key] = label
                        else:
                            record["object_sections"] = sections(obj, size)
                            link = measured([compiler, obj, "-Wl,--gc-sections", "-o", image], output,
                                            label + "-link", args.timeout, args.memory_mib)
                            record["link"] = link
                            if link["returncode"]:
                                record["status"] = "unexpected-link-failure"
                            else:
                                record["image_sha256"] = digest(image)
                                record["image_sections"] = sections(image, size)
                                run = subprocess.run([image], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                                     text=True, timeout=30)
                                (output / (label + "-execute.log")).write_text(run.stdout, encoding="utf-8")
                                record["correctness"] = json.loads(run.stdout)
                                weights = {"field": 6, "command": 4, "service": 5}
                                expected = rows * (sum(weights[name] for name in active) +
                                                   (len(active) if stage == "model" else 0))
                                expected += 2 * len(active) + (2 if stage == "model" else 0)
                                record["expected_checks"] = expected
                                record["status"] = "pass" if (run.returncode == 0 and
                                    record["correctness"] == {"checks": expected, "failures": 0}) else "correctness-failure"
                        print(f"{label}: {record['status']}", flush=True)
                    else:
                        record["status"] = "generated-only"
                    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if inputs != library_inputs():
        raise RuntimeError("Library inputs changed during the run; timing evidence is not coherent")
    if any(item["status"].startswith("unexpected-") or item["status"] == "correctness-failure"
           for item in report["profiles"]):
        raise SystemExit(1)
    if args.require_pass and any(item["status"] not in {"pass", "generated-only"}
                                 for item in report["profiles"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
