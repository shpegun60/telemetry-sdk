"""Complete-packet Descriptor retrieval and bounded failure-path regressions.

The fake exchanges explicit Resource packets and independent frozen Descriptor
bytes. UDP lifecycle tests mock sockets; no network or hardware is accessed.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""

from pathlib import Path
import socket
import struct
import sys
import unittest
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.telemetry_codegen.parser import InvalidDescriptor
from tools.telemetry_codegen.resource import ResourceError, UdpExchange, fetch


def chunk(cursor, eof, payload=b"", status=0):
    return struct.pack("<BQBH", status, cursor, int(eof), len(payload)) + payload


def paths(*names):
    return b"".join(struct.pack("<H", len(name.encode("utf-8"))) + name.encode("utf-8") for name in names)


class FakeResource:
    """Two LIST pages and three READ chunks with deliberately opaque cursors."""

    def __init__(self, data):
        self.data = data
        self.requests = []
        self.transform = lambda packet, response: response

    def __call__(self, packet):
        self.requests.append(packet)
        operation = packet[0]
        if operation == 1:
            _, cursor = struct.unpack("<BQ", packet)
            if cursor == 0:
                result = chunk(1, False, paths("/first"))
            elif cursor == 1:
                result = chunk(3, True, paths("/telemetry/descriptor.bin", "/last"))
            else:
                raise AssertionError("unexpected LIST cursor")
        elif operation == 2:
            _, index = struct.unpack("<BI", packet)
            if index != 1:
                raise AssertionError("wrong file index")
            result = struct.pack("<BIB", 0, len(self.data), 1)
        elif operation == 3:
            _, index, cursor = struct.unpack("<BIQ", packet)
            if index != 1:
                raise AssertionError("wrong file index")
            if cursor == 0:
                result = chunk(0x123456789ABC, False, self.data[:20])
            elif cursor == 0x123456789ABC:
                result = chunk(17, False, self.data[20:120])
            elif cursor == 17:
                result = chunk(17, True, self.data[120:])
            else:
                raise AssertionError("unexpected READ cursor")
        else:
            raise AssertionError("unexpected Resource operation")
        return self.transform(packet, result)


class ResourceTests(unittest.TestCase):
    def setUp(self):
        self.data = bytes.fromhex((ROOT / "tests/structured/descriptor/edge.hex").read_text())

    def test_exact_descriptor_pagination_and_opaque_read_cursor(self):
        resource = FakeResource(self.data)
        self.assertEqual(fetch(resource), self.data)
        self.assertEqual([packet[0] for packet in resource.requests], [1, 1, 2, 3, 3, 3])
        self.assertEqual([struct.unpack("<BIQ", packet)[2] for packet in resource.requests if packet[0] == 3],
                         [0, 0x123456789ABC, 17])
        self.assertEqual(fetch(FakeResource(self.data), max_packets=6, max_files=3,
                               max_bytes=len(self.data)), self.data)

    def test_missing_ambiguous_paths_and_list_limits(self):
        with self.assertRaisesRegex(ResourceError, "not found"):
            fetch(FakeResource(self.data), "/missing")
        resource = FakeResource(self.data)
        resource.transform = lambda packet, response: (
            chunk(3, True, paths("/telemetry/descriptor.bin", "/telemetry/descriptor.bin"))
            if packet[0] == 1 and packet[-8:] == struct.pack("<Q", 1) else response)
        with self.assertRaisesRegex(ResourceError, "ambiguous"):
            fetch(resource)
        with self.assertRaisesRegex(ResourceError, "file limit"):
            fetch(FakeResource(self.data), max_files=2)
        with self.assertRaisesRegex(ResourceError, "packet limit"):
            fetch(FakeResource(self.data), max_packets=5)
        with self.assertRaisesRegex(ResourceError, "byte limit"):
            fetch(FakeResource(self.data), max_bytes=len(self.data) - 1)

    def test_noninteger_limits_cannot_bypass_packet_budget(self):
        for name in ("max_bytes", "max_files", "max_packets"):
            for value in (True, 1.5, "100", 0, -1, 1 << 32):
                resource = FakeResource(self.data)
                with self.subTest(name=name, value=value):
                    with self.assertRaises(ValueError):
                        fetch(resource, **{name: value})
                    self.assertEqual(resource.requests, [])

    def test_bad_chunk_envelopes_and_statuses(self):
        bad = [b"", b"\0" * 11, bytearray(chunk(0, True)), chunk(0, True) + b"extra",
               struct.pack("<BQBH", 0, 0, 2, 0), chunk(0, True, status=9),
               chunk(0, True, status=1), struct.pack("<BQBH", 0, 0, 1, 3) + b"x"]
        for response in bad:
            with self.subTest(response=response):
                with self.assertRaises(ResourceError):
                    fetch(lambda packet: response)

    def test_invalid_list_names_and_cursors(self):
        for name in ("relative", "/", "/a/", "/a//b", "/a/../b", "/a/./b", "/a\\b", "/a\0b", "/a\nb"):
            with self.subTest(name=name):
                with self.assertRaisesRegex(ResourceError, "LIST path"):
                    fetch(lambda packet: chunk(1, True, paths(name)))
        for payload in (b"\x01", b"\x05\0/a", b"\x02\0/\xff"):
            with self.assertRaises(ResourceError):
                fetch(lambda packet: chunk(1, True, payload))
        with self.assertRaisesRegex(ResourceError, "cursor/count"):
            fetch(lambda packet: chunk(7, True, paths("/telemetry/descriptor.bin")))
        with self.assertRaisesRegex(ResourceError, "no progress"):
            fetch(lambda packet: chunk(0, False))

    def test_invalid_stat_replies(self):
        for response in (b"", b"\0" * 5, b"\0" * 7,
                         struct.pack("<BIB", 1, len(self.data), 1),
                         struct.pack("<BIB", 0, len(self.data), 0),
                         struct.pack("<BIB", 0, len(self.data), 4),
                         struct.pack("<BIB", 0, 63, 1)):
            resource = FakeResource(self.data)
            resource.transform = lambda packet, valid, response=response: response if packet[0] == 2 else valid
            with self.subTest(response=response):
                with self.assertRaisesRegex(ResourceError, "STAT|unreadable"):
                    fetch(resource)

    def test_read_cycles_no_progress_and_size_mismatch(self):
        scenarios = (
            (lambda packet, valid: chunk(0, False, b"x") if packet[0] == 3 else valid, "no progress"),
            (lambda packet, valid: chunk(99, False) if packet[0] == 3 else valid, "no progress"),
            (lambda packet, valid: chunk(0, True, b"x") if packet[0] == 3 else valid, "ended before"),
            (lambda packet, valid: chunk(0, True, self.data + b"x") if packet[0] == 3 else valid, "exceeds"),
            (lambda packet, valid: chunk(0x123456789ABC, False, b"x")
             if packet[0] == 3 and struct.unpack("<BIQ", packet)[2] == 17 else valid, "cyclic READ"),
        )
        for transform, reason in scenarios:
            resource = FakeResource(self.data)
            resource.transform = transform
            with self.subTest(reason=reason):
                with self.assertRaisesRegex(ResourceError, reason):
                    fetch(resource)

    def test_retrieved_descriptor_is_fully_validated(self):
        data = bytearray(self.data)
        data[-1] ^= 1
        with self.assertRaisesRegex(InvalidDescriptor, "fingerprint"):
            fetch(FakeResource(bytes(data)))
        with self.assertRaisesRegex(OSError, "exchange failed"):
            fetch(lambda packet: (_ for _ in ()).throw(OSError("exchange failed")))


class UdpTests(unittest.TestCase):
    def test_connected_datagram_exchange_and_cleanup(self):
        fake = MagicMock()
        fake.recv.return_value = b"reply"
        with patch("tools.telemetry_codegen.resource.socket.socket", return_value=fake) as create:
            with UdpExchange("127.0.0.1", 1234, 0.5) as exchange:
                self.assertEqual(exchange(b"request"), b"reply")
        create.assert_called_once_with(socket.AF_INET, socket.SOCK_DGRAM)
        fake.connect.assert_called_once_with(("127.0.0.1", 1234))
        fake.send.assert_called_once_with(b"request")
        fake.recv.assert_called_once_with(65548)
        fake.close.assert_called_once()

    def test_socket_is_closed_when_connect_or_exchange_raises(self):
        for failure in ("connect", "recv"):
            fake = MagicMock()
            getattr(fake, failure).side_effect = OSError("failed")
            with patch("tools.telemetry_codegen.resource.socket.socket", return_value=fake):
                with self.assertRaises(OSError):
                    with UdpExchange("127.0.0.1", 1234) as exchange:
                        exchange(b"request")
            fake.close.assert_called_once()
        for port, timeout in ((0, 1), (65536, 1), (1234, 0), (1234, float("nan"))):
            with patch("tools.telemetry_codegen.resource.socket.socket") as create:
                with self.assertRaises(ValueError):
                    with UdpExchange("127.0.0.1", port, timeout):
                        pass
                create.assert_not_called()


if __name__ == "__main__":
    unittest.main()
