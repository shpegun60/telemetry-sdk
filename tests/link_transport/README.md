# Installed link transport

`TcpTransport` is a `LinkTransport` over a `TcpLink` and the TCP profile's
`ProfileFramer`. This suite proves the split along three lines, each against
the relocated installed package, with no board and only ephemeral localhost
sockets:

- **Framer** (`Framer.cpp`, Qt-free, `Telemetry::tcp_link` only): encoding adds
  no wire bytes and refuses what does not fit; frames split to single bytes or
  joined back to back come out whole and in order; `expect()` is refused in the
  middle of a frame; bytes that are not a frame, an oversized header and
  `close()` all latch `Invalid` until `reset()`.
- **Channel** (`Channel.cpp`): `LinkTransport` over a link that is not a socket,
  an in-memory `QIODevice` answered by the native TCP profile server. Discovery
  with the optional ReadMany profile completes through one-byte writes and
  delivers the exact packed Descriptor; a link that reports its loss twice is
  reported once; a broken frame, a silent peer and a zero session give their
  errors with the medium's own name (`MEMORY`), and the transport ends locally
  quiescent each time.
- **Trace** (`Trace.cpp`): a `ConnectionSession` over TCP against a peer that
  answers, closes right after answering, answers with bytes that are not a
  frame, or never answers. Every change of the session's observable state and
  every operation outcome is written as one line. The trace runs three times,
  so a line that depends on timing fails as a difference between runs, and is
  compared with [trace.golden](trace.golden).

```sh
python3 -B tests/link_transport/run.py --build-dir /external/link-transport \
  --qt-prefix /path/to/qt --cxx g++
```

## Where the golden comes from

`trace.golden` was recorded with `--record` on `b5fa34f`, the last revision
before the split, from a checkout of that revision holding only `Trace.cpp`
and a `CMakeLists.txt` that built only the trace (the framer and channel tests
need modules that revision does not have). The refactored transport reproduces
it byte for byte. Record it again only on purpose, on a revision whose
transport behaviour is meant to be the new reference, and say why in the
commit.

The error texts in it are part of what is compared: `TCP exchange timed out`
and `Malformed or oversized TCP frame` come from `LinkTransport` with the medium
named by `TcpTransport`; `The remote host closed the connection` is Qt's own
text for the socket error the link reports first.
