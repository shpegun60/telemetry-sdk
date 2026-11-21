# Using DynamicClient

`<integration/dynamic/DynamicClient.hpp>` provides an owner-only C++ runtime
client without a generated SDK. Select the optional CMake component
`dynamic_client`, or include `lib/integration/dynamic/dynamic.pri` with qmake.
It depends on `runtime` and `client`; choose an Encoded, Retrying Encoded or RPC
backend separately. The [implementation contract](DynamicClient.md) defines
the lifecycle and result guarantees.

## Admission and calls

Parse the complete Descriptor into stable caller-owned workspace storage.
The Descriptor and indexes stay immutable until that generation is invalidated.
Construct the backend with its own stable buffers, then construct the client:

```cpp
namespace dyn = telemetry::integration::dynamic;
namespace cl = telemetry::client;

dyn::DynamicClient<8, 4096> client{backend, clock};
auto status = client.admit(schema, observedPeerHeader, session);
if (status != cl::Status::Ok)
    return;

// The path is the descriptor's group/.../endpoint; .member and [i] read a part.
const auto config = client.endpoint("device/Config");
if (!config)
    return; // config.status names the reason; describePath() writes it.

auto submitted = config.handle.read(
    [](const dyn::DynamicResult& result) noexcept {
        // Check operation independently: stale metadata does not erase a
        // successful operation, Accepted, timeout or uncertain outcome.
        if (!result.hasValue())
            return;
        auto baud = result.value.member("baudRate");
        if (baud) {
            auto number = baud.value.get<std::uint32_t>();
            // Consume number here, while the response is borrowed.
        }
    });
```

`endpoint(path)` resolves the path once against the admitted descriptor;
the handle's `write(payload, callback)` and `call(request, callback)` take
canonical bytes, and after another admission a kept handle refuses with
`NotAdmitted` ([handles](GeneratedNamesAndHandles.md#the-dynamic-clients-endpointpath),
[the same operation in four places](user/APIOverview.md#одна-операція-в-чотирьох-місцях)).
Looking the Field up by group and name and binding its ID
(`schema.findField("device", "Config")`, `client.bind(cl::Operation::FieldRead,
field->id)`, `client.readField(bound.endpoint, callback)`) is the lower
level; it stays available.

Check `submitted.status` immediately. A rejected submission invokes no callback.
An accepted submission gets one callback while the owner processes the client.
The callback must be `noexcept`. `CallbackBytes` includes the internal endpoint
snapshot and the callable; oversized captures fail at compile time. Defaults
are eight requests, a 4096-byte request pool and 192 bytes per callback slot.

`bind(operation, id)` resolves live metadata and returns a sealed value holding
the client, schema generation, session, category, capabilities, exact types and
wire sizes. It accepts no caller-supplied `RuntimeEndpoint` as authority. A
binding from another live client or an earlier admission is rejected. Bindings
must not outlive the issuing client; reconstruction at the same address does
not extend a borrowed identity's lifetime.

Use `readField(binding, callback)`, `writeField(binding, payload, callback)`,
`command(binding, payload, callback)` or `service(binding, payload, callback)`.
Void requests also have overloads without `payload`. All use the same FIFO via
`submit(binding, payload, callback)`. Payloads have exact runtime wire sizes and
canonical scalar encodings; no numeric conversion or native DTO copy is implied.
The wrapper exposes no raw `submitEncoded()`, static typed calls or core accessor.

## Transport and completion

Call `process(nowMs)` in the owner's event loop. All clocks use the backend's
same modulo-32-bit millisecond domain. The ordinary complete-packet port contract
applies:

1. Obtain `nextPacket()` and copy its bytes into port-owned storage.
2. Immediately before possible transfer, call
   `prepareHandoff(packet.session, packet.requestId)` and check its result.
3. After the complete packet has been accepted, report `onSendAccepted()`.
4. Deliver received complete packets through `onReply(bytes, session, requestId)`.
5. Retain the port's TX copy until the driver actually releases it.

Schema reset/reparse, client calls, callbacks and backend operations all run in
that one owner context. Concurrent runtime producers and IRQ calls are not
supported. A port must not send a copied packet after its handoff is refused.

`DynamicResult::operation` preserves Client status, dispatch and endpoint status.
`valueStatus` independently reports `Stale`, `NoValue`, `TypeMismatch`,
`InvalidPayload` or `Ok`. Only `Ok` supplies a validated non-Void `value`.
An empty struct can supply a view with zero bytes; a Void response supplies none.
`Accepted` remains distinct from `Executed`.

Response bytes and views expire when the callback returns. For deferred work,
copy bytes first, retain the endpoint binding, and check `client.isCurrent(binding)`
before building a fresh view from that copy. The workspace object must remain
alive. Prepared member handles avoid repeated path lookup, but do not extend
schema or payload lifetimes.

## Invalidation, recovery and replacement

`invalidate()` immediately revokes admission without invoking callbacks.
Resetting/reparsing the workspace or observing a session identity change also
revokes the old binding. Continue owner processing to drain requests. No new
handoff or retry is allowed under that binding. A previously authorized
execution-capable handoff remains evidence of possible execution; correlated
results processed before the deadline and uncertain outcomes remain intact.

New submissions check `Closed`, then `RecoveryRequired`, then `NotAdmitted`,
without calling `process()`, changing FIFO occupancy or invoking a callback.

Before a new `admit()`, complete logical draining and actual transport cleanup:
release TX/DMA copies, exclude all old frames and close/reset the old remote
session. For partial RPC writes this includes releasing/draining the old
`RpcSessionPool` slot even when no final WRITE could execute the endpoint.
Then explicitly attest those facts:

```cpp
bool confirmed = client.confirmQuiescence({
    .session = oldSession,
    .txReleased = true,
    .oldFramesExcluded = true,
    .remoteSessionClosed = true
});
// Only after confirmed: admit the newly parsed schema with a newer session ID.
```

These flags record application evidence; the library does not inspect a DMA
driver or close a remote connection. Confirmation is refused during callbacks,
with occupied requests, incomplete evidence or a current admitted binding.
It performs local backend closure and any required recovery confirmation.
Old endpoint bindings remain invalid after replacement. No request is replayed.

`close()` is terminal. Process until `drained()`, finish physical port release,
and destroy/replace the object. `pending() == 0` and `drained()` alone establish
neither remote cleanup nor physical TX completion.

The [working host example](../examples/dynamic_client/README.md) demonstrates
Read → Edit → Write → Refresh using RuntimeSchema, Writer, the Client FIFO and
the real Encoded dispatcher. Its transport is a host loop, not hardware DMA.
