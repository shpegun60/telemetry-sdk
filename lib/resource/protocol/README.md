# Resource packets v1 (C++20)

Authors: Ruslan Kovtun (shpegun60), codexAi. [MIT](../LICENSE).

The `protocol` module depends only on the [resource core](../README.md).
`process(view, request, response)` accepts one complete request and returns
`Reply{status, written}`. The caller provides framing, transport, checksums and
retry policy. No telemetry, session storage, dynamic allocation or path lookup
is performed. Include `lib/resource/resource.pri`, or compile `Protocol.cpp`
with `lib` on the include path. The public header is
`<resource/protocol/Protocol.hpp>`; the namespace is `resource::protocol`.

All integers are encoded explicitly little-endian, without struct layout or
unaligned typed loads. There is no terminator after paths or data. Boolean
bytes are exactly zero or one. Extra trailing input bytes are invalid.

| Op | Request | Response |
|---|---|---|
| LIST = 1 | `u8 op, u64 cursor` | `u8 status, u64 next, u8 eof, u16 dataSize, data[]` |
| STAT = 2 | `u8 op, u32 index` | `u8 status, u32 size, u8 flags` |
| READ = 3 | `u8 op, u32 index, u64 cursor` | `u8 status, u64 next, u8 eof, u16 dataSize, data[]` |
| WRITE = 4 | `u8 op, u32 index, u64 cursor, u8 final, u16 dataSize, data[]` | `u8 status, u64 next, u32 consumed, u8 complete` |
| SAVE = 5 | `u8 op, u32 index, u64 cursor` | `u8 status, u64 next, u8 complete` |

Status codes are fixed: Ok=0, InvalidFile=1, NotReadable=2, NotWritable=3,
InvalidCursor=4, CursorExpired=5, BufferTooSmall=6, InvalidData=7,
InternalError=8. STAT flags are Readable=1, Writable=2 and Savable=4. Existing
codes are not renumbered. `consumed` remains u32 as in the revised specification;
it is checked against the actual u16-length request payload.

LIST data consists of repeated `[u16 pathLength][path bytes]` entries. The
index of the first entry is the request cursor; each following entry increments
it by one. LIST never calls provider `size()`. A cursor equal to fileCount is
valid EOF; a larger cursor is invalid. A whole path entry must fit the available
payload after the 12-byte header. With some entries emitted, a full buffer
returns Ok and the first unreturned index. If the first entry cannot fit, it
returns BufferTooSmall with an unchanged cursor. Because the complete data size
is u16 and includes the two-byte path length, the largest individually
transferable LIST path is 65533 bytes. A longer path ends an already populated
page with Ok and leaves its index as the next cursor. Starting at that entry
returns InvalidData even with a large response buffer: enlarging it cannot
make such an entry fit. That reply has the unchanged cursor, no data and
`eof=0`. To skip this unrepresentable path and continue listing later entries,
the client sends LIST with `cursor + 1`; advancing to `fileCount` returns EOF.
Earlier valid paths remain listable.
The transport-independent core deliberately imposes no wire length limit.

READ payload capacity is min(response size - 12, 65535). WRITE validates the
entire packet and the 14-byte reply capacity before invoking the provider.
Providers are invoked synchronously; in-place request/reply storage is supported.
Malformed packets, invalid final bytes and unknown operations produce a single
InvalidData status byte if possible. Valid requests retain the operation's
normal response envelope on errors. If that envelope cannot fit, no provider
is invoked and `Reply{BufferTooSmall, 0}` tells the transport to enlarge its
buffer. Provider result counts/statuses are checked before encoding; inconsistent
results become InternalError. An Ok, unfinished READ/WRITE must produce bytes
or change its cursor; returning zero bytes with an unchanged cursor becomes
InternalError. EOF/complete with zero bytes remains valid, as do cursor-only
progress and byte-only progress. This cannot repair a provider that has already
written outside its C++ span: that remains a provider contract violation.

WRITE retries are delivered again. There is no transaction or deduplication
layer. Clients check status, byte count, progress and completion on every reply.

WRITE `final` marks the end of the file: the provider applies what it was
given. SAVE is a separate operation that asks the provider to persist what the
file holds now. A file is Savable when its provider has
`SaveResult save(Cursor) noexcept`; SAVE to any other file answers NotWritable,
and to an unknown index InvalidFile, both in the 10-byte envelope with the
cursor echoed. A provider persists at once (`{Ok, cursor, true}`) or in steps:
an unfinished Ok step must move the cursor, and the client sends SAVE again
from the returned `next` until `complete`. As for WRITE, an error makes no
progress and is not complete; any other result becomes InternalError. SAVE
validates the exact 13-byte request and the 10-byte reply capacity before
invoking the provider.

