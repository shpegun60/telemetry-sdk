# User-guide example checks

`run.py` compiles the nine complete examples under
[`examples/user_guide`](../../examples/user_guide) with C++20 and warnings as
errors, plus the complete multi-file
[`device_integration`](../../examples/device_integration/README.md) application
and the [`group_tree`](../../examples/group_tree/README.md) branch-file layout.
Host mode executes all eleven programs. `Native.cpp` keeps its runtime
assertions enabled through `-UNDEBUG`, as does `QuickStart.cpp`; a preprocessor
control verifies that `NDEBUG` is absent and `assert` remains defined. The Native example reports
completion, while NativeOnly, Resources, Encoded, Ergonomics, FlatNative, Codec, DeviceIntegration and GroupTree
report their executed check counts.
The runner records those counts without claiming per-assert counts for Native
or QuickStart. ResourceClient executes complete client/server operations and
reports completion after checking outcomes. It also requires exact text agreement between `QuickStart.cpp`
and the root README's marked `quickstart:begin` / `quickstart:end` C++ block,
normalizing only platform line endings.

```sh
python tests/docs/run.py --cxx g++ --build-dir build/docs
python tests/docs/run.py --cxx clang++-18 --sanitize --build-dir build/docs-san
python tests/docs/run.py --cxx g++ --null-checks --build-dir build/docs-null
python tests/docs/run.py --cxx arm-none-eabi-g++ --arm --build-dir build/docs-arm
```

Select the actual compiler on the machine. `--build-dir` is required and may
point outside the checkout. Sanitizers are a host execution mode. ARM mode
compiles and links all eleven programs at `-O2` and `-Os` with newlib-nano and
nosys; it executes no image and connects to no board. `--null-checks` retains
the existing null-check compiler mode in either host or ARM builds.

Each command writes its arguments, exit status, elapsed time and output to a
log. `summary.json` records compiler and input hashes, image hashes, enabled
assertions, build/execution counts, reported conditions and completion state.
All generated files stay under the supplied output directory. QuickStart,
Native and Encoded link the compiled telemetry ABI and Model adapter.
Resources links only the generic resource packet implementation.
Codec is a standalone header-only example: it requires neither an endpoint
declaration nor Model/ABI compiled sources. Its counted checks cover nested
DTO golden bytes, all three decode forms, validation before existing-object
mutation, scope-bound reservation release and nested LIFO results.
NativeOnly is also header-only: it creates one local Field, Command and Service
without Model, ModelView, Workspace or files. Its 33 counted checks cover native
calls, checked Field conversions, exact runtime signatures, application refusals
and owning result lifetime. Transitive headers still provide type/reflection
contracts; no automatic Flash saving is claimed.
DeviceIntegration links both compiled telemetry sources and the generic
resource processor. Its counted checks exercise native operations, canonical
payloads, resource requests and bounded split/coalesced frame reception.
The project's header and CMake input hashes are recorded alongside its C++
source hashes. The same executable can also be built with the example's
CMakeLists.txt and run through CTest; the docs runner covers it directly in
all existing CI modes.
GroupTree is header-only on the SDK side: branch files over classes that know
nothing of telemetry, a Model of two branches, named handles, path access, a
`TypeReflection` override, and the descriptor decoded by `RuntimeSchema` with
every endpoint's path and type name. Its 20 counted checks run in every host
mode; the runner fails if a source or header is added to the example without
being listed.

## Documentation UX validation, 2026-10-11

The four commands above passed on the documentation working tree based on
`111d10c`. Build outputs and `summary.json` stay in the supplied external
directories. These are local results, not CI for the later documentation commit.

| Mode | Actual toolchain | Result |
| --- | --- | --- |
| Host | Qt MinGW GCC 13.1, C++20 `-O2` | 10 programs executed; 396 counted conditions; assertions enabled |
| Host null checks | Same GCC, `-fno-delete-null-pointer-checks` | 10 programs executed; 396 counted conditions |
| Sanitized host | WSL Ubuntu 24.04, Clang 18, ASan/UBSan `-O1` | 10 programs executed; 396 counted conditions |
| Cortex-M7 | CubeIDE GCC 14.3.1, `-O2` / `-Os` | 20 images compiled and linked; no board execution |

The counted total excludes the unreported individual assertions in QuickStart,
Native and ResourceClient. The runner also checks README/QuickStart parity and
assertion-enabling controls. See the [API Map validation matrix](../../doc/user/APIOverview.md#api--довідник--приклад--перевірка)
for the canonical guide and executable for each route.

Documentation review also checked 866 local links (including inbound links
to revised guides) and retained every previous heading anchor. Raw SHA-256
comparison of all 138 files under `lib/` found no byte changes. The only new
C++ source is NativeOnly; the runner change registers that program without
removing or weakening existing checks. Source style/format checks passed.
