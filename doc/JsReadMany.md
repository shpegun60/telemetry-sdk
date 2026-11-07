# JS/TS ReadMany

The optional Encoded operation 5 uses the existing JS/TS `DeviceClient` FIFO.
Generate the normal SDK with `--targets js --client-runtime`; scalar/open-enum
Fields carry explicit eligibility metadata. No RuntimeSchema or UI is required.

Host code can inspect discovered limits with
`inspectReadManyCapability(bytes)` from `telemetry-sdk/client`. It returns an
immutable `itemLimit`, `requestLimit`, `replyLimit`, `session` and `fingerprint`,
or `undefined` for an invalid record. This pure function performs no I/O or
admission. A long watchlist can plan successive bounded requests using those
limits; `bindReadMany()` still validates each request against the current session,
field types and complete byte budget. There is no atomic snapshot across chunks.

```ts
import {DeviceClient, EncodedBackend, Status, ReadManyStatus}
    from './generated/js/client.js';
import {scalar, extra, matchesHeader} from './generated/js/device.js';

const backend = new EncodedBackend(sendFrame, {readMany: true});
const client = new DeviceClient({backend, matchesHeader});
// The existing transport delivers complete incoming frames to backend.receive().

async function readSelected(freshSession: number, observedHeader: Uint8Array,
                            trm1Bytes: Uint8Array) {
    if (!client.openSession(freshSession, observedHeader)) return;

    // Discover TRM1 through the application's Resource bootstrap before binding.
    const selected = client.bindReadMany(trm1Bytes,
        [scalar.fields.F00, extra.fields.Wide, extra.fields.Enabled]);
    if (selected.status !== Status.Ok) return;

    const result = await client.readMany(selected.binding);
    if (!result.succeeded || !result.value) return;
    const [number, wide, enabled] = result.value;
    if (wide.status === ReadManyStatus.Ok) {
        // wide.value is bigint; number.value and enabled.value have their own types.
        console.log(wide.value.toString());
    }
}
```

These endpoint names come from the maintained native test Model; the
[strict consumer](../tests/client_read_many_web/Consumer.ts) checks inferred tuple
types and invalid calls. Ordinary application code does not build payloads or
request IDs. Cache the binding for repeated reads in that admitted session.

Select 1-32 distinct scalar/open-enum Fields. Four-byte Structs and Arrays are
ineligible. Handwritten bindings can provide the same explicit `readManyScalar`
underlying ScalarCode marker; old bindings without it remain ineligible.
Local backend opt-in and a matching remote TRM1 record are both required. The
binding freezes identity, IDs, sizes and decoders for one client/session. RPC
returns Unsupported before transfer; there is no hidden individual-read fallback.

The entire reply body, item statuses and canonical scalar bytes are validated
before user decoders run. Results own their bytes and values. A bad later item
cannot publish an earlier partial tuple. U64/S64 remain bigint. JavaScript
allocation and Number's floating-point representation still apply; this is not
the C++ no-heap or bit-exact NaN contract.

This adapter retains the existing JS result shape: a valid checked envelope with
invalid value bytes returns CodecError, retaining dispatch/status/payload without
publishing values. A malformed envelope, budget echo or actual length uses the
existing MalformedReply/recovery path. ReadMany is read-only for outcome
classification, so a lost response does not become mutation OutcomeUnknown;
physical/session recovery can still be required after handoff. It does not
guarantee simultaneous sampling. Explicit retrying backends keep their existing
repeated-execution policy.

See the [runtime ownership contract](../lib/client/web/README.md),
[wire mapping](ReadManyEncoded.md), and
[maintained interoperability qualification](../tests/client_read_many_web/README.md).
