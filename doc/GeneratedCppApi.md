# Generated C++ API

Include `device_api/Codec.hpp` with the SDK's `cpp` directory in the include
path. Types, schema constants, endpoints and codec need only the C++20 standard
library: no Telemetry, Boost.PFR, magic_enum, heap, RTOS or network dependencies.

Address endpoints by path. For every descriptor `ClientBindings.hpp` gives
the device's handles, `device.read<"adc/Config">(callback)`,
`device.adc.channel1.Gain` and `device_api::client::endpoint<"adc/channel1/Gain">(device)`,
and the group namespaces hold their endpoint constants without a family level
(`codec::encode(adc::Calibrate, request, bytes)`; for a v3.0 descriptor one
group per catalog name). Named types (v3.1) are `types::adc::Config`. See [type names, group trees and endpoint
handles](GeneratedNamesAndHandles.md) and the
[API map](user/APIOverview.md#одна-операція-в-чотирьох-місцях). The examples
below use the `device` group's constants (`device::Inspect`) of a v3.0
fixture.

## Types and operations

Each reachable descriptor TypeId has one canonical definition in `detail::Type_<id>`.
Readable aliases live in `catalog::types`; endpoint constants are separated
into `catalog::fields`, `catalog::commands`, `catalog::services`. A scalar Field
aliases its scalar directly. Void and a zero-byte empty struct remain distinct.
Member and export names are normalized deterministically; the manifest maps
every original name and enum code to its generated symbol. Catalog names that
are identical across categories share one namespace.

```cpp
#include <device_api/Codec.hpp>
using namespace device_api;
device::types::InspectRequest request{};
std::array<std::byte, codec::wireSize<decltype(request)>> bytes{};
auto status = codec::encode(device::Inspect, request, bytes);
// Custom transport: endpoint::Operation::Service, endpoint.id, bytes.
```

FieldRead has no request and a Field value response. FieldWrite has a value
request and no response. Command has struct/void request and no response.
Service has struct/void request and struct/void response. Endpoint metadata
contains category, packed ID, original TypeIds, wire sizes, capabilities and
schema fingerprint. A Field's `readOperation` and `writeOperation` are distinct.

## Codec contract

`codec::encode(value, output)` and `codec::decode(input, value)` return
`codec::Status`. Endpoint overloads encode Command/Service requests and decode
Field/Service responses. Both spans must have the **exact** wire size. The
codec uses canonical little endian values, bool bytes 0/1 and IEEE bit patterns.
Enum values need only fit their underlying integer; dictionary labels do not
close their domain. Native padding never enters the payload.

The source object and output bytes, or input bytes and destination object,
must not overlap. Overlap and incorrect length return an error. Decode first
validates every bool without modifying the output, then commits; invalid bytes
leave output unchanged. It does not put a second large DTO on the stack.
Array element loops and all payload buffers are caller owned. Input bytes must
remain stable throughout a call. The codec supplies no synchronization.

## Optional preserving edits

Generate C++ with `--wire-editing` to add
`codec::encodePreserving(value, originalPayload, output)`. Both byte spans have
exactly `codec::wireSize<T>` bytes. The original span and native value must be
disjoint from output and stable throughout the call. Invalid length, overlap or
noncanonical original bytes refuse before changing output.

If both original and new F32/F64 values are NaN, the original sign, signaling
bit and payload are preserved. Other leaves use the edited typed values. This
supports an editor that changes one member without canonicalizing unrelated
NaNs. It is allocation-free, uses the existing generated adapter/writer, and
adds no RuntimeSchema, Qt, transport or hidden scratch DTO. Zero-byte structures
and arrays remain valid. Ordinary output without this opt-in is unchanged.

The [focused suite](../tests/cpp_wire_editing/README.md) covers nested types,
canonical/overlap failures, exact float bits, no-heap controls and ARM probes.

## Direct Encoded dispatch

`EncodedBindings.hpp` adds optional local helpers over the existing ModelView:

```cpp
auto outcome = device_api::encoded::callService(model.view(),
    device_api::device::Inspect, request,
    requestBytes, responseBytes, workspace, response);
if (outcome.ok()) { /* use response */ }
```

Helpers are `readField`, `writeField`, `executeCommand`, `callService`, including
void-request/response overloads. They preserve operation, dispatch status,
endpoint status, written count and codec status. `dispatched` distinguishes
local codec rejection from an actual dispatch attempt. A response is decoded
only after dispatch success, endpoint status zero and exact response length.
This applies even when the response is an empty struct. Command Accepted stays
visible through `commandAccepted()` and its endpoint status; `.ok()` means status
zero, not physical completion. The Accepted helper also checks the operation family.

Native endpoint DTOs and generated DTOs are separate C++ types. The Encoded
path joins them by canonical bytes. It requires the normal native adapter
sources but no RpcClient, RpcMailbox, Resource protocol, ACK or retry machinery.

## Existing RpcClient

`RpcBindings.hpp` adds `rpc::binding(endpoint, trustedRpcMapping)`, returning an
existing typed FieldEndpoint/CommandEndpoint/ServiceEndpoint. The caller first
admits the peer schema for the current session, then explicitly asserts trust:

```cpp
namespace rpc = resource::telemetry::v3;
client.beginService(device_api::rpc::binding(
    device_api::device::Inspect, ::telemetry::client::rpc::trustedRpcMapping), request);
// Drive process/nextPacket/onSendAccepted/onReply as documented by RpcClient.
// Once completed, client.decodeResult(response) enforces its status/type gate.
```

RpcClient uses the existing `telemetry::encode/decode` with the generated DTO.
Its public API and state machine are unchanged. `TelemetryBindings.hpp`
provides explicit enum dictionaries, including sparse 64-bit and empty enums,
so this optional native Codec route does not depend on magic_enum's search
range. Include that header for direct `telemetry::encode` on generated DTOs;
RpcBindings includes it automatically. The standalone route does not need it.

See the [complete examples](../examples/codegen_cpp/README.md) and
[schema compatibility contract](GeneratedSchemaCompatibility.md).
