# Asynchronous generated clients

The optional client module provides `read`, `write`, `command`, and `service`
over the existing generated DTOs and standalone codecs. STM32 and Qt use the
same C++ `DeviceClient`; JS/TS use one TypeScript runtime with matching operation,
status, queue, deadline, and recovery contracts.

## One owner and one execution queue

One DeviceClient accepts several pending requests but activates one at a time.
The RPC backend exclusively owns one native RpcClient and one remote mailbox
session. Independent remote clients still use separate `RpcSessionPool<N>`
bindings; there is no server-global single-mailbox restriction.

Generate a C++ SDK, include `device_api/ClientBindings.hpp`, provide a backend,
and use owning `noexcept` callbacks:

```cpp
device_api::client::DeviceClient<8, 4096> client{backend, clock};
auto submitted = client.read(device_api::device::fields::Config,
    [](const auto& result) noexcept {
        if (result.hasValue()) consume(result.value());
    });
if (!submitted) report(submitted.status);
// Owner loop:
client.process(board_millis());
```

Admission takes the actual observed 64-byte Descriptor header and a fresh
nonzero session generation. The application first validates the full Descriptor
and binds it to its peer/model/session; the exact declared-header check is not
authentication or proof of the physical device model. Read-only Fields are
rejected before enqueue. Different endpoint categories cannot be interchanged.

FIFO refers to execution of published requests. Queued cancellation and elapsed
deadlines can complete while another request is active. Each accepted request
gets exactly one owner callback. Refused submissions return immediately and do
not register or invoke a callback. A callback may enqueue/cancel/close, but
reentrant `process()` does not run another callback. The active closure stays
alive until its invocation returns. Destruction of the client from a callback
is outside the lifecycle contract.

## Resource requests for the device's files

`Operation::Resource` puts a raw Resource List, Stat or Read on the same queue as
every other request, so reading the device's files never bypasses it. The payload
is the complete Resource request built with `resource::protocol::client::makeList`,
`makeStat` or `makeRead`; the endpoint is 0; the reply bound is at most
`chunkReplyHeaderSize + maxPayloadSize`. The result's payload is the raw Resource
reply, which the caller parses with `parseList`, `parseStat`, `parseRead` or
`parseWrite` inside its callback, since the view is borrowed.

- The value 7 is client-only. It never appears in an Encoded header, and the Encoded
  operation space keeps it reserved.
- Only `EncodedBackend` constructed with `enableResource` admits it; any other
  backend, and the RPC backend, answers `Unsupported` at submission.
- The packet carries `ReplyFrame::Resource` and the Resource operation byte. List,
  Stat and Read are marked `mayExecute = false`: they change nothing on the device,
  so a cancelled one never counts as possibly executed. A Write is marked
  `mayExecute = true`: once sent, a cancelled or lost one may have changed the
  file. A Resource reply has no identity of its own; the port matches it by
  position, as the one outstanding exchange.
- A port frames such replies only if its transport says so:
  `DeviceTransport::supportsResourceReplies()`. `LinkTransport` does; it switches
  its framer to the Resource reply when the packet is sent - not when it is staged,
  because a staged packet can still be discarded - and back to Encoded right after
  the reply.
- Write (operation 4) is admitted like the others since F-4: what may be written is
  the device's providers' to decide (a file without `write()` answers
  `NotWritable`), and a host that wants a lock - "allow changes" - applies it
  before submitting. `files::FileWriter` writes a whole file in pieces; it never
  resends a WRITE, since the protocol has no deduplication.

## Memory and results

`N` includes Reserved, Queued, Active and Completing slots. `PayloadBytes` is one
shared contiguous request-snapshot pool, not a maximum-sized buffer per request.
Fragmentation can return `PayloadFull` even when some disjoint bytes are free.
Snapshot encoding and owning callback construction occur before publication.
Callbacks are held in stable `DelegateSlot` storage with compile-time fit checks.

