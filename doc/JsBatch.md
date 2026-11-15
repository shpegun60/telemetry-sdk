# JS/TS Mixed Batch

The optional Encoded operation 6 submits ordered FieldRead, FieldWrite, Command
and Service items through the existing `DeviceClient` FIFO. Generated JS/TS
bindings infer request and response types; handwritten bindings use the same
operation/size/codec contracts. RuntimeSchema and a UI are not required.

```ts
import {Batch, BatchValueStatus, DeviceClient, EncodedBackend}
    from './generated/js/client.js';
import {control, matchesHeader} from './generated/js/device.js';

const backend = new EncodedBackend(sendFrame, {batch: true});
const client = new DeviceClient({backend, matchesHeader});
// Establish and admit a session, discover its TBM1, and forward received frames
// to backend.receive() using the application's existing transport integration.

async function update(draft: control.types.Config, tbm1: Uint8Array) {
    const result = await client.batch(tbm1, Batch.items(
        Batch.write(control.fields.Config, draft),
        Batch.read(control.fields.Config),
        Batch.command(control.commands.Configure, draft),
        Batch.service(control.services.Echo, draft),
        Batch.service(control.services.Ping))); // Void request: no placeholder.

    const written = result.items?.[0];
    if (written?.succeeded) console.log('Write confirmed');
    const readback = result.items?.[1];
    if (readback?.valueStatus === BatchValueStatus.Ok)
        console.log(readback.value.number); // Inferred device DTO, not local draft.
}
```

These endpoints come from the maintained native test Model. The
[strict consumer](../tests/client_batch_web/Consumer.ts) checks the complete
inferred tuple, exact BigInt fields, category errors and Void/empty Struct calls.
Selectors retain arguments only until `batch()` synchronously encodes owned
request bytes; they do not execute, create a second queue or acquire a session.

Select 1-32 ordered items. Repeated IDs are deliberate: a write then read or two
Commands remain independent ordered operations. All contracts, permissions and
bounded request/reply totals are checked before encoding/enqueue. Local Encoded
opt-in and the admitted peer's TBM1 are independent requirements. RPC returns
Unsupported; there is no hidden fan-out or ordinary-operation fallback.

The outer `result` describes the Encoded batch exchange. Each `items[i]` retains
its own dispatch, endpoint status, succeeded/accepted/executed flags and payload.
Inspect these independently: Command Accepted does not mean application work
finished, and a Service refusal is not Command Accepted.

Value availability is separate. The whole inner body, item order and successful
lengths are checked before publication. Decoded values stage privately and are
all withheld if any DTO is invalid or the session becomes stale. Known item
mutation outcomes remain available. A valid checked outer envelope with malformed
inner framing has `valueStatus: InvalidPayload` and no `items`; outer success
alone never authorizes indexing a tuple or using a value. Void has NoValue;
an empty Struct remains a real zero-byte value.

The aggregate Stale/InvalidPayload label suppresses all values. Items that never
carry a response (or whose endpoint failed) retain NoValue rather than acquiring
a fabricated value; callers must check each item's `valueStatus`.

An ordinary Encoded backend performs one exchange. Batch adds no rollback,
atomic snapshot or exactly-once execution guarantee. A lost reply after handoff
retains conservative OutcomeUnknown/recovery, including an all-read batch.
The native client classifies an all-read Batch as a read since November 2026
([Encoded Mixed Batch](MixedBatchEncoded.md)); whether the JavaScript client
follows is open.
An explicitly selected retrying backend may execute items again. Workbench or
application code must not silently replay an uncertain mutation.

Requests, outcomes and values allocate normally in JavaScript. Counts and wire
budgets are bounded; arbitrary user codecs and decoded zero-wire trees still
have their own CPU/memory cost. U64/S64 stay BigInt. Raw owned bytes retain float
representations, while JavaScript Number does not promise bit-exact signaling
NaN preservation after decoding and re-encoding.

See [native interoperability and lifecycle tests](../tests/client_batch_web/README.md),
[C++ typed Batch](StaticBatch.md), [production wire contract](MixedBatchEncoded.md),
and [client transport ownership](../lib/client/web/README.md).
