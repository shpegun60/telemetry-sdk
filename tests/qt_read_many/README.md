# Optional TCP/Qt ReadMany profile checks

`run.py` builds and installs `qt_connection` and `tcp_server`, relocates the
installation, and compiles two ordinary installed consumers. The tests require
Qt 6.12 or newer with Core/Network and the matching kit compiler; SerialPort,
UX and a board are absent.

```powershell
python -B tests/qt_read_many/run.py --build-dir C:/temp/qt-read-many-check `
  --qt-prefix C:/Qt/6.12.0/mingw_64 `
  --cxx C:/Qt/Tools/mingw1510_64/bin/g++.exe `
  --cmake C:/Qt/Tools/CMake_64/bin/cmake.exe `
  --ninja C:/Qt/Tools/Ninja/ninja.exe
```

Use the compiler supplied for the selected Qt kit and a fresh external directory.
Nothing is deleted or overwritten. On Linux, use the installed Qt/Clang tools
and optionally `--sanitize`. `summary.json`
records source hashes before/after, exact commands, log hashes and check counts.

`Native.cpp` tests the standard Resource cursor-zero session WRITE, malformed
and nonfinal requests, response-capacity preflight, repeated/changed sessions,
canonical/chunked TRM1 reads, session mismatch, getter isolation, freeze before a
reentrant getter, and replay rejection. `Session.cpp` uses an ephemeral localhost
server and the public Qt owner: bootstrap invokes no getter, optional pair
discovery fails closed if incomplete or schema-mismatched, legacy Descriptor-only
servers keep individual FieldRead, ReadMany and FieldRead share one FIFO, reconnect
advances identity, queued retirement completes once, and borrowed values remain
valid while a callback destroys its Session owner. It also checks that the
Session runs only when there is work: idle for 500 ms it publishes no
`progressed()`; a read submitted to the Workbench directly is written on the
next turn after `wake()` and by the 1 s heartbeat without it; behind a request
the peer never answers, waiting publishes nothing, a request with a 200 ms
deadline times out within the 100 ms backstop of it, and six queued requests
cancelled together all report `Cancelled` within 20 ms, in three rounds.

These tests establish local host contracts. They do not measure physical DMA,
firmware memory fit, sustained throughput, or hardware behavior.
