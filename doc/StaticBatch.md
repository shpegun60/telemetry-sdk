# Typed C++ Mixed Batch

`integration/batch/StaticClient.hpp` adds ordered typed batches to an existing
`DeviceClient`. Generated and handwritten endpoints/Codecs are supported without
RuntimeSchema, Model, Qt, another client or execution queue.
All adapter calls and storage access are owner-only, including when the core
client uses a locking queue policy. This is not a ThreadSafeClient producer API.

```cmake
find_package(Telemetry REQUIRED COMPONENTS batch_static_client encoded_backend)
target_link_libraries(application PRIVATE
    Telemetry::batch_static_client Telemetry::encoded_backend)
```

For qmake, select `CONFIG += telemetry_batch_static_client
telemetry_encoded_backend` before including `lib/integration/integration.pri`.

## Prepare typed items, then submit once

After ordinary schema admission and trusted discovery of this peer/session's
TBM1 capability, construct a selection using ordinary generated endpoint objects:

```cpp
namespace bt = telemetry::integration::batch;
using Codec = device_api::client::CodecPolicy;

// config and arguments are application-owned DTO lvalues.
const auto request = bt::items(
    bt::write(configField, config),
    bt::read(configField),
    bt::command(startCommand, arguments),
    bt::service(statusService)); // This illustrative Service has a Void request.

using Storage = bt::StaticBatchStorage<Codec, decltype(request)>;
Storage storage; // Place it in the session owner; retain through completion/drain.
const auto bound = bt::bindBatch(client, backend, observedCapability, request);
if (!bound)
    return; // Inspect bound.status; nothing was enqueued.

const auto submitted = bt::batch(client, bound.binding, request, storage,
    [](const auto& result) noexcept {
        if (!result.operation.succeeded())
            return; // This is the outer transport/batch result.
        const auto written = result.template get<0>();
        const auto readback = result.template get<1>();
        if (written && written->operation.succeeded() && readback) {
            if (const auto* value = readback->valueIf()) {
                // *value is the typed device value, including normalization.
            }
        }
    });
if (!submitted)
    return; // A refused submission has no callback.

// Drive the same owner process/transport loop as ordinary typed operations.
// This scope and storage must remain alive until the callback closure is destroyed.
```

Endpoint names in this fragment are illustrative. The maintained
[successful example](../tests/batch_static/Example.cpp) uses real generated
endpoints and a native Model. Request arguments use exact declared types and
lvalue references; implicit numeric conversions and borrowed temporaries are
rejected at compile time. Void calls have overloads without a placeholder.

`valueIf()` returns a nullable pointer, combining the value check and access.
`value()` remains checked and fail-fast on invalid use; that failure path can
pull target libc support into a linked binary when a compiler does not eliminate
it. Embedded consumers can use `valueIf()` to avoid that dependency without
assuming a value is present. The ARM qualification uses this checked pointer API
and still refuses allocation symbols.

## Storage and publication

`StaticBatchStorage` is caller-owned and cannot be copied or moved. Its capacity
is derived from the selected types, not the maximum protocol payload. It owns
two private wire buffers: encoded argument bytes and the framed batch request.
`encodedBytes`, `requestBytes` and `responseBytes` expose their wire budgets.
Private typed response slots and copied endpoint metadata are additional native
RAM; use `sizeof(Storage)` for the actual ABI's complete cost. Large storage
belongs in the session object or explicitly placed static memory, rather than a
small task's stack. There is no heap fallback or hidden full DTO in the callback.
Response slots hold the sum of the selected native response types, rather than
only the largest type. This permits one decode pass with whole-batch publication
even when a handwritten Codec can partially modify its destination on failure.

Each store permits one in-flight operation. Several stores can use the same
client FIFO. A store remains busy during encoding, callback ownership changes,
completion, and callable destruction. Reuse from its own callback or destructor
returns Busy. The default callback budget is unchanged; a small capture fits
64 bytes because completion holds pointers rather than all response DTOs.
Larger captures require an explicit client callback capacity.

Arguments remain alive and stable through submission's encoding only. An
accepted submission owns a complete copy in the existing FIFO, so later changes
to the original arguments do not change the request. Client, backend and storage
must remain stationary and alive through completion and closure destruction.
Bindings cannot outlive their client, including reconstruction at the same
address. Custom ReadMany/Batch-capable backends must not reuse admitted session
IDs during that client's lifetime, as the concrete Encoded backends enforce.

The complete reply's ordering, IDs, operations, status envelope and exact
successful lengths are checked before any per-item result is exposed. All
successful non-Void responses decode into private slots. A late malformed or
noncanonical item cannot publish an earlier typed value. Admission is checked
again after encoding, callback ownership and decoding. Stale values never erase
the actual outer or structurally validated per-item execution outcomes.

`get<I>()` returns an optional typed item view. It is empty when no complete
per-item result exists, including outer timeout or malformed framing. Its
presence does not imply success: inspect `item.operation`. `hasValue()` is
independent of operation success; Void has no value, while an empty Struct has
a real zero-wire-byte value. `value()` requires `hasValue()`. Results, payloads,
item views and typed references are borrowed only for the callback; copy needed
values before returning. No existing application DTO is a decode destination.

## Execution semantics

Select 1-32 ordered operations. Repeated IDs and IDs shared by different endpoint
categories are valid. Bind checks permissions, copied metadata, capability and
total request/response bounds. Submission checks the current owner/session,
request contracts and encoding before publication. The existing server executor
performs its global preflight before the first endpoint call.

Batch is sequential and non-atomic. An endpoint refusal does not stop later
items. Command `Accepted` does not mean application work completed; use its
existing `accepted()`/`executed()` distinctions. There is no rollback,
exactly-once guarantee or automatic replay by this helper. A selection of reads
only is classified by the backend as a read: a lost reply after handoff times
out, never OutcomeUnknown; a selection with any write, Command or Service keeps
the potentially mutating OutcomeUnknown classification. Normal recovery remains
mandatory either way.
Unsupported backends and RPC return Unsupported, without hidden fan-out. See
[Encoded Mixed Batch](MixedBatchEncoded.md) for the unchanged wire contract.
For a group of eligible scalar reads, [ReadMany](StaticReadMany.md) has a smaller
wire representation and an explicitly read-only operation classification.
