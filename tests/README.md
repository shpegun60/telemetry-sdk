# Telemetry checks

The optional [connection Session suite](connection_session/README.md) checks a
packed, offline-installed JS/TS consumer, deterministic bootstrap/recovery,
actual local close fences and native Encoded parity over the existing FIFO.

The active checks use the final C++20 telemetry API. Core tests do not require
Qt. The resource, protocol and client suites have separate runners; the CI
matrix also builds Cortex-M7 objects and the optional Qt transport and client modules.

The UX-owned [Qt/QML generator suite](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/tests/codegen_qml/README.md) executes the optional Qt
module in a real QML runtime, with native Encoded operations, existing RpcClient
and strict lossless QVariant conversions. Its builds stay outside the source tree.

The optional [code generator suite](codegen/README.md) independently executes
generated C++ and JS/TS through direct Encoded and Resource/RPC paths, with
native canonical fixtures, strict TypeScript, sanitizers and offline ARM reports.
Its build directory must be outside the repository.

The [DynamicClient suite](dynamic_client/README.md) exercises runtime admission,
the existing FIFO and real Encoded/Retrying/RPC backends. Independent
[policy and lifetime probes](dynamic_client_review/README.md) cover execution
guards, passive completion and metadata deallocation after invalidation.

The [read comparison](read_comparison/README.md) checks existing individual,
aggregate and ValuesFile paths against the same values, with actual COBS byte
accounting and observation-only host timings.

The optional [Mixed Batch suite](batch/README.md) checks ordered Read/Write/Command/
Service operations, global preflight, independent item results, explicit repeated
execution, no-heap, relocated SDK consumers and offline Cortex-M7 roots. It uses
the existing DynamicClient FIFO and adds no transaction journal or deduplication.

The [typed Static ReadMany suite](read_many_static/README.md) checks generated and
handwritten Codec policies over that same core FIFO, exact scalar/item outcomes,
controlled lifecycle and malformed replies, noheap, isolated installed consumers
and offline Cortex-M7 roots without RuntimeSchema or UI dependencies.

The [typed Static Batch suite](batch_static/README.md) checks typed ordered
operations, explicit storage and callable lifetime, whole-reply publication,
noheap and installed consumers without RuntimeSchema or UI dependencies.

The [JS/TS ReadMany suite](client_read_many_web/README.md) checks inferred generated
tuples, owned results, canonical scalar validation, controlled clocks and the
actual C++ Encoded dispatcher, including a sanitized native peer.

The [JS/TS Mixed Batch suite](client_batch_web/README.md) checks all four item
operations through that FIFO, inferred DTO tuples, independent mutation outcomes,
canonical payload failures, deterministic lifecycle and the native Model trace.

The [dynamic JS/TS suite](client_dynamic_web/README.md) uses a full validated
Descriptor and compares all six operations with the generated static SDK over
the native Model. It checks opaque admission, passive invalidation, explicit
quiescence and controlled Encoded/RPC deadlines without a browser or GUI.

The [Inspector suite](inspector/README.md) checks read-only runtime browsing,
owned snapshots, exact scalar presentation, stale node/callback rejection and
real Encoded/RPC reads without Qt. Installed-module checks isolate its public
dependency closure from native Model, Resource and concrete backends.

The same maintained runner executes [FieldWrite Workbench checks](workbench/README.md)
with shared snapshot ownership, transactional scalar edits, real Encoded/RPC
Write/Refresh, deterministic mutation deadlines and independent result handling.

