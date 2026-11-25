# Telemetry resource providers (C++20)

Authors: Ruslan Kovtun (shpegun60), codexAi. [MIT](../../../../LICENSE).

DescriptorFile and ValuesFile expose the current `telemetry::Model` as an
immutable descriptor and live native values. `v3` names their binary wire
format 3.0; this is the unified Model adapter, including borrowed Field
outputs. Optional RpcMailbox uses its separate RPC wire v1.0 for deferred
encoded endpoint calls. The independent Bind/Exchange control protocol stays
an application example. There is no separate `.pri` here or telemetry
dependency in the generic resource core.

## Optional RPC modules

| Component | Role | qmake selection | Contract and example |
| --- | --- | --- | --- |
| `RpcMailbox` | One incoming deferred transaction | `resource_telemetry_rpc` | [Wire/state contract](../../../../doc/RpcMailboxV1.md), [server example](../../../../examples/rpc_mailbox/README.md) |
| `RpcSessionPool<N>` | Bounded ownership of N existing mailboxes | `resource_telemetry_rpc_sessions` | [Session API](../../../../doc/RpcSessionsV1.md), [four clients](../../../../examples/rpc_multi_session/README.md) |

The Integration manifest includes the selected server's Telemetry dependencies.
The independent client runtime uses `telemetry_client_rpc` in its own
`client/client.pri`; see the [client API](../../../../doc/RpcClientV1.md).
Server applications select the desired flags before including
`integration/integration.pri`. Descriptor/Values still use
`resource_telemetry`. Repeated manifest inclusion adds no duplicate sources.
The [qmake fixture](../../../../tests/structured/resources/README.md) builds all
eight maintained combinations and checks the actual source/object selections.

The client has no local Model or mailbox dependency. The pool copies only its
bindings; each mailbox owns separate caller-provided request/result storage.
One serialized owner can share a Model and Workspace. Connection identity,
generation, reply pairing, framing and ACK handoff belong to application;
none is added to RPC wire v1.0 or Resource v1. Application agrees on endpoint
types once before admitting a session. `trustedRpcMapping` is that explicit
application assertion, not an automatic descriptor check.

```cpp
#include <telemetry/descriptor/Descriptor.hpp>

// adc::branch and metrology::branch are telemetry::group(name, items...) branches.
inline constexpr telemetry::Model model{adc::branch, metrology::branch};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};

// Optional alternative for immutable compile-time metadata: only the final
// byte array needs to survive linking when the streaming object is unused.
inline constexpr auto descriptorBytes =
    ::telemetry::descriptor::packDescriptor<descriptor>();
```

The descriptor copies the Model view and borrows its group tables and names.
Table and name storage must outlive the descriptor and remain immutable;
the Model object itself need not outlive that copied view. Owner state and slot targets may change;
neither is inspected by construction, hashing or reading. A descriptor view
also borrows its owning descriptor; obtaining a view from a temporary is
rejected. Copying a descriptor does not copy or own the underlying tables.

## Format and identity

The [wire v3 contract](../../../../doc/WireV3.md) defines the frozen bytes
and the [architecture](../../../../doc/Architecture.md) explains the provider boundary. The file contains a 64-byte `TDS3` header, then Type,
Catalog and Field/Command/Service records. Integers are canonical little-endian;
strings are a u32 byte length followed by UTF-8 without NUL. Wire bytes contain
no pointers, C++ padding, unit, limits, defaults or policy flags. Descriptor
v3.1 adds an optional, unique type name on enums and structs, emitted when
`TELEMETRY_TYPE_NAMES` (Config.hpp, default 1) is on, and a skippable
attribute area that no SDK tag uses yet; without both, the bytes are v3.0.
A v3.0 endpoint record ends with its name; a v3.1 record may continue with an
attribute area flagged in its header.

The one u64 fingerprint is FNV-1a over the entire descriptor with header
bytes 16–23 treated as zero. It is computed once during construction, or
at compile time for a constexpr object. `read()` and `fingerprint()` never
recalculate it. Field writable capability comes from its definition type,
not the current availability of a setter slot or weak function address.

Changing shapes, dictionaries, positions, names or writable capability changes
the descriptor bytes and normally the fingerprint. Owner/callback addresses,
live values and slot availability do not. A hash collision remains possible;
the fingerprint is not a payload checksum or an access-control mechanism.

## Streaming and storage

```cpp
std::array<std::byte, 128> out;
telemetry::descriptor::Cursor cursor = 0;
auto result = descriptor.read(cursor, out);
// Consume out[0 .. result.written), then continue with result.next.
```

