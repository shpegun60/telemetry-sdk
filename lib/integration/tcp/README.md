# Native TCP stream integration

Optional component `tcp_server` supplies no socket, OS, queue, RPC or additional
wire envelope. It carries existing Resource v1 packets and Encoded TEQ1 frames
unchanged. One application owner controls each connection and shared Model/Workspace.

`FrameReader` consumes arbitrary partial or coalesced chunks into caller-owned
storage, returns the consumed prefix, and retains one complete frame until
`release()`. `Server::receive()` dispatches that frame through the existing
Resource or Encoded dispatcher. One pending reply blocks further ingress until
`releaseReply()`. Preserve unconsumed input; no operation execution queue exists.
Input passed to `FrameReader::feed()` must be disjoint from its entire storage
span. An overlapping input is refused with `Invalid` and zero consumed bytes,
before any write; the reader's prior state and storage remain unchanged, so a
disjoint continuation can still complete a partial frame. An already ready or
failed reader keeps its existing status without consuming input.

## Boundaries and bootstrap

Request first bytes 1–5 select Resource LIST/STAT/READ/WRITE/SAVE. Their sizes
are 9/5/13 bytes, 16 plus the little-endian u16 at offset 14, or 13 for SAVE. First byte `0x54`
selects TEQ1, whose size is 32 plus little-endian u32 `payloadBytes` at offset20.
Full existing TEQ1 validation still applies. Bounds are checked before addition.

Resource replies require one known outstanding operation: STAT6, WRITE14, SAVE10,
LIST/READ12 plus u16 at offset10. A single `InvalidData` byte is never a completed
reply: EOF or timeout closes the connection. A valid normal error envelope can
start with the same byte, so it cannot be classified from that byte alone.
The server closes on malformed input instead of emitting an ambiguous short error.

Bootstrap performs sequential checked LIST/STAT/READ against a configured
Descriptor path, owns copied descriptor bytes and validates them before admission.
There are no packet retries. After one bootstrap reply, reject already-buffered
trailing bytes before issuing another request. Timeout, malformed or unsolicited
input closes the connection; never label an old reply with a new request identity.
After bootstrap the application uses existing `EncodedBackend` and its FIFO.

## Session and lifetime

The first fully validated TEQ1 request binds its nonzero session to this admitted
TCP connection. Later session mismatches and nonincreasing request IDs close it
without endpoint execution. This is connection binding, not authentication or
duplicate suppression across connections. The application validates the peer and
pins the descriptor revision; changing that revision closes the connection.

RX, TX and Workspace storage must be stable and mutually disjoint. Send only
`reply()` bytes, and call `releaseReply()` after the transport owns a full copy or
has finished reading the borrowed buffer. `close()` revokes logical access; it
does not establish physical TX/RX release or erase buffer bytes. Close/drain the
driver before destruction. Reentrant ingress is refused; closure during a native
callback prevents publishing a later reply.

For the initial single-client profile the server finishes synchronous dispatch,
closes the old socket and discards its parser state before admitting a new one.
Successful fresh descriptor bootstrap then establishes prior server context
closure. Local socket disconnection alone is not evidence of remote quiescence.

```cpp
#include <integration/tcp/Server.hpp>
// Stable Model/Workspace/files/rx/tx live longer than server and socket operations.
telemetry::integration::tcp::Server server(model.view(), workspace, files.view(), rx, tx);
// Owner retains input.subspan(result.consumed), hands server.reply() to transport,
// and calls releaseReply() only after the transport owns that complete reply.
```

CMake links `Telemetry::tcp_server`. qmake includes `tcp.pri` explicitly.
See the maintained [host tests](../../../tests/tcp_stream/README.md).

For explicit TRM1 and TBM1 discovery together, select
`Telemetry::tcp_encoded_profile` and include `EncodedProfile.hpp` before creating
the optional Server constructor. qmake selects `telemetry_tcp_encoded_profile`
or includes `tcp_encoded_profile.pri`. One Resource session WRITE binds both
capabilities; op5 routes ReadMany and op6 routes Mixed Batch using that same
session and replay guard. `ReadManyProfile` keeps its previous read-only profile.
The legacy constructors do not acquire Batch link roots. See
[Qt connection](../../../doc/QtConnectionSession.md#optional-mixed-batch) and
[installed consumer tests](../../../tests/qt_batch/README.md).

**The device's own files beside a profile's.**
- A profile serves its own files: the descriptor, `session.bin`,
  `read-many.bin`, and `batch.bin` for the encoded profile.
- To serve logs, settings or data as well, begin a table with the profile's
  files and pass it with the profile. `systemFiles()` names the same providers
  the profile uses, so a session binds the same way:

```cpp
const auto table = resource::filesystem(profile.systemFiles(),
                                        resource::file("/logs/events.csv", eventLog),
                                        resource::file("/config/settings.csv", settings));
telemetry::integration::tcp::Server server(model.view(), workspace, profile, table.view(), rx, tx);
```

- A table that does not begin with the profile's files, in their order, is
  refused: the Server is closed.
- The client lists the profile's files first, then the device's.
  [Device files](../../../tests/device_files/README.md) checks this over a live
  session.
- [Device file conventions](../../../doc/DeviceFileConventions.md) lists every
  `/telemetry/` file, what a host does with each and the rules for the device's
  own file names.

**Which unit it is: `/telemetry/identity.bin`.** The descriptor is the same for
every unit of one firmware, so a client cannot tell two boards apart by it, nor
keep each one's files apart. A device that serves an identity file can be:
- **id:** unique to the unit and never changing - the MCU's unique ID
  (`identity::hexId` formats it) or a serial number from production; letters,
  digits, `.`, `_` and `-`, at most 64 bytes.
- **name:** what its user calls it; may be empty or change.
- **model, firmware:** optional.

```cpp
resource::telemetry::v3::IdentityFile identity; // IdentityFile.hpp, tcp_encoded_profile
std::array<char, 24> text{};
identity.set(telemetry::protocol::identity::hexId(uid, text), savedName, "PA-3", "1.4.2");
const auto table = resource::filesystem(profile.systemFiles(),
                                        resource::file(telemetry::protocol::identity::path, identity),
                                        resource::file("/logs/events.csv", eventLog));
```

- The bytes are defined once, in `wire/IdentityWire.hpp`, writer and reader
  together; at most 512 bytes, so one READ returns the file.
- `rename()` changes the name in place; call it from the context that serves
  the file. Clients read the identity when they connect.
- `qt::ConnectionSession` reads it once a connection is ready or files only, and
  offers `identityState`, `deviceId`, `deviceName`, `deviceModel`,
  `deviceFirmware`; a missing or broken file never refuses a connection.
