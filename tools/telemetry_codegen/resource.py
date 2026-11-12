"""Bounded Descriptor download over complete Resource protocol packets.

The exchange callback owns transport framing, timeouts and peer identity. UDP
is provided because a datagram already supplies one complete packet; streams
need an application supplied framing adapter. No RPC dependency is involved.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""

from dataclasses import dataclass
import socket
import struct
from typing import Callable

from .parser import Limits, parse


class ResourceError(ValueError):
    """An invalid or unsuccessful Resource exchange."""


def _chunk(data):
    if not isinstance(data, bytes) or len(data) < 12:
        raise ResourceError("short Resource chunk reply")
    status, cursor, eof, count = struct.unpack_from("<BQBH", data)
    if status > 8 or eof > 1 or len(data) != 12 + count:
        raise ResourceError("invalid Resource chunk reply")
    if status:
        raise ResourceError(f"Resource status {status}")
    return cursor, bool(eof), data[12:]


def _paths(payload):
    result = []
    offset = 0
    while offset < len(payload):
        if len(payload) - offset < 2:
            raise ResourceError("short LIST path length")
        length = struct.unpack_from("<H", payload, offset)[0]
        offset += 2
        if length < 2 or length > len(payload) - offset:
            raise ResourceError("invalid LIST path length")
        raw = payload[offset:offset + length]
        offset += length
        if (raw[:1] != b"/" or raw[-1:] == b"/" or b"\\" in raw or
                any(c < 32 or c == 127 for c in raw) or
                any(c in (b"", b".", b"..") for c in raw[1:].split(b"/"))):
            raise ResourceError("invalid LIST path")
        try:
            result.append(raw.decode("utf-8", errors="strict"))
        except UnicodeDecodeError as error:
            raise ResourceError("LIST path is not UTF-8") from error
    return result


def fetch(exchange: Callable[[bytes], bytes], path="/telemetry/descriptor.bin", *,
          max_bytes=Limits().descriptor_bytes, max_files=65536, max_packets=65536):
    """Find an exact LIST path, STAT it, READ it, then validate wire v3 fully.

    LIST cursors are file indexes. READ cursors are opaque and are only checked
    for progress/cycles; they need not equal accumulated byte counts. The final
    descriptor fingerprint is validated by the one production parser.
    """
    if type(max_bytes) is not int or not 64 <= max_bytes <= Limits().descriptor_bytes:
        raise ValueError("descriptor byte limit must tighten the production ceiling")
    if (type(max_files) is not int or type(max_packets) is not int
            or not 1 <= max_files <= 65536 or not 1 <= max_packets <= 65536):
        raise ValueError("invalid Resource traversal limit")
    budget = max_packets

    def request(packet):
        nonlocal budget
        if budget == 0:
            raise ResourceError("Resource packet limit exceeded")
        budget -= 1
        return exchange(packet)

    cursor, index, seen, selected = 0, 0, set(), None
    while True:
        if cursor in seen:
            raise ResourceError("cyclic LIST cursor")
        seen.add(cursor)
        next_cursor, eof, payload = _chunk(request(struct.pack("<BQ", 1, cursor)))
        paths = _paths(payload)
        if next_cursor != index + len(paths) or next_cursor > max_files:
            raise ResourceError("LIST cursor/count mismatch or file limit exceeded")
        for offset, candidate in enumerate(paths):
            if candidate == path:
                if selected is not None:
                    raise ResourceError("ambiguous descriptor path")
                selected = index + offset
        index = next_cursor
        if eof:
            break
        if not paths or next_cursor == cursor:
            raise ResourceError("LIST made no progress")
        cursor = next_cursor
    if selected is None:
        raise ResourceError(f"descriptor path not found: {path}")
    stat = request(struct.pack("<BI", 2, selected))
    if not isinstance(stat, bytes) or len(stat) != 6:
        raise ResourceError("invalid STAT reply")
    status, size, flags = struct.unpack("<BIB", stat)
    if status or flags > 3 or not flags & 1 or not 64 <= size <= max_bytes:
        raise ResourceError("descriptor is unreadable or exceeds the byte limit")
    result, seen, cursor = bytearray(), set(), 0
    while True:
        if cursor in seen:
            raise ResourceError("cyclic READ cursor")
        seen.add(cursor)
        next_cursor, eof, payload = _chunk(request(struct.pack("<BIQ", 3, selected, cursor)))
        if len(payload) > size - len(result):
            raise ResourceError("READ exceeds the advertised descriptor size")
        result.extend(payload)
        if eof:
            if len(result) != size:
                raise ResourceError("READ ended before the advertised descriptor size")
            break
        if not payload or next_cursor == cursor:
            raise ResourceError("READ made no progress")
        cursor = next_cursor
    data = bytes(result)
    parse(data)
    return data


@dataclass
class UdpExchange:
    """Connected datagram adapter; one request and one reply per exchange.

    The peer must implement Resource packets directly in UDP datagrams. This
    does not add retransmission or authentication and is unsuitable when that
    peer uses another framing protocol. Use as a context manager.
    """

    host: str
    port: int
    timeout: float = 2.0

    def __enter__(self):
        if not 1 <= self.port <= 65535 or not 0 < self.timeout <= 300:
            raise ValueError("invalid UDP port or timeout")
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.socket.settimeout(self.timeout)
            self.socket.connect((self.host, self.port))
        except BaseException:
            self.socket.close()
            raise
        return self

    def __call__(self, request):
        self.socket.send(request)
        # This exceeds the maximum IPv4 datagram payload, including its reply
        # header, so recv cannot silently truncate a valid UDP packet.
        return self.socket.recv(65548)

    def __exit__(self, *_):
        self.socket.close()
