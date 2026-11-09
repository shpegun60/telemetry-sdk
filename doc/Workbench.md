# Telemetry Workbench

Author: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

The Workbench supports **Read → Edit → FieldWrite → Refresh**, explicit
**Select Command → Edit arguments → Execute**, and
**Select Service → Edit Request → Call → Inspect Response** through one runtime client.
Batch operations, new wire formats and physical programming remain separate work.

## Ownership and compatibility

One owner loop controls one DynamicClient and its existing FIFO per session.
The shared snapshot component stores a copied canonical value, identity and
revision, and provides checked typed navigation. It owns no transport, client,
queue, thread or dynamically allocated storage. Inspector and Workbench reuse
that implementation; the existing read-only InspectorModel API remains valid.

The editor uses separate fixed or caller-owned staging storage. Schema storage,
workspace control objects, callbacks and transport buffers retain their existing
lifetime contracts. Close/drain completes logical requests; it does not itself
prove physical TX release or remote session closure.

Every navigation and editing operation checks owner, admission, revision and
schema generation before metadata or payload access. A new schema/session,
selection, accepted read or invalidation retires the appropriate handles.
Handles must not outlive their owner.

## Independent borrowed views

`WorkbenchView` provides independent selection, Field snapshot, argument editor
and Service response over an existing `WorkbenchModel`. It constructs no client,
backend, queue or transport. Both types reuse the same inspection, editing and
completion implementation; the existing four-parameter owning APIs remain valid.

```cpp
using Owner = telemetry::integration::workbench::WorkbenchModel<8, 4096, 65536, 768>;
using View = telemetry::integration::workbench::WorkbenchView<8, 4096, 65536, 768>;

Owner owner{backend, clock, ownerResponse};
// Admit owner using the existing schema/header/session API first.
View left{owner, leftResponse};
View right{owner, rightResponse};
if (left.synchronize() != telemetry::client::Status::Ok ||
    right.synchronize() != telemetry::client::Status::Ok)
    return;
```

Each view uses the existing bind/read/refresh/navigation/edit/apply/Command/Service
methods. Only `owner` runs process and the transport loop. Requests from all views
use its one FIFO and shared capacity. Refused submissions do not consume an entry
or discard a valid draft. Caller-provided response buffers are exclusive, disjoint
from each other, the owner, views and schema storage, and outlive pending callbacks.

`synchronize()` admits local stores to the owner's current admission without
admitting the shared client again. Owner revocation or replacement immediately
makes old views, handles and editors stale. After an owner has established a new
admission and this view's callbacks have drained, synchronize prepares empty local
stores; it does not transfer old selections or issue reads.

`invalidateData()` retires only this view's cached values and editing leases. A
pending Read or Service cannot publish into its old revision. A pending Apply
retains its Write outcome but does not start an obsolete readback. A host with
multiple views should invoke it on siblings after an accepted managed mutation;
direct native operations require the same explicit freshness coordination.
Admission and data freshness are separate: this method does not revoke the client.

`retire()` irreversibly stops this view from admitting new operations and hides its
values. Previously accepted operations remain owned by the shared FIFO: removal of
a panel neither cancels another view nor changes a possible mutation into failure.
Callbacks still report the original operation facts, including Accepted, Executed,
confirmed Write or OutcomeUnknown. Retired views never add an automatic replay or
readback. The view deliberately exposes no admit, close, invalidate, process,
packet/handoff/reply, cancellation or quiescence API for the shared client.

Keep the owner and its backing schema alive until every view is destroyed. Before
destroying a view, retire it and wait for `drained()`: local submissions, callbacks
and their captured destructors must have returned. Its local drain says nothing
about physical TX release, the owner's other requests or remote-session recovery.
In particular, do not destroy a view from inside its own completion callback.
Qt adapters can retain a view and its owner through an explicit lifetime lease.

