# Installed device files

Reading the device's files through the client's one queue, against the relocated
installed package. No board; only ephemeral localhost sockets.

- **Reader** (`Reader.cpp`, no Qt, no network). `files::FileReader` and the
  `files::list` / `files::stat` requests run against the real Resource protocol
  handler (`resource::protocol::process`) over an in-memory file table. Every
  ending of a read is reached on purpose:
  - complete, including an empty file and a log that grows past its Stat size;
  - the device refusing (an unknown index), a write-only file, a reply that is
    not a reply, a piece without progress from a misbehaving device;
  - the sink stopping, a timeout, a cancel while a piece is in flight and between
    pieces, the queue cancelling, a busy queue that is retried and a refusal.
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
