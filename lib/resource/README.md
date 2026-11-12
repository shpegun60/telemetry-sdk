# Resources: files from ordinary C++ objects

Authors: Ruslan Kovtun (shpegun60), codexAi. [MIT](LICENSE).

A resource file is a **path plus a provider**. The path names the file for a
client. The provider supplies its bytes. No operating-system filesystem,
directory tree, storage allocation or telemetry model is created by declaring
that path.

Optional model providers are owned by
[Integration](../integration/telemetry/v3/README.md).
The generic file core and its build manifest have no Telemetry or Client
dependency. See the [module boundaries](../../doc/Modules.md) for explicit
adapter selection.

```cpp
#include <resource/Resource.hpp>

inline constexpr std::array versionBytes{
    std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
inline constexpr resource::BytesFile versionFile{versionBytes};

inline constinit const auto files = resource::filesystem(
    resource::file("/device/version.bin", versionFile)); // index 0
```

These objects are ready without an `init()` call. `BytesFile` borrows its
source; it does not copy or allocate it. A constexpr source and table can live
in Flash. Mutable provider state can live in RAM independently of the table.

`file()` returns an opaque immutable `FileEntry`. Its path, provider context
and operation table are private. Use the filesystem or its `FileView` for
public metadata and operations; copying an entry only copies the binding.

## Read a file locally

```cpp
std::array<std::byte, 32> output;
resource::Cursor cursor = 0;
const auto result = files.read(0, cursor, output);
if (result.status == resource::Status::Ok) {
    // Consume exactly output[0 .. result.written).
    cursor = result.next;
    // Stop at result.eof; otherwise read again with cursor.
}
```

Use `stat(index)` for size and capabilities, and `path(index)` for its name.
The file index is its position in the declaration, starting at zero. Reads do
not look up a string or walk directories. The core does one bounds check and
indexes the descriptor array.

## Iterate and access one file

Both `FileSystem` and `FileSystemView` provide `size()`, `empty()`, `begin()`,
`end()` and checked `operator[]`. `fileCount()` remains available. The iterator
returns a small borrowed `FileView`, which exposes the index and path without
exposing the provider pointer or operation table:

```cpp
const auto fs = files.view();
for (auto file : fs) {
    const auto index = file.index();
    const auto path = file.path();
    const bool canRead = file.readable();
    const bool canWrite = file.writable();
    const auto info = file.stat(); // Explicitly calls this provider's size().
    // Use index/path, capabilities and info.status/size/flags in the application.
}

auto file = fs[0];
auto result = file.read(0, output);
// file.write(cursor, input, final) uses the same contract as fs.write(...).
```

Enumeration and `readable()`/`writable()` call no `size()`, `read()` or `write()`
callback. The constexpr capability methods inspect declared callbacks only;
they do not establish that a provider is ready or that an operation will succeed.
Invalid indexing returns an invalid facade: `file.valid()` is false, its path is empty,
both capabilities are false, and operations report `InvalidFile`. The requested
index is retained.
`FileView` and iterators borrow the descriptor table directly, so a temporary
`FileSystemView` is safe when that table remains alive. They do not extend the
table/provider lifetimes. A temporary owning `FileSystem` cannot provide these
borrowed handles through direct indexing or iterator access.

## Add another file and choose its path

Create a stable provider object and add another `file(path, object)` entry:

```cpp
inline constexpr std::array calibrationBytes{
    std::byte{0x12}, std::byte{0x34}};
inline constexpr resource::BytesFile calibrationFile{calibrationBytes};

inline constinit const auto deviceFiles = resource::filesystem(
    resource::file("/device/version.bin", versionFile),       // index 0
    resource::file("/calibration/reference.bin", calibrationFile)); // index 1
```

There is no separate path registration. `/calibration/reference.bin` is a
flat label. The client may display it as folders by splitting `/`; the MCU
core never creates those folders. Extensions do not select an encoder:
`.bin`, `.json` or `.txt` mean whatever bytes the provider produces.

Paths must start with `/`, have nonempty components, and contain no ASCII
control bytes (`0x00..0x1f`, `0x7f`), backslashes, `.` or `..` components, or
trailing slash. Duplicate paths are rejected during table construction.
Examples: `/settings.bin`, `/logs/status.txt`, `/telemetry/values.bin`.

