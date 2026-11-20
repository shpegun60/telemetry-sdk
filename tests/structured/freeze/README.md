# Stage 15 software contract qualification

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

This suite fixes the current C++20 public contract and frozen wire v3.0.
Descriptor v3.1 is an additive contract: `wire_v3_1` in the manifest pins its
minor, type and record flags and attribute tag ranges, and `named.hex` and
`attrs.hex` join the goldens with their own minor 1. v3.0 stays frozen as the
stripped form: the four v3.0 goldens are unchanged, `binaryMinor` stays 0 and
Values stays 3.0. `default_type_names` pins the `TELEMETRY_TYPE_NAMES`
default of [Config.hpp](../../../lib/telemetry/Config.hpp).
This suite does not execute hardware. The separate
[Stage 14 H7S results](../mcu/h7s/RESULTS.md) record that execution.
The [qualification decision](../../../doc/StructuredTelemetryV3FreezeQualification.md)
keeps software contract and hardware qualification as separate statuses.

`Contract.cpp` checks public return types, mixed exact native values, named
local positions, global u32 IDs, shared type identity, nonmoving tables,
empty iteration, static/runtime As access and absence of semantic metadata.
Its **29 counted conditions per optimization** cover local ordered
forEach/visit/container traversal for all three families without reading
values, literal canonical LE bytes, native and encoded operations, checked
conversion, structural mismatch, ID bounds and invalid bool rejection before
the setter. O2/Os/Og therefore execute **87 conditions per host invocation**.
Static assertions and compile rejections are separate from that count.

Eleven compile-fail cases require the intended factory/type diagnostic: unit
metadata, wrong Command status, scalar Command request, scalar Service
response, throwing target, pointer request, Command metadata, a different
struct with identical members, structured Scalar fallback, Service metadata
and limits/default-like Field metadata. They preserve rejection contracts;
they are not eleven new defects. Positive compile-time controls also verify
the step/constraints/limits/defaults clauses in the metadata detector.

The readable [contract.json](contract.json) records header widths, LE,
record/type/scalar/status/capability codes, FNV-1a-64 constants, u32 packed
identity, ABI revision, the default 32-byte storage budget, all eleven default
`ts::Limits` resource ceilings, exact dependency
pins and four independent descriptor/values goldens. Generated compile-time
assertions tie its numeric constants to the implementation. Golden hashes
are over decoded bytes, independent of `.hex` whitespace. Existing
descriptor/resource/client suites still compare emitted bytes with their
independent oracles; this suite adds a lock on those expected bytes.

The default v1 ceilings are part of the acceptance contract, not an unpinned
implementation setting or application value limits. Changing one requires an
explicit compatibility review and contract update even if wire bytes stay
unchanged. Removing a ceiling from the manifest is refused. Eleven separate
compile controls change one expected value each and require a static-assert
failure naming that exact ceiling; these have their own rejection counter.
Explicit application profiles supported by existing interfaces are separate
from this frozen default profile and do not change the wire fingerprint.

Boost.PFR is checked against canonical tracked Git blob bytes, as described
in [VERSION.md](../../../lib/boost_pfr/VERSION.md), with all 44 headers and
the license. The working tree must match those blobs after LF normalization;
unstaged vendor changes cannot pass by reading an older HEAD. magic_enum's
original tracked header and license bytes and provenance pin are checked as well.
These checks need a Git checkout; they do not download dependencies.

```sh
python3 tests/structured/freeze/run.py --cxx g++ --build-dir build/freeze-gcc
python3 tests/structured/freeze/run.py --cxx g++ --null-checks --build-dir build/freeze-null
python3 tests/structured/freeze/run.py --cxx clang++-18 --sanitize --build-dir build/freeze-clang
python3 tests/structured/freeze/run.py --arm --cxx arm-none-eabi-g++ --build-dir build/freeze-arm
```

ARM runs compile/link O2/Os/Og, inspect real nm/sections/disassembly, reject
allocation/formatting/Scalar symbols and metadata startup constructors, and
record static individual `.su` frames. They execute **zero ARM conditions**.
The 512-byte frame ceiling is only a bound for this small contract fixture;
the stricter large-object and direct-call gates remain in the qualification
and MCU suites. No new cycle or whole-call-chain stack result is inferred.

`summary.json` records actual compiler/driver flags, executable SHA-256,
input hashes, execution role, counters, sections and individual frames.
Logs and disassembly stay in the selected build directory and CI artifacts.
The full seven-role offline receipt remains in the separate MCU suite.

Locally the contract passed with MinGW 13.1, GCC 13.3 null checks, Clang 18.1
ASan/UBSan, CubeIDE ARM 14.3.1 normal/null and ARM 13.2.1 normal/null. CI runs
the new suite in selected C++20 host modes, Clang sanitizers and both ARM
null-check modes. Publication is qualified by CI for the resulting SHA;
prior green runs do not qualify newly added tests.

The ceiling/license follow-up passed the same seven local compiler roles.
Host condition counts remain 87; declaration and ceiling rejections are 11
each. The retained [ELF comparison](elf-comparison.json) measures twelve
ARM O2/Os/Og pairs across ARM 13.2.1 and CubeIDE 14.3.1, normal/null modes:
**12/12 byte-identical, zero differing pairs**. It records each file's actual
size and SHA-256, raw summary/link-log digests, compiler/flags and a canonical
input-manifest digest. The before summaries record source HEAD
`f77ea0fbba28971c2192a11c70f547fb54fec18d`; the ceiling-follow-up summaries
record `ce630a7c6d255d537af5350937e6b315cd627805`. They have no source-dirty
field or linked-image digests, so this later file comparison does not establish
clean-source builds or cryptographically bind those old summaries to ELF
bytes. Local ignored build paths do not establish archived artifact
retrievability; no stable archive locator is established in this record.
This is offline binary evidence, not current-HEAD images, new MCU execution
or cycle timing. New runner summaries record linked-image digests directly.