The generated policy sizes one aligned response DTO scratch by native
`sizeof/alignof`, independently of canonical wire extents. Native RPC request,
result, packet storage and port TX/RX copies are additional explicit buffers.
`Result::value()` and `ResultView::payload` borrow until callback return. Copy a
result before posting it to a different thread or a queued Qt invocation.

`ResultView::status` describes client/transport/decode completion; `dispatch`,
`endpointStatus`, and `codec` retain their separate meanings. Inspect success
before accessing a value. Command `accepted()` reports application admission;
`executed()` reports the endpoint's Executed status. Device motion, flash
durability and later application completion require application-owned evidence.

## Packet port and time

The port receives and sends complete packets. UART/TCP framing and CAN
fragmentation belong to that port. All owner clock reads and `process(now)` use
one modulo-u32 millisecond domain; intervals are in `1..0x7fffffff`. Deadlines
start at submission, including queue wait. Service the owner at least once in
each half clock range; arbitrary multi-wrap pauses cannot be disambiguated.

Copy `client.nextPacket()` bytes into bounded port-owned storage, then call
`client.prepareHandoff(session, requestId)` immediately before the first possible
external transfer. It rechecks cancellation, session and the overall deadline.
False forbids send. Report `client.onSendAccepted()` only after the transport
accepts the complete packet. Partial writes do not qualify. The port's copy
remains alive and unchanged until the driver releases it, including after a
client timeout, reset or disconnect. TX release and endpoint completion are
independent events. Deliver complete replies through `client.onReply()`.
Report the current packet's acceptance synchronously with submission, exactly
once per successful handoff; do not delay or replay that notification across
attempts. DMA completion is a separate driver event. The two backend correlation
IDs cannot identify an obsolete acceptance callback from an earlier retry.
Every physical send, including an Encoded or same-transaction RPC retry with the same
request ID, requires a fresh `prepareHandoff()` permission.

## Backends and recovery

Encoded request/reply v1 adds an independent 32-byte LE envelope; it changes
neither Descriptor v3 nor RPC v1. Every operation receives a status response.
The server uses [EncodedDispatcher](../lib/integration/encoded/Dispatcher.hpp)
to call existing Model Encoded operations. Session, request, operation, endpoint,
lengths and statuses are checked. The server has no retry or deduplication state;
delivering the same request twice may execute it twice.

Select one optional Integration backend; all use the same DeviceClient queue:

| Backend | Automatic retransmission | Server duplicate handling |
| --- | --- | --- |
| `EncodedBackend` | None | Each delivered request may execute |
| `RetryingEncodedBackend` | Configured TEQ1 request retries | Each delivered retry may execute again |
| `RpcBackend` | Existing Resource packet retry/polling policy | Same transaction is retained/deduplicated within its live mailbox session |

```cpp
#include <integration/encoded/RetryingBackend.hpp>
telemetry::integration::encoded::RetryingEncodedBackend backend{
    txStorage, resultStorage, clock,
    {.replyTimeoutMs = 100, .maxRetries = 3}};
device_api::client::DeviceClient<8, 4096> client{backend, clock};
```

The retry backend composes one existing Encoded invocation and retains its exact
bytes/session/request ID. It adds no queue, DTO serialization, ACK packet or
server state. `maxRetries` counts additional sends: three permits the initial
attempt plus at most three retries; zero disables retries. Each reply timer starts
after a complete-packet `onSendAccepted`, and a retry still needs fresh handoff
permission and physical TX release. The enqueue deadline, including queue wait,
continues across all attempts. A direct backend caller supplies its bounded
overall budget through `RequestView::timeoutMs`.

## UART reply budget and overall deadline

`replyTimeoutMs` includes physical transmission time. C++ starts it at complete
packet acceptance by the driver (`onSendAccepted`); JS/TS starts it at handoff.
Neither event proves that the UART has finished sending. TX release only makes
the port available; it does not restart the timer. A retry already offered at
release can therefore begin before the first response arrives, even with no loss.

