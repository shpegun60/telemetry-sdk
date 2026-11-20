# Telemetry qualification suites

Authors: Ruslan Kovtun (shpegun60), codexAi. MIT license.

These are the maintained C++20 suites for the final `telemetry` library.
The directory name records their development sequence; it does not identify
a second public library. Current includes use `telemetry/Telemetry.hpp`,
providers use `integration/telemetry/v3`, and the JS codec is `web/telemetry.js`.

The [current architecture](../../doc/Architecture.md),
[wire contract](../../doc/WireV3.md) and
[migration guide](../../doc/StructuredTelemetryV3MigrationGuide.md) define
the contracts. Earlier plans/reviews were archived with their original bytes;
[maintenance notes](../../doc/RepositoryMaintenance.md) identify the archive.
An older green CI or receipt does not qualify a later source tree.

## Historical stage navigation

The explicit anchors below preserve links from retained qualification
documents after this index was organized by maintained suite. They locate
the current tests; they do not assign a newer source identity to old results.

<a id="stage-02-stable-reflection-facade"></a>

Stage 02: [reflection](reflection/run.py) and [facade](facade/run.py).

<a id="stage-03-fixed-wire-types-and-enum-dictionaries"></a>

Stage 03: [fixed wire types and enum dictionaries](types/run.py).

<a id="stage-04-codec-workspace-and-object-lifetime"></a>

Stage 04: [codec, Workspace and object lifetime](codec/README.md).

<a id="stage-05-compile-time-typeregistry"></a>

Stage 05: [compile-time TypeRegistry](registry/run.py).

<a id="stage-06-native-service-binding"></a>

Stage 06: [native Service binding](service/run.py).

<a id="stage-07-service-tables-and-first-encoded-model-path"></a>

Stage 07: [Service tables and encoded Model](model/run.py).

<a id="stage-08-mixed-field-command-and-model"></a>

Stage 08: [mixed Field/Command/Model](endpoints/run.py).

| Suite | Evidence |
| --- | --- |
| reflection, facade | Pinned PFR/compiler probes, normalized reflection boundary, callable rules |
| types | Supported fixed-width shape, enum dictionary/code semantics and compile refusals |
| [codec](codec/README.md) | Canonical LE, existing-object/scoped decode, bool validation, exact lengths, object lifetime and Workspace |
| registry | Compile-time deduplication and structural type identity |
| service, model | Native bindings, local/global/encoded routing and ABI controls |
| endpoints | Mixed scalar/enum/array/struct Fields and Commands, storage budgets and codegen |
| borrowed | Const-reference Field/Service outputs, view lifetime, slots, overlap, wire parity and ARM codegen |
| descriptor | Independent binary parser, golden bytes and fingerprint |
| [type_names](type_names/README.md) | Descriptor v3.1 type names and attribute areas: goldens, three decoders, seam, switch, link, configuration and cross-compiler bytes |
| resources | Streaming/packed DescriptorFile, ValuesFile and application resource facade |
| [rpc](rpc/README.md) | Optional RpcMailbox transfer/state contract, checked dispatch, duplicate retries, no-heap, bounded ARM roots and separate [H7S UART integration](rpc/h7s/README.md) |
| [rpc_client](rpc_client/README.md) | Typed outgoing RPC, stable packet storage, retry, schema contract, exact result types and uncertain outcomes |
| [rpc_sessions](rpc_sessions/README.md) | N independent mailboxes, session ownership, ACK tickets, closing/reuse and serialized dispatch |
| traversal | Ordered metadata access, visit/container and runtime readAs/writeAs |
| exchange | Optional Bind/Exchange consumer framing and refusal before callbacks |
| client | JS/C++ interoperability, exact BigInt values and browser behavior |
| qualification | Multi-TU consumer, retained ARM symbols, bounds, leaf/direct codegen |
| mcu | Actual probe bodies on host, ARM O2/Os/Og compile/link and seven-role receipt |
| freeze | API/type/status/ceiling/dependency and immutable v3 byte contracts |

Each runner keeps commands and logs in the selected `--build-dir`. Expected
compile failures require their intended diagnostic. ARM runners inspect
linked symbols, sections, instruction streams and individual `.su` frames;
they execute zero ARM conditions. Host conditions, compiler commands,
declaration checks and mutation controls are reported separately.

```sh
python tests/structured/endpoints/run.py --cxx g++ --build-dir build/endpoints
python tests/structured/endpoints/run.py --cxx clang++-18 --sanitize --build-dir build/endpoints-san
python tests/structured/endpoints/run.py --arm --cxx arm-none-eabi-g++ --build-dir build/endpoints-arm
python tests/structured/client/run.py --node node --build-dir build/client
python tests/structured/rpc/run.py --cxx g++ --build-dir build/rpc
python tests/structured/mcu/receipt.py verify tests/structured/mcu/local-receipt.json --self-test
python tests/structured/mcu/h7s/verify.py --self-test
```

The [H7S runner](mcu/h7s/README.md) requires an explicit selected device,
builds every image before access, checks its internal-Flash load span, and
backs up/restores all 64 KiB with a fresh readback. The current
[receipt](mcu/h7s/receipt.json) covers the later borrowed extension at its
recorded source snapshot. The [current binary-equivalence proof](../resources/evidence/README.md)
relates refreshed source/comment hashes to all twelve unchanged measured images.
The
[baseline results](mcu/h7s/RESULTS.md) retain their original source identity, distinct
from the [pre-unification proof](../../doc/evidence/pre-unification/README.md).
The independent PSP experiment measures observed stack writes across the
complete probe chain; it is not an unconditional maximum stack bound.
The separate [borrowed result suite](borrowed/README.md) and
[H7S comparison](borrowed/h7s/README.md) qualify const-reference outputs.
Their [qualification results](../../doc/BorrowedNativeValues.md#qualification-results)
remain distinct from the owning Stage 20 baseline.

Legacy Scalar/v2 executable tests were retired after the baseline was
captured. Relevant ID/numeric/lifetime/slot/compiler controls remain in
`tests/regression`, generic filesystem checks in `tests/resources`, and
mixed endpoint/codec/wire checks here. Retired review work is in the verified
external archive; retained measured evidence keeps its original source identity.
