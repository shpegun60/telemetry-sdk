# Descriptor v3.1 type names and attribute areas

Authors: Ruslan Kovtun (shpegun60), codexAi. MIT license.

This suite qualifies [descriptor v3.1](../../../doc/WireV3.md#descriptor-v31-type-names-and-attributes):
an optional, unique type name on enums and structs, and a skippable attribute
area after any record. It also qualifies the project configuration file
[Config.hpp](../../../lib/telemetry/Config.hpp) and the `typeName<T>()` seam
in [TypeName.hpp](../../../lib/telemetry/reflection/TypeName.hpp).

## Reproduce

```sh
python3 tests/structured/type_names/run.py --cxx g++ --node node --build-dir /external/type-names
python3 tests/structured/type_names/run.py --cxx clang++-18 --sanitize --build-dir /external/type-names-sanitized
python3 tests/structured/type_names/run.py --cxx g++ --cross clang=clang++-18 \
  --arm-cxx arm-none-eabi-g++ --base-ref origin/main --build-dir /external/type-names-matrix
```

On Windows a `--cross` compiler may be reached through WSL with a `wsl:`
prefix, for example `--cross wsl-clang++-18=wsl:clang++-18`. The build
directory must be outside the checkout. Nothing touches a device.

## What it checks

1. **Goldens.** [golden.py](../descriptor/golden.py) builds `named.hex` (the
   mixed model with its three C++ type names) and `attrs.hex` (the same plus
   advisory attributes on Struct records, one of them in the member shape of
   a presence bitmap and items, an Enum, an Array, a Catalog, a Field, a
   Command and a Service record, and one application tag) from explicit
   records, without the producer. It also writes
   [v31_cases.json](../descriptor/v31_cases.json): accepted v3.1 inputs and
   one malformed descriptor per rule with the verdict every decoder must give.
2. **Producer.** `Producer.cpp` is built with the default configuration and
   with `TELEMETRY_TYPE_NAMES=0`. The default build emits `named.hex` byte for
   byte; names off reproduces `mixed.hex`, `edge.hex` and `empty.hex`; the
   `withoutTypeNames` profile reproduces them on both builds. It also checks
   the scoped automatic names (`fixture::MotorConfig`, a nested class
   `Outer::Inner`, `left::Status` beside `right::Status`), explicit
   (`typeName("...")`), removed (`noTypeName()`) and absent names (template
   specializations, arrays, scalars), the registry's names array, streamed
   reads of a named descriptor and the runtime `DuplicateName` of one
   innermost scope used twice (`left::io::Status`, `right::io::Status`).
3. **Three decoders, one table.** `Decode.cpp` (RuntimeSchema), `decode.mjs`
   (web/telemetry.js) and the Python parser decode every accepted case to one
   canonical model dump and refuse every negative case with its verdict,
   including every type name that is not `[scope "::"] name` (two scopes, an
   empty part, one or three colons, `/ . [ ] \` in a part: `TypeShape`).
   `attrs.hex` decodes to the model of `named.hex`, and each fingerprint is
   FNV-1a over every byte, attribute bytes included. `findType`, `typeName`
   and the minor are checked against the same model.
4. **Compile-time refusals.** Duplicate automatic struct names, duplicate
   automatic enum names and an explicit name colliding with an automatic one
   (`duplicateTypeName`), invalid UTF-8, NUL and empty explicit names, an
   explicit name with two scopes or a path character, a class declared in a
   function (a local class, which has no stable automatic name; GCC, MinGW
   and ARM GCC refuse it, while Clang spells it without its function, so
   under Clang the case is a control that builds), a
   `TypeReflection` on an array, a wrong `TypeReflection::name` type and a
   name longer than a tightened profile. Seven controls must build: names
   off, the stripping profile, an explicit name and `noTypeName()` settle
   the same clash, and an explicit name, names off or the stripping profile
   accept the local class.
5. **Link.** `LinkApp.cpp`, `StructuredAbi.cpp` and `Adapter.cpp` link and run
   when built with one setting, and fail to link when the application and
   the SDK objects disagree on `TELEMETRY_TYPE_NAMES`: the switch is part of
   the structured ABI tag, like the local-bytes budget.
6. **Configuration.** `ConfigProbe.cpp` sees the defaults, a project file
   found through `__has_include` ([config/telemetry_config.hpp](config/telemetry_config.hpp)),
   and `-D` definitions; the same value set both ways builds, a different
   value set both ways is the reported redefinition, and a value other than
   0 or 1 is refused.
7. **Cross-compiler names.** `CrossCompiler.cpp` packs one named model whose
   automatic names come from each compiler's function-signature spelling:
   a namespace (`device::Mode`, `device::Level`), nested classes
   (`Board::Probe`, `Board::Channel`), the global scope (`Global`), the
   anonymous namespace (`Sealed`), a typedef-named struct
   (`device::Legacy`), an inline namespace named explicitly `v2::Config`,
   a class nested in a specialization (unnamed), an explicit UTF-8 name and a
   removed name. Every given compiler must produce [cross.hex](cross.hex):
   host compilers run it, an ARM compiler builds the object and the bytes are
   taken from its section. A disagreeing compiler gets the name parser fixed;
   there is no explicit-names fallback. `AliasProbe.cpp` records the two
   forms measured to differ and checks the second per compiler family. An
   unnamed struct named by an alias-declaration (`using X = struct {...};`):
   Clang names it `probe::X`, GCC, MinGW and ARM GCC leave it unnamed, and
   their signature carries no name to recover; it is reported, not passed. A
   struct in an inline namespace (`probe::v2::Status`): GCC, MinGW and ARM
   GCC name it `v2::Status`, Clang `probe::Status` (plan rule 4, which is why
   the cross model names its inline-namespace type explicitly). A local class
   is not in the byte fixture either: GCC spells its function and the
   producer refuses it, Clang spells it as if it were global (`Local`), so a
   Clang-only build names it `Local` without an error; every device builds
   with ARM GCC, which refuses it.
8. **Cross-backend.** `Backend.cpp` reports whether a compiler announces both
   `__cpp_impl_reflection` and `__cpp_lib_reflection`. The comparison of
   today's backend with `std::meta` is reported as **not established**: no
   compiler in the set qualifies, and no `std::meta` code is written before
   one can build it. It is a visible skip, never a pass.
9. **Decoder size.** With `--arm-cxx`, `DecoderSize.cpp` links
   `RuntimeSchema::parse` alone with `--gc-sections` at O2 and Os, against the
   current tree and, with `--base-ref`, against that revision's
   `lib/telemetry`. These are offline object sizes, not device execution.

`summary.json` in the build directory records the counts, the per-compiler
cross bytes and digests, the alias measurements, the cross-backend status and
the decoder sizes.

## Measured on 20 November 2026

| Compiler | `cross.hex` (1475 B) | `using X = struct {...}` | inline namespace |
| --- | --- | --- | --- |
| MinGW g++ 15.1.0 (Qt) | equal | unnamed | `v2::Status` |
| g++ 13.3.0 (WSL Ubuntu 24.04) | equal | unnamed | `v2::Status` |
| clang++ 18.1.3 (WSL) | equal | `probe::Alias` | `probe::Status` |
| arm-none-eabi-g++ 14.3.1 (CubeIDE 2.0.0) | equal | unnamed | `v2::Status` |

No compiler announces P2996 reflection, so the cross-backend test is not
established.

ARM decoder `.text` with CubeIDE arm-none-eabi-g++ 14.3.1, `RuntimeSchema::parse`
linked alone, against `lib/telemetry` of the base `3469202`:

| Optimization | Base | v3.1 decoder | Delta |
| --- | ---: | ---: | ---: |
| O2 | 10548 B | 11744 B | +1196 B |
| Os | 5880 B | 6964 B | +1084 B |

The delta is reading and checking type names (`typePrefix` with the
`[scope "::"] name` shape check, about 105 B of it; one heapsort
instantiation for uniqueness) and validating compact attribute areas (`done`:
5-byte headers, ascending tags). These are offline object sizes; no device
was run.
