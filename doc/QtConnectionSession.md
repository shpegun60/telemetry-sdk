# Owned Qt connection

`Telemetry::qt_connection` is an optional host component. It requires Qt 6.4
Core/Network and uses one existing `WorkbenchModel`/`DynamicClient` FIFO. Portable
core/default SDK builds do not discover or link Qt. `qt_tcp_transport` separately
exports the carrier; `tcp_wire` exports stream framing without server/Model code.
The carrier, `TcpTransport`, is a `LinkTransport` over a `TcpLink` and the TCP
profile's `ProfileFramer`: the exchange rules live in `LinkTransport` and serve
any medium that supplies a `Link` and a `Framer`.

The device's files are read on the same queue: `filesSupported()` says whether
the connection can, and `submitResource()` makes the session a files client, so
`files::list`, `files::stat`, `files::read` and `files::FileReader` take it
directly. A `FileReader` submits its next piece from `poll()`, called from the
owner's loop, never from a completion: the Workbench refuses submissions while
one of its completions runs.

### Files only

A device whose file list is read correctly but which offers no usable descriptor
still connects, for its files. `filesOnly()` is then true, `ready()` false, and
every telemetry request is refused; `filesSupported()` is true and the file API
works as above, on a queue of its own that is admitted without a descriptor and
carries only Resource requests. `limitation()` says why:

| Limitation | When |
|---|---|
| `NoDescriptor` | The list has no `/telemetry/descriptor.bin`. |
| `DescriptorUnreadable` | The device refused to stat or read it. |
| `DescriptorSize` | Its size is outside what a client accepts; `refusedDescriptorSize()` has it. |
| `DescriptorRejected` | It was read whole, but the descriptor parser or admission refused it. |
| `Capability` | A ReadMany, Batch or session file is missing, malformed or does not match. |

`lastError()` holds an English description; a program words `limitation()` itself.
`discoveredPaths()` keeps every path the device listed, until the next connection,
so a program can name a likely typo in the descriptor's name. A device whose
replies do not follow the protocol is still a plain failure: no files, no
limitation. A files-only connection never asks for same-peer recovery; it
executed nothing, so a new connection is all a later one needs.

```cpp
#include <integration/qt/ConnectionSession.hpp>
telemetry::integration::qt::ConnectionSession device;
device.connectTcpReference("127.0.0.1", 5577);
// readiness and errors arrive through stateChanged; the Qt owner loop progresses
// native work automatically. No additional application pump is needed.
```

For ready-made views, install Telemetry UX's `qt_connection` component, create
`telemetry::ux::QtConnectionBinding{&device}` and call `createProvider()` for each
panel. See UX `examples/qt_connection` for two actual QML panels. Each panel owns
selection, draft and snapshots; execution/transport/session remain shared.

## Named profile and lifecycle

`connectTcpReference` explicitly selects the maintained exclusive single-client
Resource-bootstrap + Encoded TCP reference server contract. It does not infer a
protocol, authentication, authorization or remote cleanup guarantee from a host
name. Use this profile only with a matching trusted server. TLS/access control is
an application responsibility; TCP socket closure proves local release only.

The Session owns the Qt TCP carrier, Descriptor bytes/indexes, one Workbench,
request/reply storage and owner-loop timer. `ConnectionStorage` factors that owned
state and guarded handoff/deadline-ordered reply processing for existing hosts;
its advanced factory does not create a second execution owner. Inspector uses
this same implementation instead of a private TCP driver.

The owner-loop timer ticks every 5 ms for deadlines and for whatever waits. A
reply that arrives, and a file request the session accepts, also queue one
`process()` on the loop's next turn, coalesced: the reply is settled and the next
request sent without waiting for a tick. Before this, each file request waited
for two ticks - one to settle its reply, one to send the next - which made a
listing of 4096 files take over a minute on Windows, where a 5 ms timer fires
about every 15.6 ms.

All calls and storage access belong to the creating Qt thread. `workbench()` is
an advanced borrowing seam, available only while ready. Consumer UI code normally
uses a provider. Borrowed pointers/views cannot outlive their admission/storage.
`storage()` can retain a closed generation, not make stale metadata usable.

- `disconnect()` revokes transmission and closes/drains local work. It can reconnect.
- `close()` does the same and permanently prevents new admission.
- No operation is replayed automatically, including after `OutcomeUnknown`.
- Before handoff, cancelled queued/offered mutations remain `Cancelled`.
- After authorized handoff without a known result, mutation facts retain
  `OutcomeUnknown`; session invalidation does not erase possible execution.
