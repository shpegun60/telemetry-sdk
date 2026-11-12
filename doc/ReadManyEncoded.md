# Optional Encoded ReadMany

Typed generated/handwritten C++ clients can select the independent
[Static ReadMany adapter](StaticReadMany.md). The runtime adapter described
below retains its existing API and component dependencies.

This page describes the C++ complete-frame Encoded v1 mapping. It reads
1–32 distinct scalar/open-enum Fields from any catalogs through **one existing
DynamicClient FIFO request**. Struct and Array Fields are unsupported even when
their wire size is small. Scalar lengths are 1, 2, 4 or 8 bytes.

This is sequential reading, without an atomic snapshot guarantee. Use an
application Aggregate/Snapshot Field when values must represent one instant.
For one Field, normal FieldRead remains smaller. This adapter adds no implicit
fan-out or polling scheduler, and RPC does not support this operation.
[JS/TS ReadMany](JsReadMany.md) uses the same Encoded mapping. The optional
[Qt/QML Watchlist](ReadManyWatchlist.md) and
[browser Configurator](https://github.com/shpegun60/telemetry-ux/blob/main/web/README.md)
provide polling above the existing FIFO. Device firmware enables ReadMany
explicitly, as in the optional NUCLEO profile. Existing Inspector/Workbench and
four-operation SDKs keep working.

## Select the modules

```cmake
find_package(Telemetry REQUIRED COMPONENTS read_many_client encoded_backend)
target_link_libraries(app PRIVATE Telemetry::read_many_client Telemetry::encoded_backend)
# A server instead selects Telemetry::encoded_server and Telemetry::core.
```

For qmake, select `CONFIG += telemetry_read_many_client telemetry_encoded_backend`
before including `lib/integration/integration.pri`. A server selects
`CONFIG += telemetry_read_many_server` (which also selects the existing Encoded
server). A protocol-free core-only caller selects `telemetry_read_many_core`
before including `lib/telemetry/telemetry.pri`. These explicit source selections
avoid linking batch objects into old qmake applications without section GC.
Resource is not required to execute
ReadMany. The [host example](../examples/read_many/Host.cpp) also demonstrates
discovery of an ordinary application-owned `resource::BytesFile` capability.

The optional client header is
[`integration/read_many/Client.hpp`](../lib/integration/read_many/Client.hpp).
The base `dynamic_client` target still compiles without any wire, Resource,
Native Codec, Qt or server headers. It owns the only client and queue; the
ReadMany functions have no independent client object or scheduler.

## Admission and API

The application obtains Descriptor v3 and the capability through its trusted
discovery path. Neither DynamicClient nor the batch helper performs discovery
or authenticates a peer. Publish an immutable capability for the admitted
Model/session and replace it only through the ordinary session lifecycle.

```cpp
namespace rm = telemetry::integration::read_many;
namespace en = telemetry::integration::encoded;
namespace dy = telemetry::integration::dynamic;

en::EncodedBackend backend{tx, rx, clock, 30000, true}; // Explicit local opt-in.
dy::DynamicClient<4, 130, 640> client{backend, clock};
// Check client.admit(schema, observedHeader, session) first.
const auto binding = rm::bindReadMany(client, backend, observedCapability, fieldIds);
if (!binding)
    return;
const auto submitted = rm::readMany(client, binding.binding,
    [](const rm::ReadManyResult& result) noexcept {
        if (!result.operation.succeeded() || result.valueStatus != dy::ValueStatus::Ok)
            return;
        for (std::size_t i = 0; i < result.count; ++i) {
            const auto& item = result.items[i];
            // Check item.status and item.hasValue(), then use DynamicValueView.
            // Copy retained values before this callback returns.
        }
    });
if (!submitted)
    return;
// Run the existing owner process/transport loop; no new FIFO or thread.
```

The concrete enabled Encoded/RetryingEncoded backend must be the client's own
backend. An erased `client::Backend&`, RPC backend, missing capability or
unsupported profile is rejected explicitly. Bind checks schema/session,
readability, unique known IDs, supported types, request bytes and worst-case
response bytes. Failure does not reserve FIFO space or invoke a callback.
Submission uses `Closed → RecoveryRequired → NotAdmitted` priority without
advancing the state machine. QueueFull leaves the binding reusable.

The binding copies IDs, TypeIds, widths and session/schema identity. Accepted
requests copy that binding into their callback, so source arrays and the
binding variable may be released after submission. The client and schema
workspace object must outlive every binding and callback. Descriptor/index
storage follows RuntimeSchema's generation/lifetime contract. Bindings must
not outlive their issuing client, even if a new object reuses its address.

`CallbackBytes=640` explicitly reserves room for a 32-item binding and a small
application callback; a larger callback can need more. Default DynamicClient
storage stays unchanged. Payload capacity must cover `2 + 4*N` request bytes;
TX/RX must additionally cover the 32-byte TEQ1 header and response ceiling.
This callback storage is a deliberate per-slot RAM cost, not a hidden heap.
For the tested x86-64 ABI, `DynamicClient<4,130,192>` occupies 1536 bytes and
the explicit 640-byte callback variant occupies 3328 bytes (+1792, exactly
4 times the callback-storage increase). Binding size is 464 bytes and the
callback's bounded ReadManyResult is 1824 bytes. These are object sizes, not
stack peaks or universal ABI constants; the result and parser also consume
stack during completion. ARM compiler frame records are retained separately.

## Capability record: TRM1

The standalone [wire helper](../lib/wire/ReadManyWire.hpp) reads/writes exactly
24 little-endian bytes. Prototype `RCP0`/`RMX0` records are not production data.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `TRM1` |
| 4 | 1 | Capability version 1 |
| 5 | 1 | Item limit, 1–32 |
| 6 | 1 | Mapping 1: complete-frame Encoded v1 |
| 7 | 1 | Profile 1: scalar/open enum, at most 8 bytes |
| 8 | 2 | Request body limit, 6–130 |
| 10 | 2 | Reply body limit, 10–514 |
| 12 | 4 | Nonzero admitted session |
| 16 | 8 | Descriptor fingerprint |

Limits apply independently: the advertised item count need not fit every
possible collection into the byte budget. The application binds this record to
its Model. The record carries no path, and the Descriptor and Resource wire
formats gain no tags. The TCP profiles serve it as `/telemetry/read-many.bin`, the path a
`ConnectionSession` looks for ([device file conventions](DeviceFileConventions.md));
an application that discovers it some other way may name its file as it likes.
The server trusts that application binding; it does not reserialize/hash a
Descriptor on every read.

## Encoded request and response

TEQ1 retains its 32-byte header, magic and version. New optional opcode **5**
has endpoint ID 0 and endpoint status 0. Its `expectedResponseBytes` field is a
reply **ceiling** (10–514), echoed unchanged. Operations 1–4 retain their exact
response-size rules. Old peers may reject opcode 5; absence of capability means
the application must explicitly choose ordinary FieldRead instead.

Request body: `count:u16`, then `count` ordered packed Field IDs (`u32`).
Response body: `count:u16`, then ordered records:
`id:u32, status:u8, reserved:u8=0, length:u16, value[length]`.

| Item status | Code | Value bytes |
| --- | --- | --- |
| Ok | 0 | Exactly the admitted scalar/enum width |
| NotFound | 1 | None |
| UnsupportedType | 2 | None |
| Unavailable | 3 | None |
| ReadError | 4 | None |

No TypeId comes from the reply. The runtime client uses its captured schema.
It validates the complete count, order, IDs, statuses, reserved bytes, lengths,
trailing extent and every successful scalar's canonical representation before
exposing any borrowed item view. Unknown open-enum codes are valid. Bool is
only 0/1; IEEE floating bits retain NaN payloads and signed zero.

Global errors use the existing dispatch status in a header-only response.
Malformed envelopes, wrong session and absent opt-in retain the dispatcher
admission/error precedence. Per-item failures do not poison successful siblings.
The overall operation result and value status remain independent: malformed
body does not invent operation success/failure; stale schema prevents views
without erasing a confirmed operation outcome.

## Server and lifecycle

The old five-argument Encoded dispatcher remains batch-disabled. The opt-in
overload takes the admitted capability as its sixth argument; its implementation
is a separate archive object. Legacy roots do not pull the batch executor.

The transport-independent [`readManyEncoded()`](../lib/telemetry/encoded/ReadMany.hpp)
performs all count/duplicate/overlap/metadata/response-capacity/workspace checks
before the first getter. Worst-case reply space is `2 + 8*N + sum(widths)`;
scratch is the maximum existing getter requirement, checked against remaining
Workspace space including alignment. One 8-byte staging area prevents a failing
getter from publishing partial bytes. Unknown IDs/types become per-item errors.
Metadata and arbitrary native endpoint-owner aliases remain caller preconditions,
as for the existing Encoded API.

ReadMany is read-only for timeout/cancellation uncertainty. It never becomes
OutcomeUnknown merely because its handoff was authorized. Physical recovery is
still required after an unresolved authorized transfer. Its packet `mayExecute`
remains true because getters may run; invalidation forbids new handoffs, while
a correlated response before deadline may retain its actual outcome. A stale
schema yields no DynamicValueViews. New admission requires the existing logical
drain, recovery and application-confirmed physical transport quiescence.

Ordinary EncodedBackend sends once. A deliberately selected RetryingEncodedBackend
can retry the same transaction identity according to its existing policy; this
does not guarantee coherent readings, exactly-once getters or automatic fallback.

## Qualification

See [maintained tests](../tests/read_many/README.md) for real Model/Encoded/FIFO,
phase-specific deadlines, malformed replies, allocation traps, relocated consumers
and offline ARM selected roots. Existing DynamicClient, Inspector/Workbench,
generated SDK and wire-freeze regressions remain independent.

The optional [UART console example](../examples/read_many_uart/README.md) uses
the existing serial transport, Resource discovery and the same DynamicClient
FIFO. The [NUCLEO-H7S3L8 qualification](evidence/read-many-uart-2026-11-01/README.md)
records physical 8/12/16/32-field batches, controlled timeout/recovery and
byte-verified original Flash restoration. Sustained UART/DMA/cache load, physical
cable interruption remain separate hardware qualification work. The
[Inspector watchlist](ReadManyWatchlist.md) is implemented separately in UX;
its host tests do not extend those historical hardware results.
