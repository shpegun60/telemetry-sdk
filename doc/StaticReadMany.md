# Typed C++ ReadMany

`integration/read_many/StaticClient.hpp` adds scalar ReadMany to an existing
typed `DeviceClient`. It works with generated `ClientBindings.hpp` and with a
handwritten Codec policy. There is no RuntimeSchema, Model, Qt, second client,
execution queue or scheduler in this optional component.

```cmake
find_package(Telemetry REQUIRED COMPONENTS read_many_static_client encoded_backend)
target_link_libraries(application PRIVATE
    Telemetry::read_many_static_client Telemetry::encoded_backend)
```

For qmake, select `CONFIG += telemetry_read_many_static_client
telemetry_encoded_backend` before including `lib/integration/integration.pri`.

## Bind once, read asynchronously

After ordinary schema admission and trusted discovery of this session's TRM1:

```cpp
#include <device_api/ClientBindings.hpp>
#include <integration/read_many/StaticClient.hpp>

namespace rm = telemetry::integration::read_many;

// Use an explicit callback budget for a multi-field selection, for example
// DeviceClient<device_api::client::CodecPolicy, 2, 128, 128>.
const auto selected = rm::bindReadMany(client, backend, observedCapability,
    rm::fields(device_api::measure::VoltageScalar,
               device_api::measure::CurrentScalar));
if (!selected)
    return; // Inspect selected.status; nothing was enqueued.

const auto submitted = rm::readMany(client, selected.binding,
    [](const auto& result) noexcept {
        if (!result.operation.succeeded() ||
            result.valueStatus != rm::StaticValueStatus::Ok)
            return;
        const auto& voltage = result.template get<0>();
        const auto& current = result.template get<1>();
        if (voltage.hasValue() && current.hasValue()) {
            // Use voltage.value() and current.value() with their actual C++ types.
            // Each item also retains its independent id and wire status.
        }
    });
if (!submitted)
    return; // Inspect submitted.status; a refused request has no callback.

// Drive the same client process/transport loop as ordinary read/write calls.
```

Endpoint names are illustrative scalar Fields. The
[successful installed example](../tests/read_many_static/Example.cpp) reads the
actual generated `left::F05` and `mixed::U64` endpoints. The
[minimal installed consumer](../tests/read_many_static/Standalone.cpp) separately
checks isolation and cancellation; the
[real Model tests](../tests/read_many_static/Host.cpp) cover the full lifecycle.
The binding may be reused during its admitted session. The field pack owns
metadata copies; the caller need not retain it after binding.

All calls are owner-only, including the new side-effect-free observations
`DeviceClient::submissionStatus()`, `usesBackend()` and `session()`. This helper
does not add a ThreadSafeClient producer interface. It never calls process merely
to check admission; Closed takes precedence over RecoveryRequired and NotAdmitted.

## Bounds, identity and lifetime

Select 1-32 distinct readable scalar/open-enum Fields, with canonical native
wire widths 1/2/4/8. Structs and arrays are rejected by the typed interface.
Field IDs are `uint32_t`. The Codec supplies a fingerprint matching its exact
`descriptorHeader()`, `wireSize<T>` and canonical decode. Endpoint fingerprints
and declared value widths, when present, must match the Codec.

The concrete backend must enable ReadMany, and the application must bind an
immutable capability to the same peer, fingerprint and session. Capability is
an application assertion, not peer discovery or authentication. Erased backends
and RPC return Unsupported. Encoded backends enforce increasing session IDs;
custom backends offering ReadMany must likewise never reuse an admitted session
ID during the client lifetime.

Bindings cannot outlive the client, including destruction/reconstruction at the
same address. Results, values and `operation.payload` are valid only during the
callback. Copy needed scalar values before returning. No borrowed RX pointer is
stored in a later UI event.

The complete reply envelope, every successful item's exact width and every Codec
decode are checked before any item reports `hasValue()`. Invalid data never
publishes an earlier valid prefix. A failed item keeps its own wire status and
has no value without hiding successful siblings. `value()` aborts if `hasValue()`
is false; check it first. Interpret item IDs and wire statuses only after the
outer operation succeeds and the overall `valueStatus` is Ok. A failed or stale
completion has no usable per-item metadata contract. Admission is checked again after user callback ownership
and after Codec decoding. Stale value status does not overwrite the actual
operation result.

Selection IDs are captured in bounded callback storage. The default capacity
is unchanged: a one-field/small-callback request can use the default 64 bytes;
larger selections or captures require explicit `CallbackBytes`. A compile-time
diagnostic reports insufficient capacity. The installed tests use
`DeviceClient<Codec, 2, 128, 128>` for the two-field example and
`DeviceClient<Codec, 4, 1024, 256>` for up to 32 fields. These are tested budgets,
not a promise that arbitrary user captures fit on every ABI. Payload storage remains the existing
client FIFO's budget. Replies and scalar decode storage are bounded by the
selected pack; there is no heap fallback or hidden large DTO scratch.

ReadMany is sequential and read-only, without an atomic snapshot guarantee.
Timeout never becomes mutation OutcomeUnknown merely because ReadMany was
handed off, but normal backend recovery rules still apply. For one Field,
ordinary FieldRead has a smaller wire request. There is no implicit fan-out,
polling, retry policy change or new protocol. See
[wire/admission mapping](ReadManyEncoded.md) and the
[SDK/UX roadmap](SdkUxRoadmap.md).
