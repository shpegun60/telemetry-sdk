# Stage 09 descriptor evidence

Authors: Ruslan Kovtun (shpegun60), codexAi. MIT license.

Implementation: [integration/telemetry/v3](../../../lib/integration/telemetry/v3/README.md),
using the native API in `<telemetry/Telemetry.hpp>` and namespace `telemetry`.
This slice implements v3.0 descriptor bytes and identity, without ValuesFile,
Bind/Exchange, transport integration or a client UI.

## Reproduce

```text
python tests/structured/descriptor/run.py --cxx g++ --build-dir build/descriptor
python tests/structured/descriptor/run.py --cxx clang++-18 --sanitize --build-dir build/descriptor-sanitized
python tests/structured/descriptor/run.py --cxx g++ --null-checks --build-dir build/descriptor-nullchecks
python tests/structured/descriptor/run.py --arm --cxx arm-none-eabi-g++ --build-dir build/descriptor-arm
```

`golden.py` constructs fixtures independently from explicit format records.
The frozen `.hex` files must agree with that specification, the C++ constexpr
packed arrays, every-offset/chunked reads and ARM object-section extraction.
The frozen models are built through `withoutTypeNames`, so `mixed`, `edge`
and `empty` stay v3.0 whatever `TELEMETRY_TYPE_NAMES` says. The same script
writes the descriptor v3.1 fixtures: `named.hex` (1027 B, the mixed model
with the scoped names `fixture::Mode`, `fixture::State` and
`fixture::MotorConfig`, which the default producer emits byte for byte),
`attrs.hex` (1106 B, named plus advisory and
application attributes on every record kind, 5-byte attribute headers) and
`v31_cases.json`, the
decoder table the [type-names suite](../type_names/README.md) runs in C++,
JS and Python.
Running `golden.py` explicitly rewrites fixtures; the test runner never does.
`parser.py` is an independent bounded reader, not a call into the producer.

| Fixture | Bytes | Types | Section offsets | FNV-1a 64 |
|---|---:|---:|---|---|
| mixed | 968 | 16 | 64 / 545 / 644 | `0c8d769b46fae6b6` |
| edge | 1486 | 22 | 64 / 797 / 992 | `c380b060ffc7ce47` |
| empty | 348 | 12 | 64 / 348 / 348 | `685aa33933892327` |

The edge model covers a type shared by Field/Command/Service, all builtin
records, enum names and signed/sparse/u64 codes, UTF-8 labels, nested structs,
repeated types, fixed arrays, empty structs/arrays/catalogs, zero-argument
endpoints and an empty getter/setter slot with declared writable capability.

Checks include every byte offset with eleven buffer lengths, complete-file
reconstruction at chunk lengths 1–129, EOF/invalid cursors, untouched output
on failure, a 4096-byte label, no getter calls, changed owners/slot targets,
changed capability/name, duplicate names and another translation unit.
Runtime construction/reading also runs with C++ allocation operators replaced
by aborting controls. Twenty-two compile-fail cases require specific diagnostics.

The parser's 1729 checks include every-byte corruption, reserved bits/version,
length/count/section inconsistencies, references to future/Void/missing types,
incorrect wire sizes, dictionaries, invalid UTF-8/NUL, metadata tails and all
resource ceilings. Structural mutations recalculate the hash before parsing,
so a fingerprint mismatch cannot conceal a missing structural validation.
Zero-byte nested arrays separately exercise depth and expanded-node budgets.

## Toolchains and retained native paths

Locally passed MinGW GCC 13.1, WSL GCC 13.3 with null-check mode, Clang 18
ASan/UBSan, CubeIDE ARM GCC 14.3.1 and ARM GCC 13.2.1 at O2/Os/Og.
The ARM null-check matrix also passes. Each host run records 5 successful
commands + 22 intended rejections; ARM records 34 + 22, plus parser checks.
CI runs these alongside the other active v3 and generic resource suites and retains
logs, stack reports and extracted golden bytes.

Stage 08 CI at `45652e7` finished 8/9: GCC 13 `-Os` outlined a Service wrapper
shared by native and encoded dispatch. The stricter existing Stage 07 byte
comparison caught it. This slice separates those two instantiations of the
same wrapper body; it does not force-inline the boundary or callback.
The unchanged Stage 07 codegen gate now passes on ARM GCC 13 (24/20 equal
bytes for direct/local/global at O2/Os). CubeIDE's equivalent object retained
identical normalized disassembly. All eleven Stage 08 direct/local/global
comparisons pass again on both ARM toolchains. These are codegen checks;
the historical Stage 08 cycle receipts remain measurements of their recorded
source snapshots, not a new measurement of every endpoint in this slice.

## Historical Stage 09 ARM footprint and stack

The figures below are the original Stage 09 measurements, not new MCU
measurements of the final namespace migration. Current runners keep the
native/codegen and stack gates enabled.

