#!/usr/bin/env python3
"""Service tables, encoded Model dispatch, ABI and offline ARM code-generation checks.

Authors: Ruslan Kovtun (shpegun60), codexAi. MIT license.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


ROOT = Path(__file__).resolve().parents[3]
SOURCES = Path(__file__).resolve().parent
ADAPTER = ROOT / "lib/telemetry/model/Adapter.cpp"
ABI = ROOT / "lib/telemetry/abi/StructuredAbi.cpp"

DIAGNOSTICS = {
    1: r"Service position is outside this table",
    2: r"Service position is outside this table",
    3: r"Service group is outside this catalog",
    4: r"Service position is outside this table",
    5: r"Packed ID must be an integer",
    6: r"deleted (?:constructor|function)",
    7: r"cannot bind.*lvalue reference|no matching function",
    8: r"no matching (?:member )?function",
    9: r"no matching (?:member )?function",
    10: r"deleted (?:member )?function",
    11: r"deleted (?:member )?function",
    12: r"deleted (?:constructor|function)",
    13: r"deleted (?:constructor|function)",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    parser.add_argument("--arm", action="store_true")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--null-checks", action="store_true")
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.arm and args.sanitize:
        parser.error("--arm and --sanitize cannot be combined")

    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)

    def run(command: list[str], label: str, expected_failure: str = "") -> str:
        start = time.perf_counter()
        result = subprocess.run(
            command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace", timeout=180,
        )
        duration = time.perf_counter() - start
        log = f"COMMAND {command!r}\nEXIT {result.returncode}\nSECONDS {duration:.3f}\n{result.stdout}"
        (output / f"{label}.log").write_text(log, encoding="utf-8")
        if expected_failure:
            if result.returncode == 0 or re.search(expected_failure, result.stdout, re.I | re.S) is None:
                raise RuntimeError(f"{label} missed its intended failure\n{log}")
        elif result.returncode:
            raise RuntimeError(f"{label} failed\n{log}")
        print(f"{label}: {'expected rejection' if expected_failure else 'pass'} "
              f"({duration:.2f}s)", flush=True)
        return result.stdout

    run([args.cxx, "--version"], "compiler-version")
    flags = [
        args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pedantic-errors",
        "-fdiagnostics-color=never", "-Ilib", "-Ilib/boost_pfr/include",
        "-Ilib/magic_enum",
    ]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if args.null_checks:
        flags += ["-fno-delete-null-pointer-checks"]
    if args.arm:
        flags += [
            "-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard",
            "-fno-exceptions", "-fno-rtti", "-ffunction-sections", "-fdata-sections",
            "-fstack-usage",
        ]

    optimizations = ("O2", "Os", "Og") if args.arm else ("O2",)
    for optimization in optimizations:
        optimized = flags + [f"-{optimization}"]
        for stem, sources in (
            ("ModelProbe", [SOURCES / "ModelProbe.cpp", ADAPTER, ABI]),
            ("ModelEdges", [SOURCES / "ModelEdges.cpp"]),
            ("ModelSlots", [SOURCES / "ModelSlots.cpp"]),
            ("LargeResponse", [SOURCES / "LargeResponse.cpp"]),
        ):
            if args.arm:
                for source in sources:
                    obj = output / f"{optimization}-{stem}-{source.stem}.o"
                    run(optimized + ["-c", str(source), "-o", str(obj)],
                        f"compile-{optimization}-{stem}-{source.stem}")
            else:
                program = output / (f"{optimization}-{stem}" +
                                    (".exe" if os.name == "nt" else ""))
                run(optimized + [*[str(source) for source in sources],
                                 "-o", str(program)], f"link-{optimization}-{stem}")
                run([str(program)], f"execute-{optimization}-{stem}")

        negative_flags = [flag for flag in optimized if flag != "-fstack-usage"]
        for case, diagnostic in DIAGNOSTICS.items():
            run(negative_flags + [f"-DCASE={case}", "-fsyntax-only",
                             str(SOURCES / "ModelNegative.cpp")],
                f"negative-{optimization}-{case}", diagnostic)

    if args.arm:
        tool_dir = Path(shutil.which(args.cxx) or args.cxx).resolve().parent
        objcopy = tool_dir / ("arm-none-eabi-objcopy.exe" if os.name == "nt"
                              else "arm-none-eabi-objcopy")
        objdump = tool_dir / ("arm-none-eabi-objdump.exe" if os.name == "nt"
                               else "arm-none-eabi-objdump")
        table_sections = run(
            [str(objdump), "-h", str(output / "O2-ModelProbe-ModelProbe.o")],
            "sections-O2-ModelProbe")
        for symbol in ("local", "services"):
            if re.search(rf"\.rodata\._ZN5probe\d+{symbol}E\b", table_sections) is None:
                raise RuntimeError(f"{symbol} table did not remain in ARM read-only storage")
        if re.search(r"\.(?:preinit_array|init_array|ctors)\b", table_sections):
            raise RuntimeError("Service tables unexpectedly require startup constructors")

        for optimization in optimizations:
            large_usage = (output / f"{optimization}-LargeResponse-LargeResponse.su")\
                .read_text(encoding="utf-8")
            large_thunks = re.findall(r"ServiceTable[^\n]*invokeOne[^\n]*\t(\d+)\tstatic",
                                      large_usage)
            if len(large_thunks) != 2 or any(int(frame) > 256 for frame in large_thunks):
                raise RuntimeError(f"{optimization}: a 4 KiB encoded Service hides a stack copy")
            print(f"{optimization}: 4 KiB encoded thunks use {large_thunks} B frames",
                  flush=True)

            obj = output / f"{optimization}-ArmModelCodegen.o"
            run(flags + [f"-{optimization}", "-c",
                         str(SOURCES / "ArmModelCodegen.cpp"), "-o", str(obj)],
                f"compile-{optimization}-ArmModelCodegen")
            usage = obj.with_suffix(".su").read_text(encoding="utf-8")
            for symbol in ("model_direct", "model_local", "model_handle", "model_path",
                           "model_encoded"):
                match = re.search(rf"\b{symbol}\([^\n]*\)\s*\t(\d+)\tstatic", usage)
                if match is None or int(match.group(1)) > 192:
                    raise RuntimeError(f"{optimization} {symbol} exceeds 192 B stack")
                print(f"{optimization} {symbol}: {match.group(1)} B frame", flush=True)

            if optimization in ("O2", "Os") and not args.null_checks:
                images = []
                for symbol in ("model_direct", "model_local", "model_handle", "model_path"):
                    image = output / f"{optimization}-{symbol}.bin"
                    run([str(objcopy), "--dump-section",
                         f".text.{symbol}={image}", str(obj)],
                        f"codegen-{optimization}-{symbol}")
                    images.append(image.read_bytes())
                if not images[0] or any(images[0] != image for image in images[1:]):
                    raise RuntimeError(f"{optimization}: local/handle/path native call differs from direct")
                print(f"{optimization}: direct/local/handle/path each {len(images[0])} identical bytes",
                      flush=True)

        arm_link = ["-nostdlib", "-Wl,-e,main", "-Wl,--gc-sections", "-lc", "-lgcc"]
        link_flags = [flag for flag in flags if flag != "-fstack-usage"]
        for optimization in ("O2", "Os"):
            program = output / f"{optimization}-adapter.elf"
            run(link_flags + [f"-{optimization}", str(SOURCES / "ArmAdapterLink.cpp"),
                         str(ADAPTER), str(ABI), *arm_link, "-o", str(program)],
                f"link-{optimization}-adapter")
        for case in (1, 2, 3):
            program = output / f"adapter-mismatch-{case}.elf"
            run(link_flags + ["-O2", f"-DCASE={case}",
                         str(SOURCES / "AdapterAbiMismatch.cpp"), str(ADAPTER),
                         str(ABI), *arm_link, "-o", str(program)],
                f"arm-abi-mismatch-{case}",
                r"undefined (?:reference|symbol).*callServiceEncoded")
    else:
        # These three mismatches alter revision and new Service/Model layout
        # dimensions. The *real* compiled adapter symbol must fail to link.
        modes = [("normal", [], [])]
        if os.name != "nt":
            lto_flags = ["-flto"]
            if "clang" in Path(args.cxx).name:
                lto_flags.append("-fuse-ld=lld")
            modes += [("gc", ["-ffunction-sections", "-fdata-sections"],
                       ["-Wl,--gc-sections"]),
                      ("lto", lto_flags, lto_flags),
                      ("pie", ["-fPIE"], ["-pie"])]
        for mode, compile_flags, link_flags in modes:
            if mode != "normal":
                valid = output / f"abi-valid-{mode}"
                run(flags + ["-O2", *compile_flags,
                             str(SOURCES / "ModelProbe.cpp"), str(ADAPTER),
                             str(ABI), *link_flags, "-o", str(valid)],
                    f"abi-valid-link-{mode}")
                run([str(valid)], f"abi-valid-execute-{mode}")
            for case in (1, 2, 3):
                program = output / f"abi-mismatch-{mode}-{case}.exe"
                run(flags + ["-O2", *compile_flags, f"-DCASE={case}",
                             str(SOURCES / "AdapterAbiMismatch.cpp"), str(ADAPTER),
                             str(ABI), *link_flags, "-o", str(program)],
                    f"abi-mismatch-{mode}-{case}",
                    r"undefined (?:reference|symbol).*callServiceEncoded")

    print("Stage 07 structured Service Model checks passed", flush=True)


if __name__ == "__main__":
    main()