Here the core descriptor cursor is a byte offset. Reading may stop inside a
record or UTF-8 string; the reconstructed file is identical to a single read.
At `size()` the result is `Ok`, zero bytes and EOF. Beyond `size()` it is
`InvalidCursor`. An empty buffer before EOF gives `BufferTooSmall`. Errors
preserve the cursor and leave the output untouched.

The builder, view, format and fingerprint belong to `telemetry::descriptor`;
their reads return `telemetry::descriptor::ReadResult` with `ReadStatus`.
`DescriptorFile` is the Resource adapter and maps those outcomes to
`resource::ReadResult`. There are no duplicate Descriptor classes, enum imports
or old include entry points in Integration. `ValuesFormat.hpp` owns only the
live Values header size and availability status bytes.

The streaming descriptor owns a 12-byte index entry per segment plus one
sentinel. A segment is a header, type prefix, member, enum entry, catalog or
endpoint. It locates a resume position by binary search, then emits only
visited segments. Skipping a string prefix does not rescan it. Reading costs
O(log M + emitted bytes + visited segments), with bounded stack and no heap.
Construction validates names, sorts name indexes in place to check uniqueness,
restores positional order, computes offsets and hashes once. No complete
serialized duplicate is created in RAM.

For a constexpr model, **packed bytes in Flash are the recommended storage**:
the [measured fixture](../../../../tests/structured/descriptor/README.md) needed
less Flash and fewer cycles than on-demand emission. `packDescriptor` returns
an ordinary constexpr `std::array<std::byte, N>`; it uses the same canonical
emitter as streaming. Use one representation for a provider. The streaming
form remains useful when names/catalog metadata are constructed at startup.
No explicit `init()` phase is required. `DescriptorFile{descriptorBytes}`
wraps packed storage; `DescriptorFile{descriptor}` wraps indexed streaming.
Both borrow a stable immutable lvalue and reject temporary sources.

The entire output span passed to `DescriptorFile::read()` must be disjoint from
the immutable source and its borrowed metadata, including packed bytes, tables
and names. This applies to packed and canonical streaming descriptors, even
when only a prefix of the output will be written. This is a caller precondition,
without a runtime alias check. Copying into the descriptor's own backing storage
would violate its immutability; `memmove` would not make that mutation valid.
Use a separate transport/output buffer. Custom streaming sources also retain
their own `read()` preconditions.

## Validation and limits

Names are nonempty valid UTF-8. Factory arguments preserve const array extent
long enough to reject embedded NUL in literals; mutable character buffers and
char pointers use bounded C-string semantics. A pointer must reference a valid
NUL-terminated string. Text after its first terminator is not part of that
string. Names are stored as the same single borrowed pointer as before; native
endpoint dispatch gains neither a member nor a runtime name check.

Catalog names are unique within their category; endpoint names are unique
within their catalog. Cross-category duplicates are valid. In a constexpr
descriptor, an invalid declaration produces a construction diagnostic. At
runtime descriptor validation reports `DescriptorError`, size/fingerprint zero
and `InvalidData` on read; invalid factory names themselves remain contract
violations. `Descriptor<F,C,S,Profile>` checks its model against profile ceilings for
type count/depth, members, arrays, enum entries, strings, wire bytes, expanded
nodes, descriptor bytes and total catalogs/endpoints. The independent global
`Type<T>`/registry checks still apply; a profile cannot enable an unsupported
wire type. Profiles normally lower ceilings. The current implementation does
not require every profile constant to be at most its `Limits` counterpart,
so descriptor-only ceilings can also be raised. This does not change
global wire-type validation or the supported structural types.

The independent host parser fixture checks header budgets before allocating
record collections, validates every reference and recomputes wire sizes,
depth and expanded nodes without expanding arrays. It is a test oracle;
the [reference client](../../../../tests/structured/client/README.md) consumes the
same descriptor and values bytes independently of any control protocol.

## Values and integration

```cpp
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <integration/telemetry/v3/ValuesFile.hpp>
#include <resource/FileSystem.hpp>

namespace ts = telemetry;
namespace rs = resource::telemetry::v3;
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = ::telemetry::descriptor::packDescriptor<descriptor>();
inline constexpr rs::DescriptorFile descriptorFile{descriptorBytes};

inline std::array<std::byte, model.maxFieldScratch()> storage;
inline ts::Workspace workspace{storage};
inline constexpr rs::ValuesFile values{descriptor, workspace};
constinit auto files = resource::filesystem(
    resource::file("/telemetry/descriptor.bin", descriptorFile),
    resource::file("/telemetry/values.bin", values));
```

