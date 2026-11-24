#!/usr/bin/env python3
"""Measure bounded compiler costs for repeated and distinct structured types.

Generate comparable registry-only and complete Field/Codec/Model/Descriptor
translation units. Linux GNU time records compiler peak RSS; each process has
an address-space ceiling and timeout. Compiler failures remain evidence rather
than triggering silent template-depth/constexpr-limit changes. All generated
sources, images, logs and measurements belong to the explicit --build-dir.
No board, production source, vendor tree or existing test is modified.
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
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
BASELINE = "617d037a94e8761a6d70cdc45eafe97232a3c924"
CPP_PURPOSE = """/*
 * Generated structured-type scalability probe; owned by compile_types.py.
 * Shared subtypes are repeated intentionally; exact C++ root identities vary.
 * Each translation unit independently forces registry metadata and, for Model
 * profiles, encoded bindings plus a constexpr streaming/packed descriptor.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
"""
COMMON = r"""
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <type_traits>
namespace ts = telemetry;
enum class Mode : std::uint8_t { Off = 1, On = 5, Auto = 9 };
// An explicit three-entry dictionary keeps enum discovery cost constant.
template<> struct telemetry::reflection::EnumReflection<Mode> {
    inline static constexpr auto entries = telemetry::reflection::enumEntries(
        telemetry::reflection::enumEntry(Mode::Off, "Off"),
        telemetry::reflection::enumEntry(Mode::On, "On"),
        telemetry::reflection::enumEntry(Mode::Auto, "Auto"));
};
// Each DTO repeats this one nested subtype twice; only its identity is shared.
struct Common { std::uint32_t tick; std::uint16_t state; bool enabled; };
template<std::size_t Index> struct Dto {
    Common left;
    Common right;
    std::array<std::uint16_t, 4> samples;
    Mode mode;
};
static_assert(ts::wireSize<Dto<0>> == 23);
static_assert(ts::Type<Dto<0>>::depth == 2);
static_assert(ts::Type<Dto<0>>::expandedNodes == 15);
"""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_for(count, variant, layer):
    identities = list(range(count)) if variant == "distinct" else [0] * count
    expected = count + 15 if variant == "distinct" else 16
    roots = ", ".join(f"Dto<{index}>" for index in identities)
    text = CPP_PURPOSE + COMMON
    if layer == "registry":
        text += f"\nusing Registry = ts::TypeRegistry<{roots}>;\n"
    else:
        text += "\ntemplate<std::size_t Index> using Value = Dto<"
        text += "Index" if variant == "distinct" else "0"
        text += r""">;
template<std::size_t Index> Value<Index> read() noexcept { return {}; }
template<std::size_t Index> ts::WriteResult write(const Value<Index>&) noexcept {
    return ts::WriteResult::Applied;
}
inline constexpr ts::FieldTable rows{
"""
        text += ",\n".join(
            f'    ts::field<&read<{index}>, &write<{index}>>("Field{index:06d}")'
            for index in range(count))
        text += r"""
};
inline constexpr ts::testing::FieldCatalogTable fields{ts::testing::group("group", rows)};
inline constexpr ts::testing::ServiceCatalogTable<> services{};
inline constexpr ts::testing::V30Model model{fields, ts::testing::emptyCommands, services};
using Registry = decltype(model)::Registry;
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto packed = ::telemetry::descriptor::packDescriptor<descriptor>();
static_assert(descriptor.valid());
static_assert(model.maxFieldWireSize() == 23);
static_assert(model.maxScratch() == 0); // 28-byte DTO fits the frozen 32-byte local budget.
ts::ModelView scalability_model() noexcept { return model.view(); }
extern "C" const std::byte* scalability_packed() noexcept { return packed.data(); }
const auto& scalability_descriptor() noexcept { return descriptor; }
"""
    text += f"\nstatic_assert(Registry::typeCount == {expected});\n"
    text += r"""
ts::TypeRegistryView scalability_registry() noexcept { return Registry::view(); }
int main() {
    const auto types = scalability_registry();
    if (types.count != Registry::typeCount || types.recordsBytes != Registry::recordsBytes)
        return 1;
"""
    if layer == "model":
        text += r"""
    std::array<std::byte, 23> bytes{};
    ts::Workspace workspace{std::span<std::byte>{}};
    const auto view = scalability_model();
    const auto result = view.fields.readEncoded(0, bytes, workspace);
    if (result.dispatch != ts::DispatchStatus::Ok || result.written != bytes.size()) return 2;
    const auto applied = view.fields.writeEncoded(0, bytes, workspace);
    if (applied.dispatch != ts::DispatchStatus::Ok || applied.endpointStatus != ts::WriteResult::Applied)
        return 3;
    const auto& data = scalability_descriptor();
    if (!data.valid() || packed.size() != data.size() || packed[0] != std::byte{'T'}) return 4;
    std::printf("{\"type_count\":%u,\"records_bytes\":%u,\"descriptor_bytes\":%u,"
                "\"descriptor_index_bytes\":%zu,\"descriptor_native_bytes\":%zu,"
                "\"field_table_bytes\":%zu,\"max_scratch\":%u,\"dto_native_bytes\":%zu}\n",
                types.count, types.recordsBytes, data.size(), decltype(descriptor)::indexBytes,
                sizeof(descriptor), sizeof(rows), model.maxScratch(), sizeof(Dto<0>));
"""
    else:
        text += r"""
    std::printf("{\"type_count\":%u,\"records_bytes\":%u,\"type_descriptor_bytes\":%zu,"
                "\"dto_native_bytes\":%zu}\n", types.count, types.recordsBytes,
                sizeof(ts::TypeDescriptor), sizeof(Dto<0>));
"""
    return text + "    return 0;\n}\n"


def backend_boundary_sources():
    """Keep the pinned PFR capacity distinct from normalized Type ceilings."""
    cases = []
    for members in (200, 201):
        text = CPP_PURPOSE + "#include <boost/pfr/core.hpp>\n#include <cstdint>\n"
        text += "struct Members {\n"
        text += "\n".join(f"    std::uint8_t member{i:03d};" for i in range(members)) + "\n};\n"
        text += "Members value{};\nauto& first = boost::pfr::get<0>(value);\n"
        cases.append((f"pfr-members-{members}", text, members == 200,
                      "Boost.PFR: Too many fields in a structure T"))
    return cases


def boundary_sources():
    """Exercise actual Type ceilings and separately labeled tightened profiles."""
    prefix = CPP_PURPOSE + "#include <telemetry/type/Registry.hpp>\n#include <array>\n"
    cases = []
    for depth in (32, 33):
        text = prefix + "template<unsigned N> struct Nest { using T = std::array<typename Nest<N-1>::T, 1>; };\n"
        text += "template<> struct Nest<0> { using T = std::uint8_t; };\n"
        text += f"static_assert(telemetry::Type<typename Nest<{depth}>::T>::depth == {depth});\n"
        cases.append((f"depth-{depth}", text, depth == 32, "Type nesting depth exceeds supported limit"))
    for members in (256, 257):
        text = prefix + "struct Members {\n"
        text += "\n".join(f"    std::uint8_t member{i:03d};" for i in range(members)) + "\n};\n"
        text += f"static_assert(telemetry::Type<Members>::wireSize == {members});\n"
        cases.append((f"members-{members}", text, members == 256, "Struct has too many members"))
    for elements in (65536, 65537):
        text = prefix + f"using Array = std::array<std::uint8_t, {elements}>;\n"
        text += f"static_assert(telemetry::Type<Array>::wireSize == {elements});\n"
        cases.append((f"array-{elements}", text, elements == 65536, "Array has too many elements"))
    for extra in (False, True):
        text = prefix + "using Huge = std::array<std::array<std::uint64_t, 65536>, 2>;\n"
        if extra:
            text += "struct Value { Huge value; std::uint8_t tail; };\n"
        else:
            text += "using Value = Huge;\n"
        text += f"static_assert(telemetry::Type<Value>::wireSize == {1048576 + int(extra)});\n"
        cases.append((f"wire-{1048576 + int(extra)}", text, not extra, "Struct wire size exceeds supported limit"))
    for exact in (True, False):
        last = 65534 if exact else 65535
        text = prefix + "struct Nodes { std::array<std::uint8_t, 65535> a, b, c; "
        text += f"std::array<std::uint8_t, {last}> d; }};\n"
        text += f"static_assert(telemetry::Type<Nodes>::expandedNodes == {262144 if exact else 262145});\n"
        cases.append((f"nodes-{262144 if exact else 262145}", text, exact,
                      "Struct expanded nodes exceed supported limit"))
    # These profiles tighten Descriptor acceptance; they never replace the
    # TypeRegistry's frozen 4096-type ceiling. Do not describe them as testing it.
    model = source_for(4, "distinct", "model")
    for member, value, label, expected in (
            ("maxTypeCount", "Registry::typeCount", "profile-type-exact", True),
            ("maxTypeCount", "Registry::typeCount - 1", "profile-type-short", False),
            ("maxDescriptorBytes", "descriptor.size()", "profile-bytes-exact", True),
            ("maxDescriptorBytes", "descriptor.size() - 1", "profile-bytes-short", False)):
        text = model[:model.index("int main()")]
        text += f"struct Profile : ts::Limits {{ static constexpr unsigned {member} = {value}; }};\n"
        text += "using Checked = ::telemetry::descriptor::Descriptor<std::remove_cv_t<decltype(fields)>, ts::EmptyEndpointCatalog, "
        text += "std::remove_cv_t<decltype(services)>, Profile>;\nconstexpr Checked checked{model};\n"
        text += "static_assert(checked.valid());\n"
        text += "static_assert(checked.size() == descriptor.size());\n"
        text += "static_assert(checked.fingerprint() == descriptor.fingerprint());\n"
        diagnostic = "Descriptor type ceiling exceeded" if member == "maxTypeCount" else "descriptorSizeExceeded"
        cases.append((label, text, expected, diagnostic))
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--counts", type=int, nargs="+", default=[32, 128, 512])
    parser.add_argument("--compilers", nargs="+", default=["g++", "clang++-18"])
    parser.add_argument("--variants", nargs="+", choices=["shared", "distinct"], default=["shared", "distinct"])
    parser.add_argument("--layers", nargs="+", choices=["registry", "model"], default=["registry", "model"])
    parser.add_argument("--boundaries", action="store_true")
    parser.add_argument("--backend-boundaries", action="store_true",
                        help="Check pinned PFR capacity200 and its exact rejection above200")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--require-pass", action="store_true",
                        help="Fail CI for any unexpected compiler, link, execution or boundary outcome")
    parser.add_argument("--template-depth", type=int)
    parser.add_argument("--compiler-extra-flag", action="append", default=[],
                        help="Explicit raised-limit repro only; recorded verbatim, never added automatically")
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--memory-mib", type=int, default=4096)
    parser.add_argument("--baseline", default=BASELINE)
    args = parser.parse_args()
    if os.name != "posix" or not Path("/usr/bin/time").exists():
        parser.error("Run on Linux/WSL with GNU /usr/bin/time for compiler peak RSS")
    if any(not 1 <= count <= 1024 for count in args.counts):
        parser.error("Explicit counts must be in 1..1024; large cases remain separately bounded")
    if not 1 <= args.timeout <= 600 or not 256 <= args.memory_mib <= 8192:
        parser.error("Require timeout1..600s and memory256..8192MiB")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if head != args.baseline:
        parser.error("Current source HEAD differs from the requested baseline")
    out = args.build_dir.resolve()
    if out.is_relative_to(ROOT):
        parser.error("Generated sources and measurements must stay outside the repository")
    out.mkdir(parents=True, exist_ok=True)
    headers = [path for path in (ROOT / "lib").rglob("*") if path.suffix in {".h", ".hpp", ".cpp"}]
    before = {str(path.relative_to(ROOT)): digest(path) for path in sorted(headers)}
    jobs = []
    for count in args.counts:
        for variant in args.variants:
            for layer in args.layers:
                jobs.append({"name": f"{layer}-{variant}-{count}", "count": count,
                             "variant": variant, "layer": layer, "expected_success": True,
                             "source": source_for(count, variant, layer)})
    if args.boundaries:
        jobs += [{"name": name, "layer": "boundary", "expected_success": expected,
                  "diagnostic": diagnostic, "source": text}
                 for name, text, expected, diagnostic in boundary_sources()]
    if args.backend_boundaries:
        jobs += [{"name": name, "layer": "boundary", "expected_success": expected,
                  "diagnostic": diagnostic, "source": text}
                 for name, text, expected, diagnostic in backend_boundary_sources()]
    for job in jobs:
        path = out / (job["name"] + ".cpp")
        path.write_text(job.pop("source"), encoding="utf-8")
        job["source_path"] = str(path)
        job["source_sha256"] = digest(path)
    report = {"completed": False, "baseline": head, "memory_mib": args.memory_mib,
              "timeout_seconds": args.timeout, "template_depth_override": args.template_depth,
              "runner_sha256": digest(Path(__file__)), "invocation": sys.argv,
              "jobs": [], "source_sha256": before, "generated_jobs": jobs,
              "ceiling_coverage": {
                  "actual_type_boundaries": ["maxTypeDepth", "maxStructMembers", "maxArrayElements",
                                             "maxValueWireBytes", "maxExpandedValueNodes"] if args.boundaries else [],
                  "tightened_descriptor_profiles": ["maxTypeCount", "maxDescriptorBytes"] if args.boundaries else [],
                  "not_exercised": ["actual 4096-type count ceiling", "actual 4MiB descriptor ceiling",
                                    "65536 total enum entries", "maximum string length"]},
              "scope": "Compiler scalability; object sections are host layout, not MCU flash cycles"}
    summary = out / "summary.json"

    def save():
        summary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    save()
    if args.generate_only:
        print(f"Generated {len(jobs)} source profiles; no compiler run", flush=True)
        return

    def limits():
        resource.setrlimit(resource.RLIMIT_AS, (args.memory_mib * 1024 * 1024,) * 2)
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    def run(command, label, measured=False):
        metric_file = out / (label + ".time.txt")
        metric_file.unlink(missing_ok=True)
        actual = ["/usr/bin/time", "-f", "%e %M %U %S", "-o", str(metric_file), *command] if measured else command
        started = time.monotonic()
        process = subprocess.Popen(actual, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, errors="replace", start_new_session=True, preexec_fn=limits)
        timeout = False
        try:
            stdout, stderr = process.communicate(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timeout = True
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
        seconds = time.monotonic() - started
        log = out / (label + ".log")
        log.write_text(f"COMMAND {actual!r}\nEXIT {process.returncode}\nTIMEOUT {timeout}\nSECONDS {seconds:.6f}\n"
                       + stdout + stderr, encoding="utf-8")
        result = {"returncode": process.returncode, "timeout": timeout, "wall_seconds": seconds,
                  "log": str(log), "stdout": stdout, "stderr": stderr}
        if measured and metric_file.exists():
            matches = re.findall(r"^(\d+(?:\.\d+)?) (\d+) (\d+(?:\.\d+)?) (\d+(?:\.\d+)?)$",
                                 metric_file.read_text(), re.M)
            if matches:
                wall, rss, user, system = matches[-1]
                result.update(compiler_wall_seconds=float(wall), peak_rss_kib=int(rss),
                              cpu_user_seconds=float(user), cpu_system_seconds=float(system))
        return result

    flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic-errors",
             "-ftemplate-backtrace-limit=6",
             "-fdiagnostics-color=never", "-Ilib", "-Ilib/boost_pfr/include",
             "-Ilib/magic_enum", "-Ilib/delegate", "-ffunction-sections", "-fdata-sections"]
    if args.template_depth:
        flags += [f"-ftemplate-depth={args.template_depth}"]
    flags += args.compiler_extra_flag
    report["flags"] = flags
    for requested in args.compilers:
        # Preserve clang++'s invocation name: resolving its symlink to clang
        # would select the C driver when linking an object-only command.
        compiler = Path(shutil.which(requested) or requested).absolute()
        compiler_flags = flags + (["-ferror-limit=3"] if "clang" in compiler.name else ["-fmax-errors=3"])
        label = re.sub(r"[^a-zA-Z0-9_.-]", "_", requested)
        version = run([str(compiler), "--version"], label + "-version")
        if version["returncode"]:
            raise RuntimeError("Compiler version query failed")
        abi = out / (label + "-abi.o")
        support = run([str(compiler), *compiler_flags, "-c", str(ROOT / "lib/telemetry/abi/StructuredAbi.cpp"),
                       "-o", str(abi)], label + "-abi", measured=True)
        if support["returncode"]:
            raise RuntimeError("ABI support compilation failed")
        for job in jobs:
            name = label + "-" + job["name"]
            obj = out / (name + ".o")
            result = dict(job, compiler=str(compiler), compiler_sha256=digest(compiler),
                          compiler_version=version["stdout"].splitlines()[0])
            compile_result = run([str(compiler), *compiler_flags, "-c", job["source_path"], "-o", str(obj)],
                                 name + "-compile", measured=True)
            result["compile"] = {key: value for key, value in compile_result.items() if key not in {"stdout", "stderr"}}
            if compile_result["returncode"] or compile_result["timeout"]:
                output = compile_result["stdout"] + compile_result["stderr"]
                expected = not job["expected_success"] and not compile_result["timeout"] and job["diagnostic"] in output
                result["outcome"] = "expected-rejection" if expected else "compile-failure"
                result["diagnostic_excerpt"] = output[-6000:]
                result["failure_class"] = (
                    "timeout" if compile_result["timeout"] else
                    "address-space-ceiling" if re.search(r"(virtual memory exhausted|bad_alloc|out of memory)", output, re.I) else
                    "expression-depth" if re.search(r"(fold expression|expression nesting|bracket nesting)", output, re.I) else
                    "template-depth" if re.search(r"template.*(depth|recursion)", output, re.I) else
                    "constexpr-budget" if re.search(r"(constexpr.*(limit|depth)|step limit|evaluation.*steps)", output, re.I) else
                    "diagnostic-rejection")
            elif not job["expected_success"]:
                result["outcome"] = "unexpected-acceptance"
            else:
                result["object_sha256"] = digest(obj)
                sizes = run(["size", "-A", "-d", str(obj)], name + "-sections")
                if sizes["returncode"]:
                    raise RuntimeError("Object section inspection failed")
                sections = {match[0]: int(match[1]) for match in re.findall(
                    r"^(\S+)\s+(\d+)\s+\d+\s*$", sizes["stdout"], re.M)}
                result["sections"] = sections
                result["footprint"] = {
                    "object_bytes": obj.stat().st_size,
                    "text_bytes": sum(value for key, value in sections.items() if key.startswith(".text")),
                    "rodata_bytes": sum(value for key, value in sections.items() if key.startswith(".rodata")),
                    "relocated_const_bytes": sum(value for key, value in sections.items() if key.startswith(".data.rel.ro")),
                    "bss_bytes": sum(value for key, value in sections.items() if key.startswith(".bss"))}
                run(["nm", "-S", "--size-sort", str(obj)], name + "-symbols")
                if job["layer"] != "boundary":
                    image = out / name
                    link = run([str(compiler), str(obj), str(abi), "-o", str(image)], name + "-link")
                    if link["returncode"]:
                        result["outcome"] = "link-failure"
                    else:
                        execution = run([str(image)], name + "-execute")
                        if execution["returncode"]:
                            result["outcome"] = "execution-failure"
                        else:
                            result["runtime"] = json.loads(execution["stdout"])
                            expected_count = job["count"] + 15 if job["variant"] == "distinct" else 16
                            if result["runtime"]["type_count"] != expected_count:
                                raise RuntimeError("Actual registry count differs from the generated graph")
                            result["image_sha256"] = digest(image)
                            result["outcome"] = "pass"
                else:
                    result["outcome"] = "pass"
            report["jobs"].append(result)
            save()
            print(f"{name}: {result['outcome']}; compile {compile_result['wall_seconds']:.3f}s; "
                  f"peak {compile_result.get('peak_rss_kib', 'unknown')}KiB", flush=True)
    after = {str(path.relative_to(ROOT)): digest(path) for path in sorted(headers)}
    if after != before:
        raise RuntimeError("Production build inputs changed during the measurement")
    report["completed"] = True
    report["all_expected"] = all(job["outcome"] in {"pass", "expected-rejection"} for job in report["jobs"])
    save()
    print("Scalability measurements completed; unexpected outcomes are retained in summary.json", flush=True)
    if args.require_pass and not report["all_expected"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
