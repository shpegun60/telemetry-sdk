# Device file conventions

A device speaks to a host through a flat list of files (the
[Resource protocol](../lib/resource/README.md)). Some of those files have a
fixed path and a fixed format, and every host looks for them by that path. The
rest are the device's own.

This page covers:
- every file the SDK defines, and who makes each one;
- what a host does when a file is there, missing or wrong;
- what a host may change;
- the rules for the device's own files.

The byte layouts are defined elsewhere; each row links to its definition.

## The `/telemetry/` folder

`/telemetry/` belongs to the SDK. A device puts only these files there:

| Path | What it is | Made by | A host | Needed |
|---|---|---|---|---|
| `/telemetry/descriptor.bin` | The device's fields, commands and services: `TDS3`, [WireV3.md](WireV3.md) | The application places a `DescriptorFile` ([v3](../lib/integration/telemetry/v3/README.md)); a TCP profile serves the descriptor it is given | reads | For everything but files |
| `/telemetry/values.bin` | Every field's value at once: `TVL3`, [WireV3.md](WireV3.md), `ValuesFormat.hpp` | The application places a `ValuesFile` | reads | No |
| `/telemetry/identity.bin` | Which unit this is and what its user calls it: `TID1`, [`IdentityWire.hpp`](../lib/wire/IdentityWire.hpp) | The application places an `IdentityFile`, or its own class derived from it to be renamed | reads; changes it only through `rename()` | No |
| `/telemetry/session.bin` | Binds one connection to a session number: four bytes, a little-endian nonzero `u32` | The TCP profile | writes once while connecting | When `read-many.bin` or `batch.bin` is listed |
| `/telemetry/read-many.bin` | What ReadMany admits: `TRM1`, [ReadManyEncoded.md](ReadManyEncoded.md) | `ReadManyProfile` and `EncodedProfile` | reads | No |
| `/telemetry/batch.bin` | What Mixed Batch admits: `TBM1`, [MixedBatchEncoded.md](MixedBatchEncoded.md) | `EncodedProfile` | reads | No |
| `/telemetry/rpc.bin` | An RPC mailbox: `TRPC` requests, `TRPS` replies, [RpcMailboxV1.md](RpcMailboxV1.md) | The application places an `RpcMailbox`; this path is the convention, and an application may serve it under another path outside `/telemetry/` | an RPC client writes and reads it | No |

The first six are the protocol's own files; the mailbox is a channel a host
writes to. Every path is exact: a host does not look for a file under another
name or in another folder.

Things to know about some of these files:
- **Session file.** It is written with cursor 0, `final`, and exactly four
  bytes. Its Stat must say four bytes and Writable. The first write binds the
  profile to the session; every later write is refused, and so is a write after
  the profile has served its first encoded request. A profile serves one
  connection: create one per accepted connection
  ([QtConnectionSession.md](QtConnectionSession.md)).
- **Identity file.** An `IdentityFile` that was never `set()` serves an empty
  file. A host reads it to its end and takes no bytes as no identity.
- **Mailbox path.** Several RPC sessions may each serve `/telemetry/rpc.bin` at
  index 0 of their own file view ([RpcSessionsV1.md](RpcSessionsV1.md)).

## What a host does with them

### Over TCP: `qt::ConnectionSession`

`qt::ConnectionSession` connects over TCP. It lists the device's files, then
looks for the descriptor, session, ReadMany and Batch files anywhere in the
list:

| The device | The connection |
|---|---|
| lists no `descriptor.bin` | carries the device's files only: `filesOnly()`, `limitation()` is `NoDescriptor` |
| refuses to stat or read it | files only, `DescriptorUnreadable` |
| serves one under 64 bytes or over 4 MiB (`Limits::maxDescriptorBytes`) | files only, `DescriptorSize` |
| serves one that does not parse, or whose size and header disagree | files only, `DescriptorRejected` |
| lists `read-many.bin` or `batch.bin` without `session.bin`, or `session.bin` without either | files only, `Capability` |
| serves a `session.bin` whose Stat is not four bytes and Writable, or that refuses the write | files only, `Capability` |
| serves a `read-many.bin` or `batch.bin` that is not 24 bytes of `TRM1` or `TBM1`, or whose session or descriptor fingerprint does not match | files only, `Capability` |
| lists none of `session.bin`, `read-many.bin` and `batch.bin` | connects; `readManySupported()` and `batchSupported()` are false |
| lists `descriptor.bin`, `session.bin`, `read-many.bin` or `batch.bin` twice | fails: `lastError()` is "Invalid Descriptor discovery reply" |
| lists no `identity.bin`, or serves it empty | connects; `identityState()` is `Absent` |
| serves an `identity.bin` that is not an identity | connects; `identityState()` is `Unreadable`, and `identityProblem()` says why |

A `ConnectionSession` keeps the first 4096 listed paths in `discoveredPaths()`.
It finds the descriptor, session, ReadMany and Batch files further down the
list too. But it looks for the identity, and applies the write protection below,
only among those 4096, and it reads the identity from its first entry. List the
SDK's files first. Other hosts have their own limits: the code generator's
`fetch` lists up to 65536 paths.

### Over a serial link: `qt::SerialTransport`

A serial link reads the descriptor and, with ReadMany discovery on
(`SerialTransport::setReadManyDiscovery(true)`), requires `read-many.bin`. It
never looks for `session.bin`, `batch.bin` or `identity.bin`. It has no files-only
mode: a descriptor that is missing, unreadable or rejected, or a required
`read-many.bin` that is missing or does not match, fails the connection.