Changing the order changes file indexes, so clients rediscover the table with
LIST after firmware changes. There is no `FileId` member in a descriptor.
The packet LIST format can carry a whole path of at most 65533 bytes; a path
larger than this is usable locally but not representable in that protocol.

## Supply your own file format or storage

For bytes already held in an array/span, use `BytesFile`. For generated data,
a flash partition, logger, SD-card file or custom write handling, use an
ordinary class with these exact signatures:

```cpp
class SettingsFile {
public:
    resource::FileSize size() const noexcept;
    resource::ReadResult read(resource::Cursor, resource::Output) const noexcept;
    resource::WriteResult write(resource::Cursor, resource::Input, bool final) noexcept;
};

inline SettingsFile settings;
inline constinit const auto settingsFiles = resource::filesystem(
    resource::file("/settings.bin", settings));
```

`write()` receives the file in pieces; the piece marked `final` is the end of
the file, and the provider decides what the end means - typically it applies
the bytes it collected. A provider that keeps the file across a power cut adds
`save()`: the client sends SAVE after the write, and the provider persists what
the file holds now. It may do so at once, or in steps for slow storage -
returning a moved cursor and `complete=false` until the bytes are really in
non-volatile memory, so the task that serves requests never blocks:

```cpp
class CalibrationFile {
public:
    resource::FileSize size() const noexcept;
    resource::ReadResult read(resource::Cursor, resource::Output) const noexcept;
    resource::WriteResult write(resource::Cursor, resource::Input, bool final) noexcept;
    resource::SaveResult save(resource::Cursor) noexcept;
};
```

A provider that writes in place may be left part old, part new by an
interrupted write; say so where it is documented.

`size()` is required and returns an exact u32 size. At least one of `read()`
or `write()` must exist. Omit `write()` for read-only files; omit `read()` for
write-only files. There is no virtual base class or registration macro.
`save()` is optional. It does not require `write()` - a file may keep what
changes in it by itself - but like every file it needs `read()` or `write()`.
A `save()` of another shape is a compile-time error, never a file that
silently does not keep its bytes. `file()` creates one immutable operation
table per provider type, and STAT computes Readable/Writable/Savable from the
callbacks that actually exist.

A complete, compilable read/write provider and a three-file table are in
[the resource example](../../examples/resources/README.md). The example is
checked by the generic resource runner and has no telemetry dependency.
Writes to a custom resource call that provider's `write()`. Writing a telemetry
Field or executing a Command/Service remains a separate Model operation; the
DescriptorFile and ValuesFile providers themselves are read-only.

## Library boundaries and build integration

```text
application objects / immutable byte arrays
        | file(path, provider)
        v
resource::FileSystem --> stat / read / write / save / path by u32 index
        |
        +-- resource::protocol::process --> LIST / STAT / READ / WRITE / SAVE packets
        |                                  caller supplies UART/TCP/framing
        |
        +-- optional resource::telemetry::v3 providers
            current Model --> descriptor.bin / live values.bin
```

```text
resource/
  Resource.hpp                   generic public umbrella
  Types.hpp                      spans, statuses, sizes, opaque cursor
  File.hpp, FileSystem.hpp       bindings and flat indexed table
  FileView.hpp                   lazy file facade and forward iterator
  BytesFile.hpp, ChunkWriter.hpp byte-array provider and output utility
  resource.pri                   one qmake manifest
  protocol/                      generic packet operations
```

The generic core depends only on the C++20 standard library. It has no
compiled sources, Qt, PFR, allocation, RTTI or virtual base class. Including
`Resource.hpp` does not select a packet protocol or a telemetry adapter.
Model providers live separately in `lib/integration/telemetry/v3`; the directory
version names their frozen wire format, not a retired Scalar model.

For qmake:

```qmake
CONFIG += c++20
include(path/to/lib/resource/resource.pri)
```

This manifest lists the generic headers and compiles `protocol/Protocol.cpp`
once. Enable telemetry providers explicitly through Integration:

```qmake
CONFIG += resource_telemetry
include(path/to/lib/integration/integration.pri)
```

`resource_telemetry` selects Descriptor/Values only. To compile the optional
RPC mailbox, select `CONFIG += resource_telemetry_rpc` before the Integration
include instead, or add both flags when both provider families are needed.
Integration includes the selected providers' Telemetry and Resource dependencies.
Descriptor/Values-only builds
do not compile `RpcMailbox.cpp`, independently of linker section removal.

