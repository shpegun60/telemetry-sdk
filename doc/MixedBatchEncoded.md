# Optional Encoded Mixed Batch

Typed C++ clients can use the optional
[Static Batch adapter](StaticBatch.md) without RuntimeSchema. The runtime client
API described below remains available independently.

Mixed Batch groups 1-32 FieldRead, FieldWrite, Command and Service operations in
one complete Encoded request and one completion through the existing FIFO.
It is a separate optional mapping, operation **6**. ReadMany (operation 5, TRM1)
keeps its existing scalar-only, read-only contract.

## Execution contract

The server checks the complete body, operations, endpoint metadata, exact input
lengths, canonical input representations, maximum response space, scratch space
and buffer overlap **before the first endpoint call**. A global preflight
failure executes no item. The transport must supply a complete frame; receiving
a TCP fragment or part of a COBS frame is not authorization to dispatch it.
COBS framing alone does not supply a CRC or execution atomicity.

After preflight, items execute synchronously in request order. Repeated endpoint
IDs are allowed: write-then-read of one Field and repeated Commands are useful
sequences. Each item retains its existing dispatch and endpoint status. An
endpoint refusal or runtime failure does not stop later items. Command Accepted
continues to mean accepted, not completed application work. Successful reads
after a write observe the application's resulting value, including normalization.

There is no rollback, durable journal, duplicate-result cache or atomic snapshot.
A reset can interrupt execution after any item. A lost response can hide already
executed items. The application may explicitly choose the existing bounded
retrying backend and permit repeated execution of the complete Batch. Repeated
attempts preserve the existing transaction identity and bytes; they can execute
all items again. A successful response describes the answered attempt, not the
number of executions. Exhausted attempts after an authorized handoff retain
OutcomeUnknown and the existing recovery requirements. Ordinary EncodedBackend
does not retry. This component adds no retry scheduler.

For conservative and predictable behavior, operation 6 is classified as potentially
mutating even when one particular Batch contains only reads. Use ReadMany or
ordinary FieldRead for the established read-only timeout classification.

## Capability and budgets

The owner explicitly admits an immutable **TBM1** record for its Model fingerprint
and nonzero session. TRM1 does not enable Mixed Batch. `EncodedProfile` serves
the record as `/telemetry/batch.bin`, the path a `ConnectionSession` looks for
([device file conventions](DeviceFileConventions.md)); other hosts may use an
application-selected Resource file or another existing trusted discovery path.
The library adds no Resource tag, connection or discovery queue.

All integers below are little endian. The capability is exactly 24 bytes:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `TBM1` |
| 4 | 1 | Version 1 |
| 5 | 1 | Item limit, 1-32 |
| 6 | 1 | Complete-frame Encoded mapping, 1 |
| 7 | 1 | Ordered mixed operations with independent results, 1 |
| 8 | 2 | Maximum request body bytes, 10-65535 |
| 10 | 2 | Maximum reply body bytes, 14-65535 |
| 12 | 4 | Nonzero admitted session |
| 16 | 8 | Descriptor fingerprint |

Count and byte limits are independent. Thirty-two items do not authorize thirty-two
arbitrarily large DTOs. Native Codec and Descriptor continue to determine each
DTO's exact wire size. The sequential executor needs the maximum operation
scratch requirement, rather than the sum. The owner supplies all payload and
scratch storage and serializes access to it.

## Body mapping

The ordinary 32-byte TEQ1 envelope uses operation 6, endpoint 0, endpointStatus 0.
`expectedResponseBytes` is a maximum complete reply-body budget, not an exact
successful DTO size. The request and successful reply bodies start with U16 count.
There are no nested Batch or ReadMany items. Operations inside the body are the
existing values 1 (FieldWrite), 2 (Command), 3 (Service), 4 (FieldRead).

Each request item contains:

| Bytes | Meaning |
| --- | --- |
| 4 | Endpoint ID |
| 1 | Operation 1-4 |
| 1 | Reserved zero |
| 2 | Request payload length |
| length | Canonical request DTO; empty for FieldRead and Void |

Each reply item contains:

| Bytes | Meaning |
| --- | --- |
| 4 | Echoed endpoint ID |
| 1 | Echoed operation |
| 1 | Existing dispatch status |
| 1 | Existing operation-specific endpoint status |
| 1 | Reserved zero |
| 2 | Returned payload length |
| 2 | Reserved zero |
| length | Successful FieldRead or Service DTO |

Reply count, order, operation and ID must match the request, including duplicates.
Failed items and successful Writes/Commands carry no DTO bytes. Dispatch failure
requires endpointStatus zero. The entire body, all statuses and all successful
canonical DTOs are validated before any borrowed runtime value is published.
Void Service success has no value; a zero-byte Struct has a real empty value.
Trailing bytes, invalid reserved fields, unexpected records or lengths are errors.

