# Universal code generator

The optional host tool turns a **binary Descriptor v3.0** into C++20 DTOs,
endpoint contracts and standalone C++/TypeScript/JavaScript codecs. The native
STM32 model, structures, reflection, Encoded API, Descriptor/Values v3.0 and
RPC v1.0 retain their existing contracts. Firmware does not run this tool.

## Generate an SDK

Python 3.10 or newer and the repository checkout suffice for C++/JS/TS. No Python packages,
compiler, device, RPC connection or network access are needed for generation.
From the repository root:

```sh
python3 -m tools.telemetry_codegen inspect descriptor.bin
python3 -m tools.telemetry_codegen generate descriptor.bin --targets cpp,ts,js --namespace device_api --output /tmp/device-sdk
python3 -m tools.telemetry_codegen generate descriptor.bin --targets cpp,ts,js --namespace device_api --output /tmp/device-sdk --check
```

`--targets` accepts a comma-separated selection of `cpp`, `ts`, `js`, `qml`.
The optional [Qt/QML target](GeneratedQmlApi.md) requires the matching installed
[`telemetry-ux-codegen` extension](UXGeneratorExtension.md). It uses the existing C++ DTOs and
codecs and automatically includes the unchanged `cpp` output. Qt is needed only
to build that module, not to run the generator. The default remains `cpp,ts,js`.
`--namespace` is a valid ASCII C++ namespace, optionally nested with `::`.
It changes C++ namespaces and their directory paths. JS/TS expose named catalog
exports from `device.js`/`device.ts` instead of an extra namespace wrapper.

Optional `--wire-editing` adds preserving codecs to selected C++/JS/TS targets.
They retain the original NaN wire representation when the edited value is still
NaN; finite values, infinities and signed zero encode normally. Ordinary
`encode` and generation without this option remain unchanged.

C++ adds `codec::encodePreserving(value, originalPayload, output)` with caller-owned,
exact-size, stable and disjoint spans. Complete original-payload validation,
length and overlap checks happen before output changes. No allocation or
scratch DTO is introduced. See the [C++ contract](GeneratedCppApi.md).

JS/TS adds `encodePreserving(value, originalPayload)` to value codecs and writable
bindings; it owns an original-payload copy before reading edited properties.
The [UX browser starter](UXGeneratorExtension.md) automatically enables this
JS/TS option. The optional `--ux-starter qml` instead requires an explicit `qml`
target and `--structural-metadata`; its matching installed UX extension supplies
the Configurator binding and it automatically enables the C++ preserving codec.
A web starter alone keeps its previous C++ output when C++ is also selected.

For the default namespace the outputs are:

| Target | Files |
| --- | --- |
| C++ | `cpp/device_api/Types.hpp`, `Schema.hpp`, `Endpoints.hpp`, `Codec.hpp`, `TelemetryBindings.hpp`, `EncodedBindings.hpp`, `RpcBindings.hpp` |
| JavaScript | `js/device.js`, `js/device.d.ts` |
| TypeScript | `ts/device.ts` |
| QML (optional) | `qml/DeviceApi.hpp`, `DeviceApi.cpp`, `QmlValues.hpp`, `QmlInput.hpp`, `QmlRpcAdapter.hpp`, `DeviceClientAdapter.hpp`, `CMakeLists.txt` |
| Receipt | `manifest.json` |

The receipt records generator version, descriptor SHA-256, schema fingerprint,
wire version, options, generated-file hashes and original-to-generated symbol
mapping. Type IDs remain the original descriptor IDs. Definitions include the
endpoint-reachable dependency closure; arrays use loops instead of expanding
one expression per element. Void support is retained even in an empty schema.

The validated immutable IR is shared by all backends. The production parser
is the existing independently tested parser promoted from
`tests/structured/descriptor/parser.py`; that path remains a compatibility
facade. There is one Python parser implementation.

## Output ownership

Generation validates and renders before publishing a sibling staging tree.
Existing manifests identify owned outputs. Unchanged owned outputs can be
updated or removed when targets change. Modified owned files and collisions
with unowned files are refused. Unrelated regular files are preserved.
Symlinks/junctions are refused. Failed publication restores the previous tree;
an unexpected backup is retained rather than deleting its only copy.

Output directories and their parents must be trusted and must not have path
components concurrently replaced by another process. Ownership checks and
publication are separate filesystem operations, so they do not provide an atomic
guarantee against concurrent symlink or junction substitution. The same
[output-root assumption](Modules.md#cmake) applies to build and installation.

`--check` makes no changes and fails for changed, missing or obsolete owned
outputs, options or receipt data. Generated files are reproducible UTF-8 text
without timestamps. Keep edited application code outside generated files.

## Retrieve a descriptor

Offline input is the primary workflow. A bounded Resource downloader also
discovers an exact path through LIST, validates STAT, follows chunked READ
cursors and validates the complete descriptor and fingerprint before returning.
Its `exchange(bytes) -> bytes` callback consumes one complete Resource packet
and returns one complete reply. The application supplies framing, a timeout,
serialization and peer/session ownership for its transport.

```python
from tools.telemetry_codegen.resource import fetch
descriptor = fetch(my_complete_packet_exchange, "/telemetry/descriptor.bin")
```

For peers that explicitly place complete Resource packets directly in UDP
datagrams, a standard-library adapter is available:

```sh
python3 -m tools.telemetry_codegen fetch --host 192.0.2.10 --port 9000 --timeout 2 --path /telemetry/descriptor.bin --output descriptor.bin
```

This UDP adapter adds no retries or authentication. It cannot be used for an
arbitrary existing UART/TCP/UDP framing protocol. Supply its framing adapter
through the Python callback instead. No TCP framing format is introduced.

## Use and verify

See [generated C++](GeneratedCppApi.md), [generated JS/TS](GeneratedTsJsApi.md)
and [schema admission](GeneratedSchemaCompatibility.md). Runnable examples:
[standalone/CMake/qmake](../examples/codegen_cpp/README.md),
[Encoded](../examples/codegen_encoded/README.md),
[RpcClient](../examples/codegen_rpc/README.md),
[web](../examples/codegen_web/README.md).
The [QML example](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/examples/codegen_qml/README.md) runs the same Qt interface
with a direct Encoded adapter or the existing `RpcClient` and Resource server.

The [repeatable suite](../tests/codegen/README.md) executes native canonical
fixtures, generated C++, generated JS and actual TS-compiled JS, direct Encoded
dispatch and real Resource/RpcMailbox exchanges. It also records identical-DTO
native/generated host timing, compilation, binary sizes and optional Cortex-M7
section/stack reports. Existing native Codec already uses compile-time templates;
generation is not a claim that a runtime type-graph walk was removed from it.
Report measured differences for the selected compiler/profile. Host timings
and offline ARM artifacts do not establish STM32 execution speed.

The [2026-10-18 qualification](CodeGeneratorQualification.md) records executed
gates, review fixes, negative controls and measured native/generated costs.
