`Client.ts` is the single runtime source. `Client.js` and `Client.d.ts` are emitted
with TypeScript 5.9.3, ES2022, strict mode. `build.py` compiles externally and checks
that these files match; `--update` deliberately publishes fresh emitted files.

`DeviceClient` owns request byte snapshots, one FIFO, and one active backend
transaction. It requires generated operation metadata containing byte sizes and
codecs, plus the generated `matchesHeader` identity check. Bindings remain pure
standalone codecs; the CLI copies the runtime only with `--client-runtime`.

The runtime uses Promise, AbortController, queueMicrotask and a monotonic clock.
Each accepted call resolves exactly once. The next backend invocation follows
synchronous Promise result reactions. Async work started inside a `.then` handler
does not hold the queue. All methods execute on one JavaScript owner event loop.
One backend instance and mailbox belong exclusively to one client.

For unknown schemas, the optional [headless DynamicClient](../../../doc/DynamicJsClient.md)
privately owns this same client and binds a full validated Descriptor/session.
It keeps operation outcomes independent of stale or invalid runtime values.

Logical completion and physical I/O ownership are separate. Cancellation and
enqueue deadlines resolve requests promptly, including requests waiting in the
FIFO, but no subsequent backend invocation starts until the previous invocation
releases physical I/O. A backend's `execute()` Promise must settle only after its physical I/O
has stopped using the transport, or return a checked `WireResult` with an optional
`released` Promise. That fence preserves the known result immediately while
retaining physical ownership until it settles. Both fulfillment and rejection
mean I/O has quiesced, and neither changes the checked endpoint outcome. Backend
cleanup must precede fence settlement. RPC ACK timeout requests abort; a retry of
the same packet waits for the actual `exchange()` Promise to settle. AbortSignal is
a cancellation request, not evidence that the transport has drained. This also
applies to an asynchronous Encoded send and `EncodedBackend.fromExchange()`.

An exchange that never settles can therefore keep physical progress blocked;
queued deadlines and cancellation still work, without spawning extra transport
waiters. To replace that transport, stop admission with `resetSession()` (or
`close()`), actually close/drain its I/O and the remote retained session, then call
`confirmSessionClosed(oldGeneration)`. Confirmation accepts only the current,
unadmitted session with no pending requests and an aborted old invocation. An
optional backend `confirmSessionClosed(generation)` hook must synchronously return
the boolean `true` before the client detaches that invocation; refusal or an
exception preserves ownership.
Truthy objects and asynchronous hooks are refused. Await application cleanup
before invoking confirmation. This is an assertion about completed external cleanup, not an I/O cancellation or
drain implementation. Only a strictly newer generation may then be opened.
Late old settlements are ignored. Re-admitting a descriptor alone does not detach
old I/O or clear recovery. `drained` becomes true after `close()` only when no
logical requests or backend invocation remain; `pendingCount === 0` alone is not
physical drain evidence. No finite physical drain time is guaranteed.

`invalidateAdmission()` revokes future execution while preserving an already
authorized active request for passive completion or its existing deadline.
Queued and unprepared work is cancelled. Backend authors must check `handoff()`
before every physical send. If it refuses and `context.admissionRevoked?.()` is
true, retain passive correlation without another send; existing transport errors,
packet deadlines and the owner's abort still terminate it. The built-in Encoded,
Retrying Encoded and RPC backends implement this optional contract. A custom
backend that immediately fails every refused handoff does not provide this
passive-completion guarantee. Invalidation alone proves no physical quiescence.

Capacities bound accepted request count and total snapshot bytes; JS allocation
and application codec costs still apply. The snapshot budget does not include
Promise objects, packet framing, decoded results or the caller's retained results.
The request is encoded synchronously before enqueue, and its deadline starts before
that encoding. Results own their payload and decoded generated values.
One completed or aborted invocation can retain its request and packet bytes until transport
settlement or explicit closure confirmation, independently of the accepted queue
snapshot budget.

See the [runnable example](../../../examples/client_web/README.md) and
[qualification runner](../../../tests/client_web/run.py).

## Optional Encoded ReadMany

Use `new EncodedBackend(sendFrame, {readMany: true})`, discover remote TRM1 and
call `client.bindReadMany(trm1, [sdk.device.Voltage, sdk.device.Counter])`.
After checking the binding status, `client.readMany(binding)` returns an inferred
tuple with independent item statuses and owned values. Only explicitly marked
scalar/open-enum Fields are eligible. RPC refuses this operation before transfer.
See [JS/TS ReadMany](../../../doc/JsReadMany.md) for a checked example and the
[native interoperability tests](../../../tests/client_read_many_web/README.md).

## Optional Encoded Mixed Batch

Enable `{batch: true}` on the Encoded backend and discover its matching TBM1.
`client.batch(tbm1, Batch.items(Batch.write(field, draft), Batch.read(field), ...))`
submits one ordered request through the same FIFO. Check each item outcome and
`valueStatus` independently; outer success does not imply usable values or that
every endpoint succeeded. See [JS/TS Mixed Batch](../../../doc/JsBatch.md) and its
[native qualification](../../../tests/client_batch_web/README.md).

## Optional Encoded retries

`EncodedBackend` performs one TEQ1 request/reply exchange. Choose the separate
`RetryingEncodedBackend` when repeated endpoint execution is acceptable:

```ts
const backend = new RetryingEncodedBackend(sendCompletePacket, {
    replyTimeoutMs: 500,
    maxRetries: 3,
});
const client = new DeviceClient({backend, matchesHeader: sdk.matchesHeader, timeoutMs: 30000});
// Feed complete verified transport frames to backend.receive(frame).
```

Import both classes from the generated `client.js` module. The same generated
read/write/command/service bindings and queue apply. Three retries allow at most
four sends, with identical request bytes, session and request ID. Zero permits
one send with a reply timeout. Options are copied at construction; reply timeout
is an integer in `1..0x7fffffff`, retries in `0..0xffffffff`.

The snippet explicitly selects an example 500 ms reply / 30000 ms overall
profile. Omitted options still use 100 ms reply / three retries and 5000 ms
overall; C++ operation defaults are 30000 ms overall. For equivalent platform
behavior, set the overall budget explicitly. Calculate a UART reply budget from
full framed TX and maximum RX time plus bounded driver/server work and jitter:
the [shared budget contract](../../../doc/AsyncClient.md#uart-reply-budget-and-overall-deadline)
includes a 2080-byte, 115200-baud example. The reply timer is not restarted at
TX release, so an undersized budget can retry a loss-free exchange.

The timer starts at each handoff and requests that attempt's cancellation through
its own AbortSignal. A retry waits for the previous send Promise to settle;
settlement must mean that physical I/O has released its bytes. A void send means
release on return. The overall enqueue deadline remains authoritative, including
when a transport ignores abort and never releases. Logical cancellation or
timeout does not make that transport available for another request.

`RetryingEncodedBackend.fromExchange(exchange, options)` accepts complete-frame
exchanges with the same release contract. An exchange waiting for a lost reply
should honor its attempt's AbortSignal and settle after actual cleanup. A late
correlated reply can still establish the outcome while the invocation is active;
a known result retains any outstanding physical release fence.

Any checked response stops retries, including endpoint refusal or Command
Accepted. Matching malformed replies remain terminal. UART framing and integrity
checks belong to the port, before `receive()`. There is no new ACK packet or
server deduplication: a lost response can cause another endpoint execution.
Exhaustion preserves `Timeout` for reads and `OutcomeUnknown` for possible
mutations, with the existing recovery gate. Opening a fresh session never replays
the old operation. Use `RpcBackend` for its existing retained transaction model.
