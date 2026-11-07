# Simpler host integration: scope and acceptance

The goal is a short application path from a Model or Descriptor to device I/O
and an embedded UI. Advanced contracts remain available for custom transports,
explicit buffers and recovery. This is an implementation sequence, not a claim
that every stage below is already present.

## Reviewed baseline

Three independent reviews covered architecture, a programmer consuming the
packages, and lifecycle/resource boundaries. The proposal to coordinate an owned
connection and reuse providers is sound. Several assumptions needed correction:

| Proposal assumption | Verified distinction |
| --- | --- |
| Each Web panel needs a new independent engine | Existing independent providers can share one client; 86 consumer assertions checked separate selection/drafts and one active exchange |
| Static always means Generated SDK | Static C++ can use handwritten known types and Codec; generated types are one optional route |
| A generated Static UI needs only codecs | Structural metadata is optional for typed I/O, required for generic UI; preservation-safe editing also needs wire-editing codecs |
| Static and Dynamic results are interchangeable | Their operation facts agree, but result shapes differ; the Session normalizes interpretation separately from operation facts |
| Closing a socket proves recovery | It proves local closure; remote quiescence/supersession remains a transport/server-specific contract |
| Add a provider around every C++ Workbench | Workbench currently owns Inspector/DynamicClient; sharing needs a borrowing seam, not another owner/FIFO per panel |
| Simple API will cover 80–90% of projects | That is a design target, not a measured coverage result |

The 86-assertion provider experiment used generated modules and a real SDK client
with a controlled backend, not a browser/network or physical-device test.

## Stage 1: SDK connection Session

Implemented in [ConnectionSession](ConnectionSession.md): optional host JS/TS
`connectStatic` and `connectDynamic`, one private existing core/FIFO, normalized
results, request cancellation and explicit lifecycle. `demoWebSocket` selects the
existing reference server's bootstrap/lease contract. Custom hosts can implement
`ConnectionProfile`. No transport, authentication or discovery scheme is inferred
from a URL alone.

Acceptance includes a packed offline-installed downstream consumer, exact emitted
TypeScript artifacts, controlled timeout/cancellation/observer transitions,
pre/post-handoff outcomes, actual local closure and same-peer supersession,
native Encoded operations, and a real Chromium/reference-server journey.
Session ownership is not a second execution engine. Core wire, Codec and FIFO
implementations remain unchanged.

## Stage 2: UX session and independent views

Implemented: Web `mountConfigurator({session})` and independent Session views
reuse one owned Session/FIFO. Installed Dynamic QML providers borrow native
Workbench views without a second client; each panel owns its selection, draft,
response snapshot and diagnostics. View removal leaves accepted work with the
Session owner until drain. Accepted mutations invalidate sibling presentation
without hidden extra reads. Write/readback stays two ordered operations, not an
atomic compare-and-write transaction.

The optional [owned Qt connection](QtConnectionSession.md) now factors the
Inspector's TCP transport/bootstrap/state/progress into SDK `qt_connection`.
Telemetry UX's `QtConnectionBinding` automatically follows its admission and
retirement. Native users no longer need to copy InspectorController or a private
pump to attach two QML panels to one real reference-profile TCP connection.

Web Session optional ReadMany/Batch presentation and complete starter generation
are separate UX changes under active qualification; their release status belongs
to the exact paired SDK/UX revision. Existing backend capabilities and limits
still apply. No new wire operation or second DTO generator is added by these
host conveniences.

Acceptance includes two independent panels on one FIFO, view removal during an
accepted operation, stale callbacks/session replacement, exact U64/S64/open enum,
no automatic mutation replay and installed downstream applications. Native Qt
connection checks additionally cover real TCP EOF and phase-specific disconnect;
hardware qualification is separate.

## Stage 3: starters, semantic metadata and diagnostics

Complete installed application starters and the UX project-creation tool are
implemented/being qualified separately from this SDK connection component. They
select documented profiles and reuse existing views and generator outputs.
Optional labels, units, ranges, initial editor values and help remain a proposed
presentation-metadata layer, separate from the
structural Descriptor and wire compatibility. Unknown metadata must not weaken
Codec validation. Initial editor values are not device defaults.

Acceptance: a programmer starts from the documented installed packages, declares
a small device, generates the optional SDK/UI entry point and performs an actual
read/edit/write/readback without implementing another queue or copying library
internals. Runtime/device work is reported separately from host demonstration.

STM32-hosted HTTP/WebSocket, additional transport profiles and concurrency across
unrelated clients remain separate tasks with their own server/access and recovery
contracts. Existing asset export is not itself a firmware web server.