Repeated includes add no duplicate sources. For another build system, add
`lib` to the include path. The generic core needs no source files; compile
`lib/resource/protocol/Protocol.cpp` for packet handling and
`lib/integration/telemetry/v3/detail/Values.cpp` for live telemetry values.
Link the Telemetry compiled adapter and ABI sources for that adapter. Compile
`lib/integration/telemetry/v3/RpcMailbox.cpp`
only for [RPC mailbox](../../examples/rpc_mailbox/README.md) consumers.
The generic Resource manifest does not select Telemetry or Integration.

## Ownership and lifetime

`FileSystem<N>` owns only the descriptor array. `view()` returns a borrowed,
non-template `FileSystemView` and rejects a temporary table. `path()` returns
an empty view for an invalid index; `stat/read/write` report `InvalidFile`.
`FileStat` includes status so an empty file is distinguishable from an invalid
index. Providers and their data are not copied.

`FileIndex` and `FileSize` are u32. `Cursor` is u64 because it is opaque provider
state, not file identity. `BytesFile` interprets it as a byte offset; another
provider may use a sequence/state token. The core neither increments it nor
compares it with `size()`.

The table, provider, path text and byte storage have independent lifetimes.
Keep all four alive at stable addresses for every operation and keep path
text unchanged while registered. String literals
are the easiest paths. An lvalue `std::string` is also usable, but must not be
moved, resized or destroyed while its descriptor is used.

`file()` rejects temporary providers and owning temporary path strings,
including explicit types and braced arguments. An explicit provider type may
add const or select a base class of the live object. It cannot introduce a
conversion temporary. Pass `std::ref(provider).get()` or explicitly dereference
a holder; conversion-proxy objects are not borrowed providers.
`{provider}` may bind the existing object; `{}`, `{Provider{}}` and conversion
wrappers are rejected. Braced paths accept literals, character pointers and
`string_view`, but not owning strings, including `{lvalueString}`.

Explicit `string_view` and span values are already borrowed views. Their
construction cannot prove that their backing storage survives. Likewise a
helper returning `const T&` may hide a temporary. The caller remains responsible
for these lifetimes. `BytesFile` rejects owning array temporaries passed
directly, and never changes its source; its read output may overlap the source.
A read that overlaps mutable source storage changes those bytes through the
caller-provided output, so applications needing an immutable snapshot keep
output separate.

## Transfer contract

- Cursor zero starts a transfer. Other values are defined by each provider;
  the core never compares them with file size or increments them.
- `ReadResult` returns status, next cursor, bytes written, and EOF. `WriteResult`
  returns status, next cursor, bytes consumed, and completion.
- On `Ok`, the byte count must not exceed the supplied span. An unfinished
  operation must consume/produce bytes or advance its cursor. Otherwise return
  a status such as `BufferTooSmall`; clients must check status and progress.
- On error, return the input cursor, zero bytes and false EOF/completion.
  Bytes already placed in the caller's output are not committed and must be
  discarded. Write providers must define their own handling of side effects.
- `final=true` marks the end of the submitted input stream. If a provider only
  consumes a prefix, the caller resubmits the unconsumed suffix with the returned
  cursor and the same final indication. Empty final input can finalize a stream.
  `complete` is the provider's acknowledgement, not a synonym for `final`.
- No repeated-write suppression exists. Every request reaches the provider.
  Synchronization, coherent snapshots and any persistent storage belong there.
- Spans passed to `read()`/`write()` are valid only during that synchronous
  operation; providers must not retain these operation spans. A provider may
  separately borrow stable source storage, as `BytesFile` does. Core and
  callbacks assume valid C++ spans and live objects.

`ChunkWriter` has `writeAtomic` (all bytes or none) and `writePartial` (what fits).
It owns no cursor and permits overlapping input/output spans.

See [resource tests](../../tests/resources/README.md), the
[packet protocol](protocol/README.md), and
[telemetry adapters](../integration/telemetry/v3/README.md).

The compact result ABI uses 16-byte ReadResult/WriteResult and 8-byte FileStat
on ARM32. Constructors take `{status, cursor, count, finished}` independently
of the in-memory member order. These types are not aggregates and use ordinary
constructor calls rather than designated initializers. The packet
representation is independent of this C++ layout.