`ValuesFile` copies the descriptor's field index and cached fingerprint, then
builds one 8-byte offset/packed-ID entry per field plus an EOF sentinel. It
does not borrow the descriptor object itself. The underlying tables/names and
Workspace must outlive all reads; metadata must stay immutable. Construction,
`size()` and STAT never invoke a getter. For constexpr metadata the index and
size are computed at compile time. Both providers are read-only.

The values header is **24 bytes**, all integers little-endian:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | bytes[4] | `TVL3` |
| 4 | u16 | major 3 |
| 6 | u16 | minor 0 |
| 8 | u32 | Field count |
| 12 | u32 | Total bytes |
| 16 | u64 | Cached descriptor fingerprint |

The fingerprint identifies the required descriptor. The receiver compares it
once when reading the file header, before interpreting values. It is not a
checksum of changing payloads and is not copied into each resource chunk.
No hash is computed by values READ. Offline exports need both descriptor and
values files. Descriptor wire bytes from Stage 09 have not changed.

Each field contributes `u8 status + wireSize<T> payload` in descriptor order.
Status 0 is Ok; status 1 is Unavailable, with an all-zero payload that must not
be decoded as a value. Other statuses are invalid. Empty structs/arrays still
contribute their status byte. Total size is always `24 + sum(1 + wireSize<T>)`.

READ checks whole-token capacity **before** reserving scratch or resolving a
binding. A fitting token reads its getter once and encodes that snapshot using
the existing typed Field thunk. There is no allocation or intermediate file
copy. This is a snapshot of one returned T, not an atomic snapshot of all fields
or synchronization with a concurrent writer.

Valid cursors are byte positions inside the header, exact token starts and EOF.
Interior payload offsets are rejected before touching getters/output. Resume
uses binary search over offsets and O(1) packed-ID access; it does not replay
earlier getters. A completed prefix is returned as Ok when the next token
does not fit. With no completed bytes, READ returns BufferTooSmall and preserves
the cursor. EOF gives Ok/zero bytes/eof=true without callbacks.

`requiredWorkspace()` considers only Field `readScratchBytes`, including
worst-case alignment margin. A const-reference getter contributes zero, even
when that Field's setter needs a large decoded object;
`maxTokenSize()` is the minimum payload capacity to make progress over every
field. The generic protocol's u16 payload ceiling is 65535 bytes. A larger
token returns BufferTooSmall; callers must report this capacity mismatch,
not retry forever. Large-object fragmentation is outside this provider.

Local/Workspace selection is the existing compile-time policy, default 32 B;
it changes storage only, never validation or wire bytes. All-local files never
access Workspace. If a file contains Workspace-backed fields, its output span
must not overlap Workspace storage: InvalidData is returned before even a
header prefix is written. Actual free scratch is checked before a large
getter. Insufficient scratch gives InternalError, not a false Unavailable
value. If earlier tokens were completed, that prefix is returned first;
retrying its nextCursor reports the configuration error without replay.

An invalid Descriptor produces a rejected ValuesFile (`size()==0`,
`fingerprint()==0`, READ InvalidData). Providers are copyable; copies own their
offset indexes but share the borrowed sources/Workspace. Parallel reads require
external serialization or separate provider/Workspace instances. Leases keep
the documented LIFO lifetime contract.

A const-reference getter is encoded from its existing native object, without
copying that payload or reserving read scratch. The complete Values READ output
span must be disjoint from live application objects: the file writes its header
and token status as well as the endpoint payload. The endpoint's payload guard
does not establish disjointness for those surrounding bytes. Getters keep their
objects alive and stable throughout encoding; the provider retains no returned
reference between READ calls. Ownership does not change the descriptor
fingerprint or Values wire layout.

## Core boundary

DescriptorFile and ValuesFile adapt an immutable Model to resource files only.
They do not own peers, readiness, sessions, packet routing, correlation or retry
policy. The application chooses when it compares the descriptor fingerprint;
values retain that fingerprint in their header. No Bind is required for local
file reads or encoded endpoint operations.

The former Bind/Binding/Exchange implementation now lives in the explicitly
selected [protocol example](../../../../examples/structured_protocol/README.md).
It is not compiled or exposed by `resource.pri`.

## Optional RpcMailbox

