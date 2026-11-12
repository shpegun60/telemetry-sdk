# Inspector serial host checks

The Linux tests open only host-created PTYs. A real `QSerialPort` slave exchanges
COBS frames with a PTY master using the existing COBS endpoint, CRC16 policy,
Resource protocol and native demonstration Model. No physical serial port is
enumerated or opened by these checks, and no board is programmed.

```sh
python3 -B tests/inspector_serial/run.py --build-dir /external/inspector-serial \
  --ux-root /path/to/telemetry-ux --sdk-prefix /path/to/sdk-prefix \
  --qt-prefix /usr --cobs-root /path/to/COBS --cxx /usr/bin/g++
```

It needs Qt 6.12 LTS or newer with SerialPort, Network, Quick Controls and Test. COBS remains
an explicitly selected external checkout for the test peer. The GUI comes from
Telemetry UX and consumes the installed SDK; the peer uses the actual SDK NUCLEO
Protocol and private serial profile. UX sources, SDK fixtures, installed SDK
metadata/bytes and COBS inputs have separate retained identities. The fresh
external build keeps logs, binaries and `summary.json` outside all source trees.

The separate Watchlist executable uses the actual optional NUCLEO ReadMany
Protocol over fragmented PTYs. It checks TRM1 discovery and fingerprint/session
admission, 32 independent Fields, per-item Unavailable, malformed item IDs,
timeout after proven handoff, and recovery through a strictly newer session.
No test automatically falls back to individual reads or replays a timed-out batch.

Coverage includes paginated Descriptor discovery after a decoy file, fragmented
COBS deliveries, bad CRC followed by a correct response without replaying the
request, real FieldReads, exact U64/S64 text, unknown enum values, nested/empty
structures, the explicit 2,016-byte Encoded response payload limit, correlated
`Unavailable`, timeout recovery, and refusal of a short Qt write without repeating
its prefix. Old LIST/STAT/READ responses cannot alone establish a fresh complete
Descriptor transcript. Complete-frame session/request IDs are never relabeled.
Discovery also rejects a complete Resource frame followed by an unfinished COBS
tail before sending the next request. Closing and reconnecting the same transport
invalidates queued notifications. Peer identity is captured at open, remains
stable through close, and changes when another PTY is opened.

The maintained headless checker runs as a separate application against the actual
Nucleo demonstration `Protocol.hpp` over the same PTY framing. All four Fields
must pass, including exact U64, three float32 voltages, nested arrays, bool and
unknown signed enum. Three additional processes alter Ua, Ub or Uc; every one
must fail with exit 9 and a negative JSON report. Logs and reports are retained
under the external `device-check` directory. No physical port is involved.

The 2,048-byte application body uses two length bytes and CRC16/Modbus through
`FrameFormat`. Neither UART routing nor a new Telemetry header is added. The
existing SerialAdapter owns partial-write refusal and the COBS endpoint owns all
framing, CRC checking, queues and pool storage.

On Windows the same target compiles and checks the unopened transport lifecycle;
PTY scenarios are explicitly marked unavailable. A separately built matching
QtSerialPort module may be supplied with `--serial-prefix`. This is not a Windows
COM or hardware qualification. `--sanitize --cxx clang++-18` adds fatal ASan/UBSan
on Linux; Qt offscreen process-global allocations exclude LeakSanitizer coverage.
