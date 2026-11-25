# JavaScript and TypeScript connection sessions

`connectStatic()` and `connectDynamic()` coordinate discovery, admission, operations
and local cleanup over one existing client FIFO. They are optional host helpers;
the portable C++ modules and all existing advanced JS entry points are unchanged.

## Start with the reference server

Use the [Telemetry UX WebSocket demo](https://github.com/shpegun60/telemetry-ux/tree/main/examples/web_demo)
to serve a real host Model. Its HTTP bootstrap advertises the WebSocket address;
the server grants an exclusive, monotonically increasing session before its greeting.
This named profile describes that server contract. It does not infer a production
authentication scheme or an arbitrary device's bootstrap protocol.

```js
import {connectStatic, demoWebSocket, Status} from 'telemetry-sdk';
import * as sdk from './generated/js/device.js';

const transport = demoWebSocket('http://127.0.0.1:8080');
const session = await connectStatic({sdk, transport});
try {
    const result = await session.read(sdk.control.Config);
    if (result.status === Status.Ok && result.succeeded && result.valueStatus === 'Ok')
        console.log(result.value);
} finally {
    await session.close();
}
```

The endpoint name above belongs to the generated application Model. Generate the
JS/TS SDK from **your** Descriptor and use its own catalog and binding names.
Typed reads do not require structural metadata. A generic UX adapter also needs
`--structural-metadata`; preservation-safe Field editing needs `--wire-editing`.

For an unknown schema:

```js
import {connectDynamic, demoWebSocket} from 'telemetry-sdk';

const session = await connectDynamic({transport: demoWebSocket('http://127.0.0.1:8080')});
try {
    const schema = session.schema; // validated Descriptor, available while ready
    // Pick an actual readable Field ID from this schema, then:
    // const result = await session.read(fieldId);
    console.log(schema);
} finally {
    await session.close();
}
```

Dynamic values remain `unknown` in TypeScript. Inspect the schema and narrow values
before using them. There is no implicit static-to-dynamic fallback on mismatch.
See the [complete runnable consumer](../examples/connection_session/README.md).

## Operations and results

| Operation | Static argument | Dynamic argument |
| --- | --- | --- |
| `read` | Generated Field/read binding | Current Field ID |
| `write` | Generated Field/write binding and typed DTO | Current Field ID and runtime DTO |
| `command` | Generated Command binding and typed arguments | Current Command ID and runtime arguments |
| `service` | Generated Service binding and typed arguments | Current Service ID and runtime arguments |
| `readMany` | Capability and ordered scalar Field tuple | Capability and ordered scalar Field ID list |
| `batch` | Capability and `Batch.items(...)` typed recipe | Capability and ordered `{operation, id, value?}` list |

For editing a previously read Field, both modes expose
`writePreserving(endpoint, editedValue, originalPayload, timeoutMs?)`. Supply the
checked payload from that Field's read in the same session. The method copies the
seed before user bindings or DTO conversion can run and submits through the same
FIFO and lifecycle tracker as `write()`. Untouched floating leaves retain their
original NaN/signaling/sign/payload bits and signed zero. Invalid input refuses
the whole submission. Static bindings require `--wire-editing`; a missing
preserving helper reports `Unsupported`, without a plain-encoding fallback.
This does not add compare-and-write or protect against concurrent device changes.

A static Session exposes its original `sdk` module and optional `metadata` for
presentation adapters. These are codec/structural context, not access to the
private DeviceClient. Dynamic Sessions expose only their validated `schema`.

Every request keeps `requestId`, `session` and `cancel()` on its Promise. Ordinary
operation failures resolve to a result; invalid connection/bootstrap fails with
`ConnectionError`, whose `code`, message and `cause` identify the failed boundary.

The common result preserves `status`, `dispatch`, `endpointStatus`, `succeeded`,
`accepted`, `executed`, `payload` and the original operation result in `rawResult`.
`valueStatus` independently describes interpretation: `Ok`, `NoValue`,
`InvalidPayload` or `Stale`. An empty Struct is a real value even with zero wire
bytes. A Void response has `NoValue`. Invalidating a session never rewrites a
confirmed operation result as a fictitious cancellation. `Accepted` does not mean
application work has finished. `OutcomeUnknown` never triggers automatic replay.

## Optional ReadMany and Mixed Batch

Both methods use the same Session FIFO, request IDs, capacity and cancellation.
Remote capability records and local backend support are checked separately for
every submission. They never fall back to individual requests. On the reference
profile, opt in explicitly and independently:

```js
import {connectStatic, demoWebSocket, Batch} from 'telemetry-sdk';
import * as sdk from './generated/js/device.js';

const session = await connectStatic({sdk, transport: demoWebSocket(
    'http://127.0.0.1:8080', {enableReadMany: true, enableBatch: true})});
try {
    const capabilities = session.capabilities;
    if (capabilities?.readMany) {
        // Use the scalar Field names generated for your own Model.
        const result = await session.readMany(capabilities.readMany,
            [sdk.control.Counter, sdk.control.Bool]);
        for (const item of result.items ?? [])
            console.log(item.id, item.status, item.valueStatus, item.value);
    }
    if (capabilities?.batch) {
        const result = await session.batch(capabilities.batch, Batch.items(
            Batch.read(sdk.control.Counter),
            Batch.command(sdk.control.Reset)));
        // Inspect each checked result; outer success does not imply item success.
        for (const item of result.items ?? [])
            console.log(item.operation, item.accepted, item.executed, item.valueStatus);
    }
} finally {
    await session.close();
}
```

ReadMany accepts 1–32 distinct scalar/open-enum Fields. Struct/Array selections
are unsupported. Results retain selection order and per-Field availability.
Static TypeScript tuples preserve each generated value type; Dynamic item values
remain `unknown`. Dynamic Batch uses current numeric IDs, for example
`session.batch(capability, [{operation: 'FieldRead', id}, {operation: 'Command', id: commandId}])`.
Repeated Batch IDs are permitted. Batch writes replace the complete DTO; they do
not perform preservation editing, hidden reads or automatic readback.

Optional results expose aggregate `valueStatus` and frozen ordered `items`.
Each item owns separate payload bytes. These bytes remain mutable arrays; freezing
their containing object does not make their contents immutable. `rawResult`
retains the actual checked operation facts, including Static ReadMany `CodecError`
versus Dynamic checked-operation `Ok` plus `InvalidPayload`. A malformed item body
does not invent partial results. A checked Batch mutation remains Accepted or
Executed even if another value is invalid. Neither operation promises atomic or
coherent sampling, transactional execution or exactly-once behavior.

At the final Session Promise projection, a revoked admission hides pending typed
values as `Stale`, preserving checked item facts and known `NoValue` outcomes.
Already-fulfilled results remain historical snapshots. Capabilities are bound to
the current session and schema; reacquire them after reconnect. Uncertain ReadMany
retains a read failure status and requires recovery; uncertain Batch retains its
existing conservative `OutcomeUnknown` behavior. Reconnect never replays either.

The WebSocket carrier limits the **complete** frame to 65,536 bytes by default.
A protocol-maximum 65,535-byte body plus its 32-byte Encoded header exceeds that
limit. Choose request/reply budgets that fit the carrier; no fragmentation or
unbounded growth is added by these Session helpers.

## Lifecycle

`subscribe(listener)` immediately sends an immutable snapshot. It reports state,
readiness, session, fingerprint, last error and recovery diagnostics. The returned
function unsubscribes. Listener exceptions are isolated. A nonready notification
revokes further submission; keep pending request owners alive until their Promises
settle, even if the screen is already displaying a disconnected state.

`disconnect()` invalidates admission, cancels managed requests and awaits their
result projections plus the owned **local** transport close fence. It is reusable.
`reconnect()` explicitly obtains a fresh peer lease, checks schema and recovery
proof, and re-admits the same client. `close()` is terminal for that Session.
There is no automatic reconnect or automatic mutation replay.

A closed local WebSocket does not prove remote work has stopped. The demo profile
uses only a fresh, strictly larger generation granted by the **same** exclusive
server as remote supersession proof. Server generation reset or changed peer is
refused. A failed candidate still consumes its observed generation. Keep one
profile instance for the peer; its retirement record survives replacement of the
Session object. This is not global arbitration against unrelated custom profiles
or advanced clients: the server must enforce its own exclusive ownership rules.

The bootstrap profile owns the socket. Its single absolute timeout covers HTTP
discovery, greeting, Descriptor and capability fetching, without extending the
deadline on each byte. Response bodies are bounded. Capability records are copied
as discovery data only. ReadMany/Batch still require current operation-specific
admission and independent local support. Both reference-profile opt-ins default
to false. Advanced capability binding APIs remain available.

The lower-level `bindWebSocket()` instead **borrows** an already-open socket. Use
`telemetry-sdk/transports/websocket/carrier` to import only that carrier, without
the optional connection coordinator. Its
`dispose()` detaches the carrier, does not close the socket and proves no remote
quiescence. Existing hosts can keep using that contract.

## Custom transports

Implement `ConnectionProfile.open(context)` when using a different bootstrap or
transport. Return an `OwnedConnection` with checked peer identity, nonzero session,
64-byte header, backend, optional full Descriptor, and an idempotent local close
fence. A rejected open must clean up all partially acquired resources. A candidate
replacing an old connection must supply the exact old peer/session it supersedes.
The adapter is trusted to establish the claimed remote lease; a boolean inferred
from socket close is insufficient. See the installed [public declarations](../web/Session.d.ts)
and [deterministic acceptance tests](../tests/connection_session/README.md).

This first stage adds no Qt provider, Configurator or panel ownership system.
Those belong to Telemetry UX and can share this Session rather than creating
another request queue.
