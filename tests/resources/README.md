# Generic resource validation

Authors: Ruslan Kovtun (shpegun60), codexAi. MIT.

This C++20 suite exercises the flat filesystem and LIST/STAT/READ/WRITE
protocol independently of telemetry, PFR and any optional provider.

```sh
python tests/resources/run.py --build-dir build/resources
python tests/resources/run.py --cxx clang++-18 --sanitize --build-dir build/resources-san
python tests/resources/run.py --null-checks --build-dir build/resources-null
python tests/resources/run.py --cxx arm-none-eabi-g++ --arm --build-dir build/resources-arm
```

- `CoreCheck.cpp` retains provider, cursor, path, partial-buffer, LIST progress,
  packet-boundary and output-canary checks. LIST never calls `size()`.
- `BoundaryCheck.cpp` adds an independent reply byte grammar. A fixed-seed
  corpus tests 34,688 complete, truncated, extended, single-byte-mutated and
  biased random packets against `parseRead`, `parseList`, `parseStat` and
  `parseWrite`. Accepted data/path ranges must remain inside the original
  packet; every validated LIST iterator is exhausted and counted. A further
  5,000 generated requests check server output canaries, parseable replies,
  unchanged request bytes and absence of provider callbacks for malformed
  requests. Positive controls require all provider operations and all four
  reply parsers to be exercised. Its 306,653 conditions are counted by the
  host runner; ARM compiles this same body without executing it. This finite
  deterministic corpus is additional boundary coverage, not an exhaustive
  proof or a measurement of transport concurrency.
- `Negative.cpp` has 38 intended compile-time refusals and an executed
  positive binding control. The runner verifies the particular diagnostic;
  a missing unrelated header cannot satisfy a lifetime refusal.
  The 34 positive controls use public `FileSystem`/`FileView` metadata and
  operations to verify exact borrowed owner and path identity. Raw
  `FileEntry` path/context/operation access and raw construction are refused.
- `BytesFileCheck.cpp` checks the ready-made borrowed byte provider: every
  small offset/capacity combination, output canaries, EOF, u64 cursor edges,
  shared backing storage and overlapping reads in both directions. Its 122
  executed conditions are counted separately. `BytesFileNegative.cpp` rejects
  temporary arrays, conversion proxies and braced buffers; the host also
  checks the u32 size ceiling during constant evaluation.
- `StagedFileCheck.cpp` checks the staged writable provider: a write changes the
  file only when its final piece completes the whole file; reads return the old
  bytes while a write is staged; a cursor out of place, a stream too long or too
  short, an empty piece that does not end it, and a stage smaller than the file
  are refused and keep the old bytes; a new transfer drops an unfinished one; the
  commit hook sees the new bytes first and its refusal keeps the old ones; an
  empty file completes with one empty final piece. Its 36 executed conditions
  are counted separately, and it is compiled for Cortex-M7 at -O2 and -Os.
- `FileViewCheck.cpp` executes 88 conditions for lazy enumeration, checked
  indexing, current size/capabilities, explicit read/write, borrowed lifetime
  through temporary views, independent forward iterators, empty ranges and
  invalid-file behavior. Capability checks cover read-only, write-only,
  read/write and invalid files without invoking size/read/write callbacks.
  Compile-time controls check constexpr capabilities, standard range concepts
  and descriptor privacy. `FileViewNegative.cpp` has 13 intended refusals for
  access through temporary owning tables, unsupported iterator operations,
  implicit bool conversion and private descriptor/iterator construction.
- The complete [three-file example](../../examples/resources/README.md) is
  built and executed with 23 counted conditions on the host and compiled for
  ARM. It adds a writable custom provider to ordinary `BytesFile` entries.
- `NoHeapCheck.cpp` rejects C++ allocation and repeatedly serves fixed
  providers through both filesystem and packet APIs.
- `ArmProbe.cpp` checks actual Cortex-M7 descriptor sizes/offsets, constant
  storage, direct known-provider dispatch and a linked protocol image.
  Private `FileEntry` offsets remain checked by production static assertions
  inside its friend `FileSystem<N>`, instantiated by the probe.
- `stack_check.py` bounds every individual protocol frame at 192 bytes and
  rejects malformed, missing or excessive reports. This is an individual
  frame gate; whole-chain peaks are measured separately on H7S.

`summary.json` records counted commands, intended refusals, executed
conditions and execution scope. ARM compile/link results are not MCU runs.
Compiler, diagnostics, disassembly, sections and symbol logs are retained
beside the summary. Linked ARM allocation/formatting checks use the same
newlib-aware controls as the final telemetry qualification suite.

The generic host suite currently executes 316,118 conditions, with 68 intended
compile-time refusals. ARM has 67 refusals: the external array larger than
u32 used by the last host control cannot be declared on its 32-bit target.

On 2026-10-04 the complete host suite passed under WSL Ubuntu Clang 18.1.3
with AddressSanitizer, UndefinedBehaviorSanitizer and float-cast-overflow
checks: 92 commands, 68 intended refusals and all 9,465 executed conditions.
That run includes the current `FileView`, `BytesFile` and
`resource::protocol` interfaces. Its logs and summary were written outside
the checkout under
`C:/Users/admin/Documents/telemetry-validation/20261004-final/resources-private-clang-san`.
It is host evidence; no device was connected or image executed on an MCU.

The optional v3 provider suite is
[`tests/structured/resources`](../structured/resources/README.md). It owns
PFR-dependent header checks, independent descriptor/value oracles, bounded
reader frames, short buffers, lifetime refusals and compiled reader ABI
checks under GC/LTO. It also builds `DeviceCheck.cpp` with the migrated
application's two-file resource facade. Generic resource users do not need
those dependencies.

The encoded endpoint counterpart is
[`PayloadBoundary.cpp`](../structured/endpoints/PayloadBoundary.cpp), run by
[`endpoints/run.py`](../structured/endpoints/run.py). Its separate fixed-seed
10,742-packet corpus checks compiled Model Field/Command/Service dispatch:
exact request lengths, three canonical bool members, all underlying enum codes,
no callbacks/state changes for rejected input, untouched output/error bytes,
missing IDs and preservation of an outer Workspace lease. Each family's
native state and callback counters are checked immediately, before another
family can overwrite them; successful Service dispatch also requires its
application status to be `Ok`. It executes 427,986
counted conditions; 4,479 inputs are canonical and 2,903 contain an accepted
unknown enum code. GCC, null-check and Clang sanitizer jobs execute it; the
ARM runner compiles the body at O2/Os/Og. It adds no production contract or
application synchronization machinery.

Scalar/v2.1 fixtures and their measurements are historical. Their source is
available in the pre-migration Git tree `389c995`; they are not part of the
final v3 acceptance matrix. The final hardware evidence belongs to
[`tests/structured/mcu/h7s`](../structured/mcu/h7s/README.md).