[`RpcMailbox.hpp`](RpcMailbox.hpp) adds one read/write provider for FieldWrite,
Command, Service and FieldRead through the existing checked encoded Model API.
[`wire/RpcWire.hpp`](../../../wire/RpcWire.hpp) defines `RpcOperation`, `RpcState`,
`rpcRequestHeaderBytes=24`, `rpcReplyHeaderBytes=32`,
`rpcReceiveTimeoutMs=10000`, and the `rpcCursor`, `rpcTransactionId` and
`rpcOffset` helpers. These are RPC wire v1.0 constants; descriptor wire 3.0,
Values and generic Resource wire bytes are unchanged.

```cpp
#include <integration/telemetry/v3/RpcMailbox.hpp>

inline constexpr auto rpcModelView = model.view();
inline std::array<std::byte, model.maxScratch()> rpcScratch{};
inline telemetry::Workspace rpcWorkspace{rpcScratch};
inline std::array<std::byte, 256> rpcRequest{};
inline std::array<std::byte, 256> rpcResult{};
inline resource::telemetry::v3::RpcMailbox rpc{
    rpcModelView, rpcWorkspace, rpcRequest, rpcResult};
inline const auto rpcFiles = resource::filesystem(
    resource::file("/telemetry/rpc.bin", rpc));
```

The constructor requires an exact stable lvalue ModelView (const is allowed)
and mutable lvalue Workspace. Temporary views, Workspace temporaries and
conversion proxies are rejected. It borrows those objects and both buffers;
their addresses, underlying metadata and binding targets must remain valid.
The mailbox is neither copyable nor movable. Request capacity includes the
whole 24-byte header and canonical request; result capacity holds only the
payload. Choose both capacities for the endpoints your application exposes.
Request capacity outside `[24, UINT32_MAX]` or result capacity above
`UINT32_MAX-32` is a construction error: constexpr construction fails and
runtime construction terminates. Buffers, Workspace, mailbox metadata and
operation spans follow the disjoint-storage preconditions in the
[RPC contract](../../../../doc/RpcMailboxV1.md).

`write()` copies a contiguous fragment or acknowledges an identical retry.
Accepted final seals Pending without invoking application code. One later
`process(nowMs)` transitions Running, makes at most one encoded dispatch
attempt, and retains its Ready result. `read()` returns a whole 32-byte status
header and an optional result payload; `size()` is the current logical read
record length. Check state before dispatch/endpoint statuses. `resetSession()`
discards logical request/result state and the high-water ID outside Running;
it is a no-op during an active callback. Storage is not zero-filled on reset
or timeout. Request retries retain exact bytes until a new request, timeout
or session reset makes them inaccessible.

One serialized application owner admits one client session, agrees the
descriptor fingerprint, and hands the final WRITE ACK to its transport before
calling `process()` for Pending. The provider has no heap allocation, mutex,
atomic state, task or transport identity. Ingress attaches an external
`connectionGeneration` to each queued packet; the owner rejects stale metadata
before the Resource processor. Clearing only the queue is insufficient when
old ingress can enqueue later. Generation is never an RPC wire field.

`complete=true` is receipt/seal, not guaranteed execution after disconnect or
reset. A lost result can leave an unknown outcome; retrying a non-idempotent
operation in a new session can repeat its effect. Deduplication is confined to
the currently retained admitted session. The complete
[host example](../../../../examples/rpc_mailbox/README.md) demonstrates file
registration, external generation checks, ACK ordering, retry and reply decode.

## qmake

Select each provider explicitly through `lib/integration/integration.pri`.
It includes the selected providers' Telemetry and Resource dependencies; the
generic Resource manifest has no Telemetry selections or PFR dependency.

| CONFIG selection | Compiled resource provider source |
| --- | --- |
| Neither flag | None |
| `resource_telemetry` | `lib/integration/telemetry/v3/detail/Values.cpp` for DescriptorFile/ValuesFile |
| `resource_telemetry_rpc` | `lib/integration/telemetry/v3/RpcMailbox.cpp` for RpcMailbox |
| Both flags | Both provider sources |

For DescriptorFile and ValuesFile:

```qmake
CONFIG += resource_telemetry
include(path/to/lib/integration/integration.pri)
```

For RpcMailbox alone, use `CONFIG += resource_telemetry_rpc` in the same
explicit include sequence. To expose all three providers, set
`CONFIG += resource_telemetry resource_telemetry_rpc` before including
`integration.pri`. RPC does not require the Descriptor/Values flag, and
Descriptor/Values consumers do not compile `RpcMailbox.cpp`.

Integration selects the compiled Model adapter, ABI and generic protocol sources
needed by each provider. Telemetry and Resource do not include Integration.
Repeated manifest includes do not duplicate the selected sources.