- Complete correlated replies owned before EOF are processed before retirement,
  with the original absolute deadline. Receipt after deadline cannot become `Ok`.
- Same-host/port reconnect after an uncertain operation must also match the
  numerical peer address and finish a new complete validated Resource transcript.
  Under this named exclusive server profile that transcript proves old native
  work was superseded. A different peer cannot discharge the old obligation.
- Session numbers never wrap or repeat in one owner; exhaustion requires a new
  application lifecycle. There is no automatic migration between unrelated peers.

`aboutToRetire` detaches presentation before native close; `progressed` publishes
owned completion slots only after the native callback stack returns. UI adapters
survive panel removal and retain accepted operations until drain. Disconnect or
removal from ordinary Qt publication callbacks is supported, including model
reset. Custom native callbacks must use `deleteLater()` rather than destroying
the connection in the native stack. A storage factory must return fresh unparsed
storage and must not start or admit a second client.

The initial convenience profile is TCP only. The separate existing SerialPort
carrier remains available; this API does not claim that a read-only UART bootstrap
can prove recovery of an earlier mutation. Arbitrary profiles, physical DMA,
concurrent native producers, configurable embedded storage and remote exactly-once
execution are outside this host convenience component.

## Optional scalar ReadMany

The same connection can discover the optional ReadMany profile. A server with
neither `/telemetry/session.bin` nor `/telemetry/read-many.bin` remains usable
for ordinary endpoint operations; `readManySupported()` is false. A partial
profile, malformed capability, wrong session or wrong Descriptor fingerprint
fails admission. It is never silently replaced with Individual reads.

```cpp
const std::array<std::uint32_t, 3> ids{voltageId, currentId, powerId};
const auto submitted = device.readMany(ids,
    [](const telemetry::integration::read_many::ReadManyResult& result) noexcept {
        // Check result.operation and each item's status. Copy/format any value
        // here: the DynamicValueViews are borrowed only during this callback.
    });
if (!submitted) {
    // QueueFull, Unsupported, NotAdmitted, RecoveryRequired, etc. are immediate.
}
```

`readManyCapability()` returns an optional copied capability for the current
admission; reacquire it after reconnect. One request accepts at most its item
limit (at most 32) distinct scalar/enum Fields of 1, 2, 4 or 8 wire bytes. Larger
watchlists belong to a host scheduler that submits sequential chunks with one
pending chunk, skips excess timer ticks and treats Stop as drain, not replay.
Chunks are independent observations, not an atomic application snapshot.

`readFieldValue(id, callback, timeoutMs)` explicitly selects an Individual read
through the same FIFO. Both methods preserve the selected Field snapshot and
editor; neither selects a Field or creates another client. `busy()` reports
pending native work. Guards return Closed before RecoveryRequired before
NotAdmitted without calling `process()`. Accepted callbacks retain their
generation's owned storage through disconnect/drain. Native callbacks must not
publish Qt model changes directly; consume copied results after `progressed`.

For an owned native server, the optional helper removes bootstrap boilerplate:

```cpp
#include <integration/tcp/Server.hpp>
telemetry::integration::tcp::ReadManyProfile profile{descriptorBytes, fingerprint};
telemetry::integration::tcp::Server server{model, workspace, profile, rx, tx};
```

Create one noncopyable profile per accepted physical connection, before its
Server, and destroy it after that Server and transport leases retire. It borrows
the stable Descriptor and owns the Resource provider table and canonical TRM1.
The named optional handshake uses existing Resource operations:

1. LIST discovers Descriptor plus both optional files; Descriptor is read first.
2. STAT verifies a four-byte writable session control file. WRITE starts at
   cursor zero with exactly four little-endian bytes, a nonzero host-selected
   session and `final=true`. Its complete ACK must consume four bytes and return
   cursor four. This changes only connection admission; it invokes no Model
   setter, Command, Service or getter.
3. STAT/READ fetch the 24-byte TRM1 from cursor zero. Its session must echo the
   requested monotonic Session ID, and its fingerprint must match Descriptor.
4. The first Encoded request freezes the bound profile. All later attempts to
   bind again are rejected, including the same ID. STAT and reads cannot rebind.

Resource cursors keep their ordinary meaning; no nonzero initial READ cursor is
used as a session token. Plain `TcpTransport::connectTo(host, port)` keeps its
Descriptor-only behavior; `connectToWithReadMany(host, port, requestedSession)`
selects this optional discovery. `ConnectionSession` supplies the session itself.
The existing `Server` constructor taking a file table remains legacy operations
1–4. TRM1 does not authorize Mixed Batch; TBM1 is admitted independently as below.