Compatibility. Old servers answer SAVE as an unknown operation: the raw
processor with the one-byte InvalidData, a TCP server built before SAVE by
closing the connection. A client therefore sends SAVE only to a file whose
STAT says Savable, which an old server never does. Clients built before SAVE
refuse a STAT whose flags exceed 3 as malformed, so a device that serves a
Savable file needs clients built with SAVE. From SAVE on, `parseStat` keeps
flag bits it does not know and `resource::has()` ignores them, so a later flag
does not break this client.

## Bounded client API

Include `<resource/protocol/Packets.hpp>` for the header-only
`resource::protocol::client` API. Packet sizes and operation codes are shared
with `process` through `<resource/protocol/Wire.hpp>`:
`wire::listRequestSize=9`, `statRequestSize=5`, `readRequestSize=13`,
`writeRequestHeaderSize=16`, `chunkReplyHeaderSize=12`, `statReplySize=6`,
`writeReplySize=14`, `saveRequestSize=13`, `saveReplySize=10`,
`maxPayloadSize=65535` and `maxListPathSize=65533`.

| Builder | Arguments after caller-owned `Output` | Bytes written on success |
|---|---|---|
| `makeList` | `Cursor cursor` | 9 |
| `makeStat` | `FileIndex index` | 5 |
| `makeRead` | `FileIndex index, Cursor cursor` | 13 |
| `makeWrite` | `FileIndex index, Cursor cursor, Input data, bool final` | `16 + data.size()` |
| `makeSave` | `FileIndex index, Cursor cursor` | 13 |

Every builder returns `BuildResult{BuildStatus status, size_t written}`.
`BuildStatus` is `Ok`, `BufferTooSmall` or `PayloadTooLarge`; failure returns
zero written bytes and leaves output unchanged. Arrays, fixed spans and
dynamic spans use the same checked result. WRITE accepts at most 65535 payload
bytes and safely copies overlapping input before writing the request header.
The caller sends only the written prefix and keeps those bytes alive until
the transport has finished reading them.

| Parser | Additional argument | Parsed response |
|---|---|---|
| `parseList(Input reply)` | none | `status, next, eof, paths` |
| `parseStat(Input reply)` | none | `status, size, flags` |
| `parseRead(Input reply)` | none | `status, next, eof, data` |
| `parseWrite(Input reply, size_t submittedBytes)` | original WRITE payload length | `status, next, consumed, complete` |
| `parseSave(Input reply)` | none | `status, next, complete` |

Parsers return `ParseResult<Response>{parsing, response, shortError}`.
`parsing` is `ParseStatus::Ok` or `Malformed`; the explicit boolean conversion
tests parsing success. It is independent of `response.status`, which carries
the remote resource/provider status. An ordinary provider error is a valid
parsed response. All parsers also accept the one-byte `InvalidData` error
that `process` emits for a malformed request. Such a result has `shortError=true`;
only `response.status` is meaningful because no normal response fields were
transmitted. A one-byte `Ok` or any other status is malformed.

Parsing checks exact packet lengths, trailing bytes, status codes, boolean
bytes, declared data counts and error responses carrying data
or completion. LIST additionally checks every complete length-prefixed path
against the core's flat-label spelling rules and the 65533-byte path limit.
WRITE verifies `consumed <= submittedBytes <= 65535`. The caller retains the
request context to check cursor progress, expected LIST indices and provider
completion semantics. READ/WRITE cursors may be opaque and need not equal
the previous cursor plus the byte count.

**Reply views borrow the received packet.** `ReadReply::data`, every path
returned by the iterable `ListReply::paths`, and its iterators remain valid
only while the response storage is alive and unchanged. Parse and use them
before receiving the next packet into that storage; copying the parsed
result or a `string_view` does not copy the bytes. The API allocates nothing
and never returns an owning packet.

```cpp
namespace client = resource::protocol::client;
std::array<std::byte, resource::protocol::wire::readRequestSize> request;
auto built = client::makeRead(request, fileIndex, cursor);
if (built.status == client::BuildStatus::Ok) {
    // Send request[0 .. built.written); receive one complete reply.
    auto parsed = client::parseRead(receivedPacket);
    if (parsed && !parsed.shortError && parsed.response.status == resource::Status::Ok) {
        consume(parsed.response.data); // Finish before receivedPacket is reused.
        cursor = parsed.response.next;
    }
}
```

A runnable in-process transport example is
[ResourceClient.cpp](../../../examples/user_guide/ResourceClient.cpp).

See [tests](../../../tests/resources/README.md) and the small
[application facade fixture](../../../tests/resources/fixtures/resources/DeviceResources.hpp).

The C++ API lives in `resource::protocol`. Users of the former global
`resource_protocol` spelling must update their calls and rebuild
`Protocol.cpp`; packet bytes, operation codes and reply layouts are unchanged.