Separate linked images retain just descriptor access and its storage root,
without LTO. The streaming image also retains metadata and the fixture callbacks
reachable from ModelView; the packed image can discard them. These are complete
probe images, **not incremental costs in a firmware that already uses Model**.

CubeIDE GCC 14.3.1:

| Optimization | Streaming `.text` / `.rodata` | Packed `.text` / `.rodata` |
|---|---:|---:|
| O2 | 6908 / 3184 B | 508 / 1486 B |
| Os | 2592 / 3146 B | 492 / 1486 B |
| Og | 6160 / 3190 B | 204 / 1486 B |

The edge segment index occupies 744 B, including the sentinel. No heap symbols
or startup constructors survive the linked descriptor probe. The streaming
image's 28 B `.data` + 12 B `.bss` belong to fixture owners/slots/counters;
descriptor storage itself is read-only. Packed-only images have zero data/bss.

Passing ModelView by reference removed an early 312 B reader frame. Final
CubeIDE reader/emitter frames are 64/48 B at O2 and 64/80 B at Os. At Og,
the wrapper/read/emitter frames are 16/56/96 B. GCC 13 has comparable bounded
frames; the runner retains its exact values in `arm-summary.json`. These are
individual frames, not a summed call-chain/interrupt-stack proof (Stage 13).

## H7S measurement, 2026-09-27

NUCLEO-H7S3L8, Cortex-M7 600 MHz, CubeIDE GCC 14.3.1, hard float, caches on,
no LTO. Both readers share the same 1486-byte model and run in the same image.
Random offsets use a fixed LCG sequence. Five windows follow cache invalidation
and warm-up; interrupts are masked only during DWT measurement. Checksums,
complete coverage and every-offset byte equivalence are verified.

Median cycles; random rows are **per read**, full-file rows **per transfer**:

| Operation | O2 stream | O2 packed | Os stream | Os packed |
|---|---:|---:|---:|---:|
| Random offset, up to 16 B | 812.79 | 95.84 | 1049.45 | 84.96 |
| Random offset, up to 64 B | 1578.77 | 234.82 | 2158.34 | 177.59 |
| Random offset, up to 256 B | 4298.61 | 749.86 | 6263.96 | 520.94 |
| Full file, 64 B chunks | 36417.07 | 5548.67 | 49835.75 | 4136.66 |
| Full file, 256 B chunks | 26790.33 | 4738.65 | 39063.70 | 3272.67 |

Packed Flash bytes are preferred for constexpr models. Streaming remains the
runtime-metadata fallback; its direct resume is bounded, but still formats each
visited record. Both representations have exactly the same wire contract.
These measurements include call/loop/checksum work, with no baseline subtraction;
they do not measure transport throughput or claim a universal ratio for all models.

The runner builds both images before touching hardware, saves the full 64 KiB
internal Flash, restores in `finally` and verifies read-back. No option bytes
or external Flash are written. This run's original and restored SHA-256:
`a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`.
Raw windows and image/source hashes are retained in the
[historical receipt](../../../doc/evidence/pre-unification/stage09/receipt.json).
It records `source_head=45652e7db79debb54d851e6d4c5208bc16379c97` and completion
on 2026-09-27. That historical receipt does not contain a `source_dirty` field;
its captured input hashes identify the measured local build, not HEAD alone.
Its verifier detects 16 altered receipts; offline verification checks consistency,
not current-board execution or availability of every original artifact.

```powershell
$arm = (Get-ChildItem 'C:/ST/*/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32*/tools/bin/arm-none-eabi-g++.exe' | Sort-Object FullName -Descending | Select-Object -First 1).FullName
$programmer = (Get-ChildItem 'C:/ST/*/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer*/tools/bin/STM32_Programmer_CLI.exe' | Sort-Object FullName -Descending | Select-Object -First 1).FullName
python tests/structured/descriptor/h7s/run.py --cube path/to/h7s_cobs_test `
  --arm-cxx $arm --programmer $programmer --serial SELECTED_SERIAL `
  --port SELECTED_COM --output build/descriptor-h7s-new --run
python tests/structured/descriptor/h7s/verify.py --self-test
```

The historical run used serial `002A001F3033510135393935`, UART COM6. Current
execution has no serial/port defaults: `--run` requires explicit `--programmer`,
`--serial` and `--port`. Without `--run`, only both images are built. Existing output directories
are refused. A power or host interruption still requires manual restoration
from the retained `before.bin`.

## Final-source H7S verification, 2026-10-03

The [current hardware evidence](h7s/README.md) and
[receipt](h7s/receipt.json) record both O2/Os images after source unification,
with `source_head=01fd180ff3678d7c47066f8a728dc1e02f0f3973` and
`source_dirty=true`. Both complete correctness/timing plans passed, and all
65536 internal Flash bytes were restored and verified. Captured input and
image hashes identify this build; the earlier table remains historical.
The [mixed MCU qualification](../mcu/h7s/RESULTS.md) separately records
endpoint cycles and observed full call-chain stack. Offline receipt
verification does not constitute another device execution.
