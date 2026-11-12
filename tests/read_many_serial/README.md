# Serial ReadMany checks

```sh
python3 -B tests/read_many_serial/run.py --build-dir /tmp/read-many-serial \
  --cobs-root /path/to/cobs --qt-prefix /path/to/Qt/6.12.0/gcc_64 --cxx clang++-18 --sanitize
```

The build directory must be fresh and outside the source tree. The runner keeps
compiler/runtime logs, binary hashes and before/after source hashes. It opens
only host-created PTYs; it never opens a hardware serial port or a programmer.

The real console executable exchanges fragmented COBS frames with the actual
opt-in board Protocol, using Resource bootstrap and the production Encoded
dispatcher. The successful run covers 32 individual reads, 8/12/16/32-field
batches, mixed per-item results, controlled deadlines, stale replies and a fresh
session after reconnect. Negative peers send invalid TRM1 bytes and a malformed
batch reply with a valid outer CRC; neither may report success. Bootstrap tests
separately exercise pagination, opaque/nonmonotonic READ cursors, cursor-only
progress and its finite budget, exact accumulated lengths at EOF, and malformed
Resource transcripts. These cursor tests use the same bootstrap class as TCP.

The sustained-load controls run the same CLI for two seconds over a real PTY,
check the bounded report, four batch-size aggregates, exact completion accounting
and the unchanged lifecycle after load. Another peer corrupts the ninth load
reply after eight validated exchanges; its requested duration is long enough
that this deterministic packet boundary, rather than a scheduling deadline,
causes failure. Partial counts must survive and no reconnect/replay may follow.
Both peers still use the actual board Protocol and production serial/COBS path.
Argument controls reject negative, excessive, malformed, missing and duplicated
`--load-seconds` values before any serial bootstrap. The report remains below
20,000 bytes rather than growing with exchange count.

The Windows build executes bootstrap and argument checks; Linux additionally
runs real PTYs. Physical TX lease ownership is separately tested by the board's
[host harness](../../examples/nucleo_h7s3l8/tests/Host.cpp), which holds UART TC
and COBS lease release independently. Physical qualification uses the guarded
[hardware runner](../nucleo_h7s3l8/README.md).