## Order

- **With a TCP profile and the application's own files,** the table must begin
  with `profile.systemFiles()`: the same paths, in the same order. Otherwise the
  `Server` closes. `resource::filesystem(profile.systemFiles(), ...)` builds
  such a table:

  ```cpp
  const auto table = resource::filesystem(
      profile.systemFiles(),
      resource::file(telemetry::protocol::identity::path, identity),
      resource::file("/logs/events.csv", eventLog));
  ```

- **Then the identity,** by convention, and then the device's own files.
- **A plain `Server(table)`, or a serial link,** checks no order. Keep the same
  order anyway: the descriptor first, the SDK's other files next, the device's
  own files last.

## What a host may change

- **The six protocol files.** `ConnectionSession::submitResource` refuses a
  WRITE or a SAVE to `descriptor.bin`, `values.bin`, `identity.bin`,
  `session.bin`, `read-many.bin` and `batch.bin` with `ReadOnly`, before the
  request leaves. `ConnectionSession::protocolFile(path)` says whether a path is
  one of them, so a program can avoid offering such a write.
  - The identity changes only through `ConnectionSession::rename()`.
  - The session file is written only by the session's own connection steps.
- **Every other file** a host may write when its Stat says Writable, and keep
  across a restart (SAVE) when it says Savable. That includes the device's own
  files and an RPC mailbox.
- **What a file allows** comes from its provider's methods:
  - `size()` and `read()` make it Readable;
  - `write()` makes it Writable;
  - `save()` makes it Savable.

  A device that keeps written bytes at once still adds a `save()` that answers
  complete; a file without one keeps a write only until the device restarts.
- **How a host writes a file.** `files::FileWriter` writes a whole file from
  cursor 0, in pieces, with `final` on the last piece, then SAVE when the file
  is Savable. It writes exactly as many bytes as the file's Stat size, and
  stops with `SizeMismatch` otherwise: writing a file does not change its size.
- **A mailbox is not written that way.** An RPC client writes each request at
  its own length, and the mailbox's Stat size is that of its reply
  ([RpcMailboxV1.md](RpcMailboxV1.md)).

## The device's own files

- **Location.** Outside `/telemetry/`.
- **Valid path** (`resource::detail::validPath`):
  - at least two bytes, a leading `/` and no trailing `/`;
  - no empty, `.` or `..` component;
  - no byte below 0x20, no 0x7f, no `\`.

  An invalid path stops the program in `resource::file()`, or fails to compile
  when the table is `constexpr`.
- **Unique.** A path listed twice also stops the program, in the `FileSystem`
  constructor.
- **UTF-8.** The SDK does not check it, and every client decodes paths as UTF-8.
- **Flat names.** A path such as `/logs/2026/events.csv` is one name. The
  device has no folders. A client shows the slashes as folders and saves the
  file at that relative path under the device's folder.
- **Short names.** A LIST reply carries a path of up to 65533 bytes, but a local
  copy has to fit a file system.
- **Names a desktop can keep.** Avoid:
  - `< > : " | ? *`;
  - a component ending in a dot or a space;
  - the Windows device names `CON`, `PRN`, `AUX`, `NUL`, `COM1`-`COM9`,
    `LPT1`-`LPT9`.

  telemetry-ux rewrites such names when it saves a copy, so the local name no
  longer matches the device's.
- **Sizes.**
  - A file is at most 4 GiB - 1 bytes (`FileSize` is a `u32`).
  - One READ or WRITE carries at most 65535 bytes; a transport's buffers may
    carry fewer.
  - A file whose size changes, such as a log that grows, is read to its end;
    its Stat size is a snapshot.

## Limits in one place

| What | Limit | Where |
|---|---|---|
| Paths a `ConnectionSession` keeps | 4096 | `DescriptorBootstrap::maxPaths`; telemetry-ux `DeviceFilesModel` too |
| Path bytes in a LIST reply | 65533 | `protocol/Wire.hpp` |
| Bytes in one operation | 65535 | `protocol/Wire.hpp` |
| File size | 2^32 - 1 | `resource/Types.hpp` |
| Descriptor | 64 bytes to 4 MiB | `Limits::maxDescriptorBytes` |
| Identity text | 64 bytes each; the file at most 276 written, 512 read | `IdentityWire.hpp` |
| Identity id | letters, digits, `.`, `_`, `-`; begins with a letter or a digit | `IdentityWire.hpp` |

## Related documents

- **The Resource protocol:**
  - [lib/resource/README.md](../lib/resource/README.md) - files, providers, paths;
  - [protocol/README.md](../lib/resource/protocol/README.md) - operations and replies.
- **The telemetry files:** [WireV3.md](WireV3.md),
  [v3/README.md](../lib/integration/telemetry/v3/README.md).
- **The TCP profiles and the identity:**
  [tcp/README.md](../lib/integration/tcp/README.md).
- **How a connection uses the files:**
  [QtConnectionSession.md](QtConnectionSession.md).
- **ReadMany, Mixed Batch and RPC:**
  [ReadManyEncoded.md](ReadManyEncoded.md),
  [MixedBatchEncoded.md](MixedBatchEncoded.md),
  [RpcMailboxV1.md](RpcMailboxV1.md), [RpcSessionsV1.md](RpcSessionsV1.md).
- **The checks:** [tests/device_files](../tests/device_files/README.md) -
  reading, writing and the session against real providers.