The view's memory budgets are explicit: it has its own `SnapshotBytes` snapshot,
two `PayloadBytes` editor/request arrays and fixed metadata. Service response
storage is additional. A MinGW x86-64 comparison against `8851044`, using identical
`8,4096,65536,768` template parameters, records unchanged owning sizes (Inspector
76,960 B, Workbench 86,608 B) and a 75,392 B borrowed view. These are object RAM
sizes on that ABI, not total firmware RAM, linked Flash or complete stack usage.

## Editing and submission

Editing starts from a successfully read canonical value of a writable Field.
The editor binds that Field, root type, schema/session identity and snapshot
revision. It uses DynamicValueWriter internally; GUI users never build payloads
or request identities. There is no native DTO generation at runtime.

Invalid text, overflow, wrong scalar type or stale identity leaves local value
bytes unchanged. U64/S64 text never passes through double or QML Number. Editing
one leaf preserves all other bytes, including NaN payloads and signed zero.
Structs and arrays are edited through their checked scalar leaves; enums use
their exact underlying type, including unknown open-enum values.

An initially refused Write submission leaves the draft and published snapshot
intact. A committed payload is retained independently of a consumed Writer so
QueueFull does not silently discard the user's edits. Accepted writes use the
same FIFO and admission/handoff guards as reads. There is no automatic mutation
retry, extra scheduler or second client.

Both Write and Refresh timeout budgets are checked before submission. Invalid
budgets leave the draft intact and never start a remote mutation. A full FIFO
can still refuse the later Refresh after a confirmed Write; the result records
that separately. In particular, a one-slot client cannot enqueue a readback
from a Write callback while that slot is still completing; choose at least two
slots for a self-contained Apply/Refresh workflow.

## Portable API

Include `<integration/workbench/WorkbenchModel.hpp>` and link the optional
`Telemetry::workbench` CMake component, or include
`lib/integration/workbench/workbench.pri` in qmake. Backend and owner clock are
the same application objects used by DynamicClient. Do not create a separate
Inspector alongside Workbench for the same session: Workbench already exposes
the Inspector read/navigation interface and privately owns it.

```cpp
namespace wb = telemetry::integration::workbench;
namespace rt = telemetry::runtime;
namespace cl = telemetry::client;

wb::WorkbenchModel<> workbench{backend, clock};
// Admit schema/header/session, select a Field, then refresh through the usual
// owner process/transport loop. After that read has published a snapshot:
if (workbench.beginEdit() != rt::Status::Ok)
    return;

// gainNode is a checked child handle obtained from root()/child(). Use the
// exact scalar type from info(gainNode); no implicit numeric conversion.
if (workbench.setScalar(gainNode, 12.0f) != rt::Status::Ok)
    return;

const auto submitted = workbench.apply([](const wb::ApplyResult& result) noexcept {
    if (!result.writeCompleted || !result.write.succeeded())
        return; // Inspect result.write: refusal, timeout or OutcomeUnknown.
    if (!result.refreshCompleted || !result.refresh.published)
        return; // Write succeeded; refreshSubmission/refresh explain readback.
    // The owned snapshot now contains the actual device value.
});
if (!submitted) {
    // The draft remains editable; handle submitted.status before retrying.
}
```

The caller continues the same owner loop after submission. No synchronous
wait or hidden thread is introduced. Completion must be noexcept and fit the
configured inline callback storage. It runs once for an accepted Apply.
`drained()` remains false while that completion is running. Handles and
callbacks must not outlive the Workbench; destruction requires logical drain
and actual transport-buffer release.

`beginEdit()` validates/copies the published value into fixed staging storage.
`setScalar()` uses a checked prepared handle and changes only one leaf.
`editScalar()` previews that draft, while ordinary `scalar()` continues to read
the published snapshot. `dirty()` compares draft bytes on demand; `canApply()`
requires an actual change. `cancelEdit()` discards only the local editor.
Accepted Apply retires the published snapshot, submits one Write and retains
the committed payload until its completion. It publishes no local draft as a
device value.

