# Headless dynamic JS/TS client

[DynamicClient](../web/DynamicClient.js) binds a fully validated Descriptor to
one session and privately owns the existing asynchronous DeviceClient. It uses
the same Encoded/Retrying/RPC backends and FIFO as generated static bindings.
It adds neither a parser/type graph nor a transport or scheduling engine.

```js
import {parseDescriptor} from './web/telemetry.js';
import {DynamicClient, ValueStatus} from './web/DynamicClient.js';
import {EncodedBackend, Status} from './lib/client/web/Client.js';

const schema = parseDescriptor(descriptorBytes);
const backend = new EncodedBackend(sendFrame, {readMany: true, batch: true});
const client = new DynamicClient({backend});
// The application's existing bootstrap supplies Descriptor/header/session and
// capabilities; its transport delivers complete incoming frames to backend.receive().

async function readConfig(session, observedHeader) {
    if (client.admit(schema, observedHeader, session) !== Status.Ok) return;
    const field = schema.fields.find(field => field.name === 'Config');
    if (!field) return;
    const bound = client.bind('FieldRead', field.id);
    if (bound.status !== Status.Ok) return;
    const result = await client.readField(bound.endpoint);
    if (result.valueStatus === ValueStatus.Ok) console.log(result.value);
    else console.log(result.operation.status, result.valueStatus);
}
```

A descriptor v3.1 names its enums and structs: `type.name` on a kind 2/3 type,
undefined when unnamed. Names are unique, so `findType(schema, name)` from
`telemetry.js`, or `client.findType(name)` for the admitted schema, returns
that type or undefined; `schema.minor` is 0 for v3.0 and 1 for v3.1.
Attribute areas are validated and skipped and never change the model.

Cache opaque endpoint bindings for repeated operations in that admission.
`writeField(endpoint, value)`, `command(endpoint, request)` and
`service(endpoint, request)` take ordinary schema-shaped values. A Void request
uses omitted/undefined arguments; an empty Struct uses an actual empty object.
IDs are resolved in the operation's category, not globally. Writable capability,
exact types/sizes and canonical payloads are checked using the existing runtime
codec. In TypeScript runtime values honestly remain `unknown` until application
code checks them; generated static bindings retain their inferred DTO types.

ReadMany binds TRM1 and 1-32 scalar/open-enum FieldRead endpoint handles with
`bindReadMany(capability, endpoints)`, then `readMany(binding)`. Mixed Batch uses
`batch(tbm1, [{endpoint: writeEndpoint, value: draft}, {endpoint: readEndpoint}])`.
Both use that same FIFO and independently admitted local/remote capabilities.
RPC explicitly refuses these optional operations; there is no hidden fan-out.
Batch remains ordered and non-atomic, with independent outcomes and no rollback
or exactly-once guarantee. An explicit retrying backend may execute again.

## Outcome and value lifetime

Every result has an independent `operation` and `valueStatus`. A current valid
non-Void value is decoded into owned JS data. A current Void response has
NoValue; an empty Struct is a real zero-byte value. Stale binding has precedence
even for Void or failed operations, matching the
[C++ runtime contract](DynamicClient.md).
Known Command Accepted/Executed and Service refusals are not erased when values
become stale or invalid. ReadMany/Batch results additionally expose item results;
inspect each status/valueStatus rather than infer success from the outer result.

The model owns its validated metadata and exact 64-byte header. Admission checks
the full header and model brand, not just a caller-supplied fingerprint. Requests
and results retain the model/token until completion, so no metadata or RX borrow
escapes. Promise requestId/session/cancel are preserved through projection.
JS parsing/decoding, arrays, Promises and result copies allocate normally; this
is not the C++ no-heap contract. U64/S64 remain BigInt. Retained raw bytes preserve
float representations; Number does not preserve every NaN bit after re-encoding.
An editor can retain the checked FieldRead `operation.payload` and use
`writeFieldPreserving(writeEndpoint, editedValue, originalPayload, timeoutMs)`.
This optional typed path uses the same FIFO and codec, snapshots the seed before
reading caller value getters, and preserves its exact F32/F64 NaN bytes whenever
that leaf is still NaN. Finite edits encode normally. Exact seed length and all
scalar representations, including Bool, are checked before a request is admitted.
Wrong type/session handles and full/closed/revoked clients retain their normal
refusal behavior. No payload, editor or second queue is stored in DynamicClient.

## Invalidation and quiescence

Submission checks Closed, RecoveryRequired, then NotAdmitted without advancing
the FIFO or invoking a completion. `invalidate()` revokes all new execution,
including retries and RPC settlement packets. Queued/offered requests complete
Cancelled. An already authorized execution-capable active request can still
receive a correlated reply before its deadline: the known operation survives,
while its value status is Stale. A reply at/after the deadline loses to timeout;
possibly executed mutations retain OutcomeUnknown and recovery.

A denied new handoff does not itself discard the passive receiver. Built-in
Encoded/Retrying/RPC backends preserve it until an existing terminal transport
event or core deadline/abort. Existing in-flight RPC ACK deadlines are unchanged.
Custom backends must honor `handoff()` before every physical send and the optional
`BackendContext.admissionRevoked()` passive mode, as well as the existing physical
release fence. A custom backend is not automatically qualified by this wrapper.

After invalidation, new admission requires explicit
`confirmQuiescence({session, txReleased, oldFramesExcluded, remoteSessionClosed})`
and a strictly newer session. These booleans assert completed application cleanup;
they do not stop DMA, close a socket or release a remote mailbox. Logical pending
count alone is insufficient, including a partial RPC without final WRITE and
without OutcomeUnknown. In-progress producer/property/encoder calls and pending
result projections also prevent rebinding. `close()` is irreversible; `drained`
includes logical/physical completion and these publication scopes.

See the [native all-six-operation and lifecycle qualification](../tests/client_dynamic_web/README.md),
[underlying client ownership](../lib/client/web/README.md), and
[SDK/UX delivery boundary](SdkUxRoadmap.md). This component has no DOM, Qt or UI.