For each operation, bound the complete framed request and maximum framed reply:

```text
TX_ms = ceil(txWireBytes * bitsPerCharacter * 1000 / baud)
RX_ms = ceil(maxRxWireBytes * bitsPerCharacter * 1000 / baud)
replyTimeoutMs = TX_ms + RX_ms + driverAndServerBudgetMs
                 + deliveryAndOwnerJitterMs + positiveMarginMs
```

For 8N1, `bitsPerCharacter` is 10. Count the 32-byte TEQ1 header in each direction
and the port's worst-case framing, integrity and escaping overhead. Bound driver
queue/start delay, flow-control pauses, half-duplex turnaround and server work in
the applicable terms. A baud-only calculation cannot bound an indefinitely
paused transport. Round upward, use checked wide arithmetic, reject invalid baud
or budgets outside `1..0x7fffffff`, and retain a positive scheduling margin.
Arrange for the checked reply to arrive before the applicable reply and overall
deadlines; processing a deadline first at the same timestamp can win the race.

A Service with 2048-byte request and response has at least 2080 bytes each way.
At 115200 baud, 8N1, each direction takes about 180.56 ms (181 ms rounded up).
With 7 ms server work and a 20 ms jitter budget, the subtotal is 389 ms before
an additional margin and any outer framing allowance. `replyTimeoutMs=100` is
too short for this profile. The tests deliberately allow the original TX to
finish despite the timeout's abort request: the short budget causes a retry and
can exhaust the request without losing any frame. A suitable budget permits one
send and the first checked response.

The examples select 500 ms for their illustrated retry profile. This is an
explicit application choice, not a universal UART timeout; recalculate it for
your actual wire-size, processing and scheduling bounds. C++ and JS/TS use the
same unchanged retry defaults of 100 ms and three retries when omitted.

The overall enqueue budget is separate and includes FIFO wait and all attempts:

| Entry point | Existing overall default | Explicit example profile |
| --- | --- | --- |
| C++ DeviceClient operation / submitEncoded | 30000 ms | Pass `30000` to the operation |
| JS/TS DeviceClient | 5000 ms | Set constructor `timeoutMs: 30000` |
| Generated QML DeviceApi | 5000 ms | Set QML `requestTimeoutMs: 30000`, or call C++ `setRequestTimeoutMs(30000)` |

Use these explicit values when comparing platforms. QML accepts overall budgets
only through 300000 ms. The overall deadline may truncate retries; multiplying
reply timeout by send count alone does not account for FIFO or physical-release
waits. Changing a timeout does not establish physical DMA or IRQ quiescence.

## Retry results and session recovery

A valid correlated response from an earlier attempt can finish the request while
a retry is offered/prepared. Any checked endpoint result, including refusal or
Command Accepted, stops retransmission; Accepted does not mean Executed. Once a
copy may have reached the peer, cancellation/exhausted retries cannot establish
that a mutation did not execute. No new session automatically replays it. An
already delivered second copy cannot be undone by accepting the first response.
A slow Service may execute repeatedly; a read may return any checked attempt's
value, without a coherent multi-read snapshot guarantee. Select this backend only
where repeated execution is acceptable, rather than assuming exactly-once effects.

TEQ1 itself has no CRC or UART framing. Reject corrupt physical frames in the
port before `onReply`; a malformed complete TEQ1 reply passed to the backend is
terminal under the existing parser/recovery rules, not a new silent retry case.

RPC wraps the moved existing engine, retaining byte-identical same-transaction
retries, polling and ACK gating. No backend automatically replays an operation
across sessions. After possible execution, lost completion of a mutation yields
`OutcomeUnknown`; reads can report Timeout/Cancelled with a separate recovery
gate. In RPC even a read uses final WRITE and can leave a retained server slot.
Unsent queued requests are cancelled when the session is invalidated.

