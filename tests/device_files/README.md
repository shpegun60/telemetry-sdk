# Installed device files

Reading and writing the device's files through the client's one queue, against
the relocated installed package. No board; only ephemeral localhost sockets.

- **Reader** (`Reader.cpp`, no Qt, no network). `files::FileReader` and the
  `files::list` / `files::stat` requests run against the real Resource protocol
  handler (`resource::protocol::process`) over an in-memory file table. Every
  ending of a read is reached on purpose:
  - complete, including an empty file and a log that grows past its Stat size;
  - the device refusing (an unknown index), a write-only file, a reply that is
    not a reply, a piece without progress from a misbehaving device;
  - the sink stopping, a timeout, a cancel while a piece is in flight and between
    pieces, the queue cancelling, a busy queue that is retried and a refusal.
- **Writer** (`Writer.cpp`, no Qt, no network). `files::FileWriter`,
  `files::write` and `files::save` against the real protocol handler over
  writable providers, some of them savable. Every ending of a write is reached on
  purpose:
  - complete, in pieces of the chunk size with `final` on the last only and no
    SAVE sent, a smaller chunk, and an empty file (one empty final piece);
  - a device that takes part of each piece: the rest sent from the cursor it
    returned, still final when it was;
  - not writable, another size, the content refused, a reply that is not one, a
    device that says complete before every byte, one that never completes (empty
    final pieces, a bounded number), one that moves its cursor without ever taking
    a byte (4096 replies, then Stalled), a timeout that ends it with nothing sent
    again;
  - a cancel between pieces (the provider's staged pieces leave the file as it
    was, a write from the start replaces them) and while a piece waits in the
    queue;
  - a busy queue retried, a refusal, a second start while running, a piece built in
    a scratch too small;
  - saving as a run of its own after the write, Stat first: a provider that
    persists at once (one SAVE) and one that persists in three steps (SAVE
    repeated from the returned cursor until complete), a save the provider
    refuses, a file that cannot save (NotWritable by its Stat, no SAVE sent), a
    save that never completes (4096 steps, then Stalled), a reply that is not
    one, the one-byte InvalidData, a timeout and a cancel in the middle of a
    save, and one `files::save` alone.
- **Values** (`Values.cpp`, no Qt, no network). `/telemetry/values.bin` as the
  device serves it (`ValuesFile`, read in pieces) is read back on the host with
  `readValues` against a `RuntimeSchema` parsed from the descriptor's bytes, and
  every value through `DynamicValueView`: scalars, a nested struct, an enum by
  name, an array, a Field served Unavailable; read again after the device
  changed. Every wrong file is refused for its own reason with nothing visited:
  too short, magic, version, size, another descriptor, the Field count, tokens
  that do not add up either way, an unknown status, an Unavailable value that is
  not blank.
- **Session** (`Session.cpp`). A `ConnectionSession` over TCP to the native server:
  - the files are listed;
  - the descriptor and a 200000-byte file are read with a `FileReader` polled from
    the owner loop, with Field reads queued between its pieces;
  - the connection is cut in the middle of a read, which never ends as complete;
  - without a connection, the file requests are refused;
  - a device with the encoded profile and its own file after the profile's
    (`resource::filesystem(profile.systemFiles(), ...)`) connects as a telemetry
    session, not files only, lists the profile's four files then its own, and has
    its own file read byte for byte; a table that does not begin with the
    profile's files is refused by the Server.
  - the identity file (`wire/IdentityWire.hpp`, `IdentityFile`): its bytes as
    written and read, a rename, every refusal of the reader (magic, version,
    flags, size, texts, id) and of the writer (an id unfit for a folder name, a
    control character, a text too long, broken UTF-8), a later version's extra
    text skipped, a read in pieces;
  - the session reads it: already Reading when `ready()` turns true, then Known
    with the id, name, model and firmware; a rename reaches the next connection;
    without the file Absent; with another file under its name Unreadable with
    the reason, the connection still ready; disconnecting forgets it.
- **Files only** (`FilesOnly.cpp`). Devices whose files are readable but whose
  descriptor is mistyped in its name, too small, or read whole and rejected by the
  parser connect files only: `ready()` false, telemetry refused, the limitation
  named, the listed paths kept, and their files - the broken descriptor included -
  read byte for byte. A device that answers with bytes that are not the protocol is
  a plain failure with no files.

```sh
python3 -B tests/device_files/run.py --build-dir /external/device-files \
  --qt-prefix /path/to/qt --cxx g++
```

GCC 13 at -O3 reports the always-emplaced `std::optional` inside the ReadMany
Workbench completion as maybe-uninitialized once `Session.cpp` inlines it; the same
code builds clean in `tests/qt_read_many`. The session target keeps that warning
visible but not fatal.