## Optional Mixed Batch

`batchSupported()` and `batchCapability()` describe a separately discovered
`/telemetry/batch.bin` TBM1. The same Resource session WRITE binds TRM1 and TBM1;
each present capability must be canonical and match the requested session and
Descriptor fingerprint. A ReadMany-only or legacy peer remains usable and
returns `Unsupported` for Batch, without fan-out or probing any endpoint.

```cpp
using Session = telemetry::integration::qt::ConnectionSession;
namespace cl = telemetry::client;
const std::array<Session::BatchRequest, 3> requests{{
    {cl::Operation::FieldWrite, configId, canonicalConfig},
    {cl::Operation::Command, setModeId, canonicalArguments},
    {cl::Operation::FieldRead, powerId, {}}
}};
const auto submitted = device.batch(requests,
    [](const telemetry::integration::batch::BatchResult& result) noexcept {
        // Check outer operation, then each independent item operation/valueStatus.
        // Copy payloads while here; DynamicValueViews end with this callback.
    });
```

There are 1–32 ordered items; repeated endpoint IDs are allowed. FieldWrite,
Command and Service payloads use the admitted runtime request type. The existing
Batch client validates all contracts and canonical request DTOs before enqueue,
and the complete response before publishing any runtime value view. Command
`Accepted` remains distinct from `Executed`. Endpoint failures are independent;
this operation provides neither rollback nor an atomic application snapshot.

Request spans are borrowed until `batch()` returns. A bounded 4096-byte host
scratch is copied into the same FIFO before return; a larger encoded body is
rejected. Replies remain within the capability, 65535-byte wire limit and owned
transport buffers. `HostWorkbench` explicitly selects 1536 callback bytes for
the copied 32-item binding and small host callable; portable defaults are intact.

Use a matching SDK/UX pair for this optional Qt integration. Native adapters
that explicitly instantiate `WorkbenchModel`/`WorkbenchView` must use this host
callback budget too; older UX adapters hard-code 768 bytes and are not compatible
with the 1536-byte host type. Applications using `ConnectionSession` and the
matching `QtConnectionBinding::createProvider()` do not manage this budget.

A rejected submission preserves editor, snapshot and pending state. An accepted
mutating Batch retires the owner's old snapshots/editor under the same native
publication guard. `dataInvalidated()` then lets sibling presentation views
retire their caches; it does not revoke schema/session admission. UI hosts apply
their shared mutation permission before submission. The SDK's explicit call is
authorization; `ConnectionStorage::mutationAuthorized` is transport handoff
evidence used for recovery, not a permission flag.

Callbacks run on the owner thread, storing owned results for `progressed()`.
Disconnect/close preserves native outcomes, including `OutcomeUnknown` after an
uncertain mutating handoff. Reconnect never replays a Batch. Closed precedes
RecoveryRequired, which precedes NotAdmitted; gates do not call `process()`.
Callable ownership and `dataInvalidated` reentries defer close/drain until native
submission publication has finished.

`WorkbenchModel::copyDraft(output)` and `WorkbenchView::copyDraft(output)` capture
the current canonical editor bytes without consuming/rearming its writer or
changing dirty state. Exact NaN payloads and signed zero are retained. Void
captures zero bytes; capacity, alias, stale and invalid-payload refusals leave
the destination/editor unchanged.

For the matching native server, explicitly select `tcp_encoded_profile`:

```cpp
#include <integration/tcp/EncodedProfile.hpp>
#include <integration/tcp/Server.hpp>
telemetry::integration::tcp::EncodedProfile profile{descriptorBytes, fingerprint};
telemetry::integration::tcp::Server server{model, workspace, profile, rx, tx};
```

It shares the existing one-time Resource session provider with ReadMany and adds
the TBM1 file and Batch dispatch route. The old `ReadManyProfile` constructor
keeps its ReadMany-only behavior and no Batch dispatcher link root. See the
[installed Qt Batch tests](../tests/qt_batch/README.md). RPC and UART bootstrap
are not extended by this optional TCP profile.

## Validation boundary

Installed downstream tests use two QML panels, real loopback TCP, queued/offered/
prepared/executed cancellation, sibling freshness, removed panels, owned outcomes
and explicit fresh admission without replay. The maintained Inspector TCP suite
runs against the extracted owner too. These are host checks, not MCU/DMA tests.