The outer successful dispatch means the Batch was processed and item results are
available; it does not imply every item succeeded. A global dispatch failure has
no item body. Each application's result handling must inspect individual outcomes.

## Ownership and integration

The portable executor is in `telemetry/encoded/Batch.hpp`, without transport,
Resource, Client or Qt. `integration/encoded/BatchDispatcher.hpp` admits the
Encoded mapping. `integration/batch/Client.hpp` binds runtime endpoint contracts
and submits one copied request through DynamicClient's existing private core.

The optional CMake components are `batch_server` and `batch_client`. The original
four-operation dispatcher and default Encoded backend do not implicitly admit
Batch. RPC, generated JS/TS and the GUI have no Batch mapping in this stage.

```cmake
find_package(Telemetry REQUIRED COMPONENTS batch_client encoded_backend)
target_link_libraries(app PRIVATE Telemetry::batch_client Telemetry::encoded_backend)
# A local server additionally selects Telemetry::batch_server.
```

The [complete Host example](../examples/batch/Host.cpp) executes all four kinds
and reads back the resulting value. Once admitted, its submission is simply:

```cpp
const auto bound = batch::bindBatch(client, backend, capability, endpoints);
if (!bound)
    return;
const auto submitted = batch::batch(client, bound.binding, payloads, requestScratch,
    [](const batch::BatchResult& result) noexcept {
        // Check the outer operation, then each item's operation and hasValue().
        // Retained values must be copied before this callback returns.
    });
if (!submitted)
    return;
```

Here `batch` aliases `telemetry::integration::batch`. `endpoints` contains
operation/ID pairs, and `payloads` contains one canonical input span per item,
including an empty span for each FieldRead or Void request. Existing Codec,
generated Codec or DynamicValueWriter produces inputs; Batch adds no serializer.
For qmake select `telemetry_batch_client` or `telemetry_batch_server` through
`lib/integration/integration.pri`.

Bindings belong to one live client, schema generation and session. They copy
endpoint/type/size contracts, must not outlive that client, and cannot migrate to
a new admission. A caller-owned request scratch span avoids a 64-KiB automatic
buffer. Accepted submission copies its body into the existing FIFO. Inputs and
scratch must remain stable and disjoint from client/control storage during
submission, including user callable ownership operations. Callback storage is
selected explicitly to hold the bounded captured contracts; base client storage
does not grow silently.

Responses and DynamicValueViews borrow backend bytes only until callback return.
Schema invalidation prevents new execution and runtime views, while preserving
correlated operation and item execution results. Replacement admission still
requires drain, recovery and actual transport quiescence. No API here establishes
physical TX/DMA release.

## Validation boundary

The maintained [Batch tests](../tests/batch/README.md) cover the byte contract,
global preflight, real Host Model execution, lifecycle, retries and installed
components. ARM compile/link and compiler stack records are offline evidence,
not physical execution or whole-firmware RAM/Flash measurements. This stage does
not program a board or qualify sustained DMA/cache operation.

Local GCC 13 `-O2` and Clang 18 `-O1` ASan/UBSan qualification each executed
15,327 wire checks, 87 server checks, 436 client checks and the same 436 checks
with allocation traps. Six intentional allocation controls and two negative
compilations passed per profile. The installed-package example produced
`Service=28; Read=100`. New and existing ReadMany qmake inventories each checked
seven selections. Independent `core`, `batch_client` and `batch_server` installed
consumers passed their isolation checks. These local results do not describe a
future GitHub Actions run.

## Measured storage scope

On the tested x86-64 ABI, BatchBinding occupies 848 bytes and BatchResult 2848
bytes. Selecting `DynamicClient<4,1024,1536>` instead of `<4,1024,192>` increases
that object's size from 2416 to 7792 bytes, exactly four callback-storage
increments. This is an explicit selection, not a change to default client
storage. Caller request scratch, TX/RX, server Workspace and user callback data
have their own budgets.

For the selected offline Cortex-M7 roots, linked `.text` was:

| Root | `-Os` | `-O2` |
| --- | --- | --- |
| Legacy dispatcher | 2796 B | 2780 B |
| Opt-in Batch dispatcher | 6912 B | 7564 B |

These are selected roots, not an incremental full-firmware Flash measurement.
The compiler reported local client-probe frames of 4880/4920 bytes, completion
frames of 3248 bytes and dispatcher frames of 1288/1296 bytes. Frames can nest;
none of these numbers is a total peak stack bound. Embedded applications must
size and measure their owner stack for the selected client, callback and DTOs.
The bounded runtime binding/result arrays account for real stack and RAM even
though they allocate no heap.

The ARM `-Os/-O2` evidence is retained for unchanged compiled inputs. A later
correction to the host-only example and host check-count floors was independently
mapped to that inventory; the ARM test was not executed again for those changes.