`PayloadBytes` bounds both request capacity and each of the two edit buffers;
`SnapshotBytes` independently bounds the read snapshot. Only Workbench pays for
the draft, committed request, Writer and Apply completion storage. There are
no dynamic allocations, virtual interfaces or additional FIFO in these classes.
The [Qt example](https://github.com/shpegun60/telemetry-ux/blob/main/examples/inspector/README.md#read-edit-write-and-refresh)
provides the complete runnable GUI workflow.

## Apply results

Apply submits one Write. Only a confirmed successful Write schedules one normal
Refresh through the same FIFO. The two outcomes remain independent:

| Write | Refresh | Reported state |
| --- | --- | --- |
| Submission refused | Not submitted | Draft preserved; explicit submission error |
| Endpoint refused | Not submitted | Write refused; no success indication |
| Confirmed successful | Successful and current | Publish the actual device value |
| Confirmed successful | Refused, failed or stale | Write confirmed; readback unavailable |
| OutcomeUnknown | Not automatically retried | Value may have changed; recovery required |

A Refresh failure, including QueueFull, never rewrites a confirmed Write as a
failed Write. A stale schema prevents value publication without erasing the
known operation result. Local edited bytes are never published as confirmed
device data. Read-modify-write does not provide compare-and-write or protection
against concurrent application changes.

The Qt controller consumes the terminal Apply result before retiring either a
Demo or TCP session. Explicit Disconnect preserves Cancelled before handoff,
OutcomeUnknown after uncertain handoff, and a known successful Write when its
Refresh is interrupted. During explicit disconnect or live-session replacement,
the accepted Apply emits one terminal signal outside the core, before a
reentrant state-change handler can replace its session.
Disconnect clears cached values; a readback already completed remains a known
historical result. No packet is handed off during retirement.

## Commands and shared argument editing

`bindCommand(id)` obtains a runtime binding from the same private DynamicClient.
`selectCommand(binding)` prepares an argument form without submitting anything.
The shared RequestEditor uses the existing draft and committed-request buffers,
SnapshotStore navigation and DynamicValueWriter; it owns no queue or transport.
Field snapshot methods keep their existing meaning. `editorRoot()`,
`editorInfo()`, `editorChild()` and `editorSnapshotInfo()` describe the argument
form instead of presenting those bytes as a value read from the device.

Initialization traverses validated runtime types and bounds before writing the
request. It encodes false, integer zero and positive floating-point zero in
canonical little-endian form, including nested structs and arrays, and validates
the resulting value. Open enums start with underlying zero, which may be unnamed.
These are initial arguments, not device defaults: Descriptor v3 contains no
application default values or parameter ranges. A Void request has no value or
Writer; an empty Struct is a real zero-byte value with a root and no children.

```cpp
const auto command = workbench.bindCommand(commandId);
if (!command || workbench.selectCommand(command.endpoint) != cl::Status::Ok)
    return;

// Navigate editorRoot()/editorChild() and use the same exact-type setScalar()
// as Field editing. A Void request has no argument root to edit.
const auto submitted = workbench.execute([](const auto& result) noexcept {
    if (result.executed()) {
        // Endpoint confirmed execution.
    } else if (result.accepted()) {
        // Endpoint accepted work; later execution is not confirmed here.
    } else {
        // Inspect status/dispatch/endpointStatus, including OutcomeUnknown.
    }
});
if (!submitted)
    return; // Arguments remain available after an initial queue refusal.
```

Execute accepts unchanged initial arguments and zero-byte requests; it does not
require `dirty()`. One accepted Execute submits one logical Command through the
existing FIFO. Setters are blocked during execution; another Execute is also
blocked until its callback returns. A normal completion callback may edit retained
arguments while their schema/session remain current, or submit an explicit FieldRead
subject to the ordinary queue/admission checks. Changing selection retires the old
argument form. Known terminal completion can leave the same arguments
available for another explicit user action. Closing, stale schema/session or
required recovery prevents further execution.
Accepted Execute retires the old Field snapshot, since a Command may change
device state, and supersedes an older pending Field read. A refused submission
preserves that snapshot. No replacement Field value is invented locally.

Moving or destroying a user callable during submission cannot replace its
selected endpoint or argument bytes. A small owner-only publication guard
refuses nested editing/submission and defers owner-loop progression during
callable ownership changes. Close and invalidation can still revoke admission.
Normal completion invocation retains the documented read/edit reentrancy;
callable construction/destruction is a separate phase.

Commands do not run the Field Apply/Refresh chain. Accepted is a terminal request
result, not a promise that an application job has finished. OutcomeUnknown never
causes Workbench to re-enqueue the command. A selected backend retains its own
documented in-flight packet retry semantics; Workbench adds no exactly-once
guarantee. A fresh TCP session proves the supported protocol cleanup, not
completion of a previously accepted application job.

## Services and owned responses

`bindService(id)` and `selectService(binding)` prepare a canonical Request with
the same editor as Fields and Commands. Selection does not perform I/O.
`callService(callback, timeout)` submits one logical operation to the same FIFO.
Service responses are copied from the borrowed DynamicClient result before the
user callback runs; no borrowed DynamicValueView escapes that callback.

Response capacity is explicit and caller-owned:

```cpp
std::array<std::byte, 4096> responseStorage;
wb::WorkbenchModel<> workbench{backend, clock, responseStorage};
// Admit the existing RuntimeSchema/session, then select a Service.
const auto bound = workbench.bindService(serviceId);
if (!bound || workbench.selectService(bound.endpoint) != cl::Status::Ok)
    return;

const auto submitted = workbench.callService(
    [&](const wb::ServiceResult& result) noexcept {
        if (!result.operation.succeeded())
            return;
        if (result.valueStatus == telemetry::integration::dynamic::ValueStatus::NoValue)
            return; // Successful Void response, with no value to inspect.
        if (!result.published)
            return; // Inspect valueStatus/modelStatus independently of the operation.
        const auto root = workbench.serviceRoot();
        if (!root)
            return;
        // Navigate serviceInfo/serviceChild/serviceScalar with checked handles.
    });
if (!submitted)
    return;
```

The two-argument constructor remains valid for existing users. It reserves no
Service response payload; a nonempty response needs an explicitly supplied span.
Request and response capacity and illegal memory overlap are checked before
enqueue. Response storage must remain live and unmodified through the owned
snapshot lifetime and shutdown/drain. It must not alias the Workbench object,
schema storage, request buffers, Field snapshot or transport buffers.

`ServiceResult` separates the copied operation outcome, runtime `valueStatus`,
snapshot `modelStatus`, response revision and `published` fact. A Service endpoint
status of 1 means InvalidArgument, never Command Accepted. A successful Void
response has NoValue and no snapshot; an empty Struct is a real zero-byte snapshot.

`serviceSnapshotInfo`, `serviceRoot`, `serviceInfo`, `serviceChild`,
`serviceChildCount` and `serviceScalar` inspect the owned response through the
existing SnapshotStore implementation. A mere endpoint selection or Field read
does not replace it. A new accepted mutation retires its revision, while a refused
submission preserves it. New schema/session, close and invalidation make its
handles stale before metadata or bytes are read. Handles cannot outlive Workbench.

Services may mutate the device. An accepted call retires an old Field snapshot,
has no automatic Field Refresh, and is never re-enqueued by Workbench after
OutcomeUnknown. A valid operation outcome survives schema invalidation even when
the response can no longer be published. Recovery retains the existing transport
and remote-session requirements; a local reset alone is not a replay guarantee.

## Application and transport scope

The Qt application owns input parsing, labels, QML models and connection policy.
It supplies a writable Host Model with a normalizing setter for the first
end-to-end workflow. Existing read-only Demo and Inspector scenarios remain
available. Remote writes require both Descriptor permission and explicit
application opt-in.

The existing NUCLEO-H7S3L8 firmware and serial profile remain read-only in this
stage. Their validated read-only reconnect proof must not be generalized to
uncertain mutations. The portable Workbench remains backend-independent; a
writable UART profile needs its own controlled peer and qualification.

The current 2048-byte serial body budget and wire formats remain unchanged.
Future writable profiles must budget the actual request and response airtime,
backend deadline and processing allowance before submission.

## Acceptance evidence

Maintained host tests cover the real Read/Edit/Write/Refresh path, device-side
normalization, structs/arrays/enums, U64/S64 limits, unchanged floating-point
bits, read-only permission, invalid input, capacity refusal, stale identities,
callback reentrancy and destruction. Timeout tests use a caller clock and explicit
queued/offered/prepared states; an elapsed wall-clock sleep is not a handoff proof.

Write success followed by Refresh failure is tested separately from Write
failure. Invalidated edits and uncertain writes cannot cause an automatic new
handoff. No-heap probes and existing Inspector compatibility suites remain gates.

Qt tests exercise actual QML edit and Apply handlers plus readback, not only C++
helper calls. GCC, Clang ASan/UBSan, installed-module and offline ARM checks retain
raw reports outside the source tree. RAM/Flash/stack claims identify their exact
toolchain and measured inputs. Physical hardware qualification is not implied by
host or CI results.

## Measured storage and compiler probes

For the Command stage before Service support (PR #11, merged as `855e9f7`),
x86-64 GCC 13.3 with CallbackBytes=256 recorded a fixed
240-byte Workbench increase over FieldWrite baseline `b06791f`. It reused
the same two PayloadBytes buffers and one completion slot:

| Configuration (N, PayloadBytes, SnapshotBytes) | FieldWrite baseline | FieldWrite + Command | Difference |
| --- | ---: | ---: | ---: |
| 2, 64, 64 | 1968 B | 2208 B | +240 B |
| 8, 4096, 65536 (Qt example) | 81648 B | 81888 B | +240 B |

Inspector remains 1248 B and 72864 B in those two configurations, respectively;
NodeHandle remains 48 B. These are complete model object sizes excluding schema
indexes, backend, transport buffers and Qt objects. They are ABI-specific
measurements, not portable size promises.

Using ARM GCC 13.2.1, Cortex-M7/Thumb, C++20, -Os, no exceptions/RTTI and heap
fallback disabled, the frozen Field-only Workbench probe from `b06791f` records
7716 B object text with baseline headers and 7986 B with the new shared editor
and publication guards (+270 B). Its largest individually recorded compiler
frame stays 656 B. This compares identical probe code and compiler options;
it is not the total cost of Command execution.

The maintained Standalone probe separately compiles the new Command selection
and Execute path for Cortex-M7. No-heap runs execute both FieldWrite and Command
workflows, with deliberate allocation controls. Unlinked objects and .su files
establish neither linked application Flash nor complete call-chain stack,
hardware latency or CPU throughput. Raw commands, hashes and compiler outputs
remain in external qualification receipts.

The Service extension is measured separately against `855e9f7` with the same
probe inputs, compiler and options:

| Configuration (N, PayloadBytes, SnapshotBytes) | Command baseline | With Service | Difference |
| --- | ---: | ---: | ---: |
| 2, 64, 64 | 2208 B | 2320 B | +112 B |
| 8, 4096, 65536 (Qt example) | 81888 B | 82000 B | +112 B |

The extra 112 B contains fixed response bookkeeping, not a payload array. The
caller-provided response buffer is additional and explicitly sized; the Qt State
chooses 65536 B, while embedded callers choose their own capacity. Inspector and
NodeHandle sizes stay unchanged. The identical Field-only ARM probe records
GNU size text 7986 → 8032 B (+46 B) and the same 656 B largest individual frame.
These measurements do not estimate the full linked Service path or hardware CPU
time; the maintained ARM standalone probe also compiles the new Service API.