The [Qt/QML Inspector suite](https://github.com/shpegun60/telemetry-ux/blob/main/tests/inspector_qml/README.md) checks real Qt model
invariants, lazy snapshot browsing, exact scalar text, asynchronous read-only
controller behavior and actual QML edit/Apply handlers over the native Encoded peer.

The [native TCP suite](tcp_stream/README.md) checks bounded Resource/TEQ1 stream
framing, real dispatch, preflight, reentrancy, no-heap and Cortex-M7 compilation.
The [link transport suite](link_transport/README.md) checks the Qt-free TCP
framer, `LinkTransport` over an in-memory link, and a TCP session trace that must
equal the golden recorded before the transport was split into link and framer.
The [TCP Inspector suite](https://github.com/shpegun60/telemetry-ux/blob/main/tests/inspector_tcp/README.md) adds actual Qt sockets,
Resource Descriptor discovery, partial transmission, connection failures, stale
replies, recovery and a separate-process TCP/QML window smoke test.

The [runtime schema suite](runtime_schema/README.md) reads real Descriptor v3
files and wire payloads with caller-owned storage, compares generated codecs,
and checks malformed descriptors against the independent Python parser.

Run the shared ID, slot and numeric checks from the repository root:

```sh
python3 tests/run_checks.py --cxx g++ --build-dir build/checks-gcc
python3 tests/run_checks.py --cxx clang++-18 --build-dir build/checks-clang
python3 tests/run_checks.py --cxx g++ --null-checks --build-dir build/checks-null
python3 tests/run_checks.py --cxx clang++-18 --sanitize --build-dir build/checks-sanitized
python3 tests/run_arm_checks.py --build-dir build/arm
python3 tests/run_arm_checks.py --null-checks --build-dir build/arm-null
```

`--source-root PATH` selects a library checkout for preparation runs while
the fixtures remain beside the runner. It defaults to the repository containing
the runner. The shared tests link no compiled telemetry adapter or serializer.

Every command keeps its arguments, exit status and diagnostic in a log. Each
build directory also contains `summary.json` with actual command, intended
refusal, execution, runtime-check and intentional-abort counts. Runtime counts
come from executed fixtures. Compiler-only checks do not claim runtime coverage.

The shared coverage preserves:

- Packed ID width and signedness before narrowing, local enum positions,
  rejected implicit ID wrappers and explicit template-type narrowing,
  using/ADL name controls, and intentional failures at runtime and before main.
- All five stable slot forms, ownership and destruction, exact capacity and
  alignment, replacement of an aliased owned target, reference forwarding,
  callable overload selection, exact signatures, and no heap fallback for
  owning slots even when the companion delegate permits one.
- Hand-derived numeric boundaries and constexpr/runtime parity, unchanged
  destination on refused conversion, signed zero, subnormal and NaN/Inf cases,
  and the independent extended-precision numeric oracle. The oracle explicitly
  reports a skip when the platform has fewer than 64 long-double mantissa bits.
- Standalone `.h` and `.hpp` headers, null-check compiler modes, ELF weak targets
  and strong overrides, `-Og` compilation, and unsupported floating-point modes.
- Cortex-M7 high-word rejection before ID use, native-width extraction without
  added work, and controls that reject a changed high-word register.

The ARM shared runner compiles objects and checks diagnostics and disassembly.
Its `.su` files describe individual compiler frames. It does not execute the
firmware or establish full call-chain stack usage. The dedicated
[MCU qualification](structured/mcu/README.md) and frozen typed-core runners
carry their separate runtime, codegen, frame, ABI and evidence gates.

[Structured suites](structured/README.md) retain native mixed endpoints,
reflection facade checks, types, codec/workspace lifetime, registry, services,
model, descriptor and values goldens, resources, traversal, optional protocol,
client interoperability, multi-TU qualification, MCU qualification and the
frozen API/wire contract. [Resource checks](resources/README.md) preserve the
independent flat filesystem, lazy `FileView`, borrowed `BytesFile` and
`resource::protocol` checks without telemetry dependencies. The optional
[v3 provider checks](structured/resources/README.md) validate descriptor/value
providers and the application's resource facade separately.

[RPC mailbox checks](structured/rpc/README.md) execute fragmented requests,
duplicate/final handling, state polling and checked Model dispatch through the
existing Resource protocol. They also qualify the integration example,
allocation traps, temporary-borrow diagnostics and retained Cortex-M7 roots
with bounded individual frames under O2/Os/Og.
The separate [typed-client suite](structured/rpc_client/README.md) checks
canonical begin/decode, exact packet retry, malformed replies, local type
identity, caller-buffered large objects and uncertain outcomes. The
[session suite](structured/rpc_sessions/README.md) checks independent N=1/2/4
mailboxes, local generation/tickets, deferred release and bounded scheduling.
Both exercise actual Resource packets on host and compile retained ARM roots;
neither establishes additional device execution.
The separate [H7S UART integration](structured/rpc/h7s/README.md) checks actual
packets, ACK ordering and duplicate Service/Command requests on the selected
board. Its session controls simulate application disconnects; Ethernet and
physical cable-removal behavior require separate integration checks.

[User-guide checks](docs/README.md) compile the complete native, resource and
encoded examples linked from the [public guide](../doc/user/README.md).
Host mode executes the examples with assertions enabled; ARM mode compiles
and links them without running an image on a device.

[COBS integration checks](cobs/README.md) execute the real framing endpoint with
telemetry and resource operations, including busy ownership, split input and
gap recovery. The [cross-endian runner](structured/codec/endian.py) executes
native little/big-endian targets and compares complete canonical wire files.
It also executes resource client request/reply byte goldens, malformed-input
controls and client/server round trips on both targets.
[Source conventions](../tools/source_style/README.md) are checked in CI too.
[ReadMany mapping checks](read_many_mapping/README.md) qualify experimental compact
frames and a padded Service, allocation traps and separate installed ARM roots.
[Production ReadMany checks](read_many/README.md) exercise the optional C++ Encoded
mapping, real DynamicClient FIFO/lifecycle, relocated packages and allocation controls.

[Ergonomic API checks](ergonomics/README.md) execute native runtime selection,
borrowed reads, detailed read failures, stoppable traversal and bounded resource
client parsing. ARM profiles compare manual/helper dispatch at 128/256 targets,
large-response stack frames and unchanged generic protocol codegen.
Its separate flat-outcome runner qualifies one-level Command/Service results,
exact owning/borrowed selection, 1/4 KiB final storage and manual/detailed/flat
128/256-target ARM profiles without changing the existing detailed API gates.

[Scalability checks](scalability/README.md) separate typed-table compilation,
exact type diversity, erased indexes, resources, visitor objects and module
dependencies. Their bounded CI profiles gate correctness and build capacity;
larger exploratory runs retain compiler limits as findings. See the
[measured report](../doc/Scalability.md) for the populations and limitations.

Scalar, legacy Field metadata/limits/arguments, JSON adapter, binary v2 and old
ABI suites are retired after their source and measured evidence are archived.
The fourteen obsolete `telemetry_*check.pro` targets referenced removed legacy
sources and are retired too. Their original files remain in Git history and the
local cleanup archive; the active qmake targets live under `tests/structured/`.
Their historical counts and receipts are evidence for those source snapshots;
they do not describe the final C++20 test matrix. The final API has no source,
ABI or wire compatibility promise with v2.1. See the
[migration guide](../doc/StructuredTelemetryV3MigrationGuide.md).