Recovery requires actual closure/drain of the old physical and remote session,
then `confirmSessionClosed(oldGeneration)`, then Descriptor admission for a fresh
generation. Calling reset or admitting the header does not prove remote closure.
Cancelling an active mutation does not undo a possible remote effect.

Owner-only `invalidateAdmission()` revokes logical admission without processing
the backend or invoking callbacks. It blocks submissions, FIFO activation and
every new handoff/retry even while the admission policy remains current. Continue
owner processing to cancel queued work and active work without execution-capable
authorization. After such authorization, backend result/deadline semantics and
passive correlated RX remain. Already checked results and deadline precedence
are preserved. Revocation does not recall an authorized copy, release DMA storage,
close the remote session or clear recovery. New work requires successful
Descriptor admission; a Closed client cannot reopen.

## Multiple producers and shutdown

The supplied `Clock` function and context must outlive the client. Its `read`
function can run concurrently on producer threads and the owner thread, so the
application must provide a thread-safe clock source.

Instantiate `DeviceClient<..., Lock>` with a task mutex policy and wrap it in
`ThreadSafeClient`. Producers reserve a stable record, initialize privately, and
publish under a short lock. Only successfully published requests are accepted;
FIFO follows publication order. No I/O, encoding, closure destruction or user
callback executes while the queue mutex is held. The wrapper wakes the owner
after every submission attempt, successful cancellation, and close. A failed
submission can release the last private reservation after close; its wake lets
an event-driven owner finish draining. A handle identifies owner, session and a
nonreused sequence; handles are valid only during that client instance's life.

Shutdown is explicit:

1. `close()` stops new admission.
2. Let in-progress producer calls return and stop/join those producers.
3. Continue the owner loop. It cancels/completes accepted work and waits for
   private producer reservations to be released without blocking.
4. Wait for `drained()`, then stop/join the owner and close/drain the transport
   and remote session before destroying their storage.

An already stopped owner cannot deliver owner-thread callbacks. The destructor
does not manufacture a drain. The mutex facade is not an ISR interface. A board
port separately owns ISR/main-loop publication, DMA-accessible storage and cache
maintenance.

## Platform adapters and qualification

Generated `DeviceClientAdapter.hpp` keeps QML tokens/pending bookkeeping and
submits to the sole core execution queue. Queue capacity failures are explicit
`QueueFull`/`PayloadFull` results, with no second hidden execution queue. Use one
adapter per DeviceApi. RpcBackend, EncodedBackend and RetryingEncodedBackend are
supported through the same `Backend&`, with no generator or QML value API change.

For JS/TS add `--client-runtime` to the normal generator command; codec modules
remain usable independently. Promise results follow the same statuses, with
`cancel()` on each returned Promise. C++ uses fixed-width integers, JS uses
`bigint`, and existing QML conversion rules keep wide integers as decimal text.
JS cancellation completes the logical request without declaring the transport
idle. The next backend invocation waits for actual I/O release; a checked reply
can complete successfully while its `WireResult.released` fence still retains
the transport. After `close()`, `drained` also waits for this release. See the
[web runtime contract](../lib/client/web/README.md) for explicit
confirmation after an application has closed and drained an old session.

Optional [JS/TS ReadMany](JsReadMany.md) uses that same FIFO with inferred scalar
tuples, explicit Encoded opt-in and remote TRM1 capability. It preserves the
physical recovery contract and never hides an individual-read fallback.

The [bare-metal example](../examples/async_client/README.md),
[C++ qualification](../tests/async_client/README.md),
[DMA transport fixture](../tests/transport_dma/README.md), and
[web qualification](../tests/client_web/README.md) exercise the implementation.
The CI jobs retain generated outputs and logs. Host/Qt/sanitizer runs and ARM
compiler stack evidence do not establish electrical disconnect/DMA behavior on
an untested physical board.
