"""Core parser, immutable model, naming and output-ownership regressions.

Fixtures use explicit independent wire records and the frozen producer goldens;
the compatibility import is tested as an API bridge, not an independent oracle.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""

from dataclasses import FrozenInstanceError, replace
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.telemetry_codegen import load_schema
from tools.telemetry_codegen import cli
from tools.telemetry_codegen.naming import allocate, identifier, validate_namespace
from tools.telemetry_codegen.parser import InvalidDescriptor, Limits, fingerprint, parse

FIXTURES = ROOT / "tests/structured/descriptor"
sys.path.insert(0, str(FIXTURES))
import golden  # Explicit wire fixture builder; does not import C++ metadata.
sys.path.pop(0)


def descriptor(extra=(), groups=((), (), ())):
    """Build records directly, including valid names not present in C++ fixtures."""
    types = bytes.fromhex((FIXTURES / "empty.hex").read_text())[64:] + b"".join(extra)
    catalogs, endpoints, counts = [], [], []
    for category, entries_by_catalog in enumerate(groups, 1):
        first = 0
        for group, (name, entries) in enumerate(entries_by_catalog):
            catalogs.append(golden.record(2, bytes((category, 0, 0, 0))
                                          + golden.u32(group, first, len(entries)) + golden.text(name)))
            for local, entry in enumerate(entries):
                primary, name, *rest = entry
                payload = golden.u32(group << 16 | local, primary)
                if category == 1:
                    payload += bytes((rest[0] if rest else 1, 0, 0, 0))
                elif category == 3:
                    payload += golden.u32(rest[0])
                endpoints.append(golden.record(category + 2, payload + golden.text(name)))
            first += len(entries)
        counts.extend((len(entries_by_catalog), first))
    cat, ep = b"".join(catalogs), b"".join(endpoints)
    total = 64 + len(types) + len(cat) + len(ep)
    header = b"TDS3" + struct.pack("<4HIQ10I", 3, 0, 64, 0, total, 0, 12 + len(extra),
                                   *counts, 64, 64 + len(types), 64 + len(types) + len(cat))
    data = bytearray(header + types + cat + ep)
    struct.pack_into("<Q", data, 16, fingerprint(data))
    return bytes(data)


class CoreTests(unittest.TestCase):
    def test_frozen_producer_goldens_and_compatibility(self):
        compatibility = importlib.util.spec_from_file_location("compatibility_parser", FIXTURES / "parser.py")
        module = importlib.util.module_from_spec(compatibility)
        compatibility.loader.exec_module(module)
        self.assertIs(module.parse, parse)
        for name in ("mixed", "edge", "empty"):
            data = bytes.fromhex((FIXTURES / (name + ".hex")).read_text())
            self.assertEqual(data, golden.fixture(name))
            schema = load_schema(data)
            self.assertEqual(schema.fingerprint, parse(data)["fingerprint"])
            self.assertEqual(schema.descriptor_sha256, hashlib.sha256(data).hexdigest())
            self.assertEqual(tuple(item["id"] for item in schema.types), tuple(range(len(schema.types))))

    def test_schema_is_deeply_immutable_and_preserves_shared_identity(self):
        schema = load_schema(golden.fixture("mixed"))
        with self.assertRaises(FrozenInstanceError):
            schema.namespace = "other"
        with self.assertRaises(TypeError):
            schema.raw["types"][15]["members"][0] = (1, "changed")
        with self.assertRaises(TypeError):
            schema.types[15]["wire"] = 0
        aliases = [endpoint for endpoint in schema.endpoints
                   if endpoint.value_type == 15 or endpoint.request_type == 15]
        self.assertEqual(len(aliases), 3)
        self.assertEqual({endpoint.group for endpoint in aliases}, {"motor"})
        with self.assertRaises(FrozenInstanceError):
            aliases[0].id = 123

    def test_identifiers_all_scopes_and_duplicate_enum_labels(self):
        names = ["class", "a b", "a_b", "a_b_2", "Type_12", "id", "__proto__", "μ"]
        extra = [golden.struct_type(12, len(names), [(2, name) for name in names]),
                 golden.enum_type(13, 2, 1, [(0, "same"), (1, "same"), (2, "same_2")])]
        groups = ((("codec_detail", [(12, "CalibrateRequest"), (13, "class")]),),
                  (("codec_detail", [(12, "Calibrate")]),),
                  (("codec_detail", [(12, "Calibrate", 12)]),))
        schema = load_schema(descriptor(extra, groups))
        self.assertEqual({endpoint.group for endpoint in schema.endpoints}, {"codec_detail_value"})
        self.assertEqual(schema.member_names[12][5], "id")
        self.assertNotEqual(schema.member_names[12][4], "Type_12")
        self.assertEqual(len(set(schema.member_names[12])), len(names))
        self.assertEqual(schema.types[12]["members"][6][1], "__proto__")
        self.assertEqual(schema.enum_names[13], ("same", "same_3", "same_2"))
        aliases = [name for endpoint in schema.endpoints for name in
                   (endpoint.value_alias, endpoint.request_alias, endpoint.response_alias) if name]
        self.assertEqual(len(aliases), len(set(aliases)))
        self.assertEqual(schema.endpoints[0].value_alias, "CalibrateRequest")
        self.assertEqual(schema.endpoints[2].request_alias, "CalibrateRequest_2")

    def test_flat_catalog_names_and_normalized_group_collisions(self):
        data = descriptor(groups=((("a/b", [(2, "read")]), ("a b", [(2, "read")]),
                                   ("a_b_2", [(2, "read")])),
                                  (("a/b", [(0, "Reset")]),), ()))
        endpoints = load_schema(data).endpoints
        self.assertEqual([endpoint.group for endpoint in endpoints], ["a_b", "a_b_3", "a_b_2", "a_b"])
        self.assertEqual([endpoint.id for endpoint in endpoints], [0, 65536, 131072, 0])
        self.assertEqual([endpoint.category for endpoint in endpoints], [1, 1, 1, 2])

    def test_suffix_collisions_and_large_duplicate_scope(self):
        self.assertEqual(allocate(["x", "x", "x_2", "x_3"]), ("x", "x_4", "x_2", "x_3"))
        symbols = allocate(["same"] * 65536)
        self.assertEqual(len(set(symbols)), 65536)
        for name in ("_Reserved", "__proto__", "operator", "1abc", "a__b", "μ"):
            symbol = identifier(name)
            self.assertRegex(symbol, r"^[A-Za-z][A-Za-z0-9_]*$")
            self.assertNotIn("__", symbol)
        self.assertEqual(validate_namespace("company::device_api"), "company::device_api")
        for name in ("Number", "Math", "Object", "Array", "Uint8Array", "BigInt", "eval", "arguments", "std"):
            self.assertNotEqual(identifier(name), name)
        for name in ("INT8_MAX", "UINT_LEAST32_MAX", "NULL", "INFINITY", "Type_2", "Type_14"):
            self.assertNotEqual(identifier(name, protect_support=False), name)
        for name in ("", "class", "_Bad", "foo::::bar", "foo/bar", "foo::rpc", "name__part"):
            with self.assertRaises(ValueError):
                validate_namespace(name)

    def test_structural_mutations_are_rehashed(self):
        data = bytearray(golden.fixture("mixed"))
        row = parse(data)["types"][14]
        struct.pack_into("<I", data, row["offset"] + 24, 14)  # self member reference
        struct.pack_into("<Q", data, 16, fingerprint(data))
        with self.assertRaisesRegex(InvalidDescriptor, "backward type reference"):
            load_schema(bytes(data))
        with self.assertRaisesRegex(InvalidDescriptor, "invalid parser limits"):
            parse(golden.fixture("mixed"), replace(Limits(), types=4097))
        with self.assertRaisesRegex(InvalidDescriptor, "type budget"):
            parse(golden.fixture("mixed"), replace(Limits(), types=12))
        with self.assertRaises(TypeError):
            load_schema(bytearray(golden.fixture("mixed")))

    def test_render_manifest_is_deterministic_and_complete(self):
        schema = load_schema(golden.fixture("edge"))
        fake = SimpleNamespace(generate=lambda schema: {"cpp/Types.hpp": "// stable\n"},
                               generate_js=lambda schema: {"js/device.js": "export {};\n"})
        with patch.object(cli.importlib, "import_module", return_value=fake):
            first = cli.render(schema, "js,cpp")
            second = cli.render(schema, "cpp,js,cpp")
        self.assertEqual(first, second)
        manifest = json.loads(first[cli.MANIFEST])
        self.assertEqual(manifest["options"]["targets"], ["cpp", "js"])
        self.assertEqual(manifest["descriptor_wire_version"], [3, 0])
        self.assertIn("enum_entries", manifest["generated_symbol_mapping"]["types"][21])
        self.assertNotIn(str(ROOT), first[cli.MANIFEST].decode())
        self.assertEqual(manifest["generated_files"]["cpp/Types.hpp"],
                         hashlib.sha256(first["cpp/Types.hpp"]).hexdigest())
        with patch.object(cli.importlib, "import_module", return_value=SimpleNamespace(
                generate=lambda schema: {"../outside": "bad"})):
            with self.assertRaises(ValueError):
                cli.render(schema, "cpp")

    def test_cli_inspect_and_invalid_input_create_no_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            source, output = Path(temporary) / "descriptor.bin", Path(temporary) / "generated"
            source.write_bytes(golden.fixture("empty"))
            stdout = io.StringIO()
            with patch("sys.stdout", stdout):
                self.assertEqual(cli.main(["inspect", str(source)]), 0)
            self.assertEqual(json.loads(stdout.getvalue())["type_count"], 12)
            source.write_bytes(b"bad")
            with patch("sys.stderr", io.StringIO()):
                self.assertEqual(cli.main(["generate", str(source), "--output", str(output)]), 1)
            self.assertFalse(output.exists())

    def test_reserved_sdk_namespace_refused_before_publication(self):
        data = golden.fixture("edge")
        self.assertEqual(load_schema(data).namespace, "device_api")
        for namespace in ("telemetry::my_device", "company::telemetry"):
            self.assertEqual(load_schema(data, namespace).namespace, namespace)
            self.assertIn("cpp/" + namespace.replace("::", "/") + "/ClientBindings.hpp",
                          cli.render(load_schema(data, namespace), "cpp"))
        with self.assertRaisesRegex(ValueError, "namespace 'telemetry' is reserved by the native SDK"):
            load_schema(data, "telemetry")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "descriptor.bin"
            source.write_bytes(data)
            existing = root / "existing"
            existing.mkdir()
            (existing / "application.txt").write_bytes(b"keep this application\n")
            before = {path.relative_to(root).as_posix(): path.read_bytes()
                      for path in root.rglob("*") if path.is_file()}
            for output in (root / "fresh-parent/generated", existing):
                stderr = io.StringIO()
                with patch("sys.stderr", stderr), patch.object(cli, "publish") as publish:
                    self.assertEqual(cli.main(["generate", str(source), "--targets", "cpp",
                        "--namespace", "telemetry", "--output", str(output)]), 1)
                self.assertIn("namespace 'telemetry' is reserved by the native SDK", stderr.getvalue())
                publish.assert_not_called()
                self.assertFalse((root / "fresh-parent").exists())
                self.assertEqual({path.relative_to(root).as_posix(): path.read_bytes()
                                  for path in root.rglob("*") if path.is_file()}, before)

    def test_fetch_cli_writes_only_completed_download(self):
        from tools.telemetry_codegen import resource
        data = golden.fixture("mixed")
        # MagicMock provides the context-manager special methods on its type.
        from unittest.mock import MagicMock
        adapter = MagicMock()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "descriptor.bin"
            args = ["fetch", "--host", "127.0.0.1", "--port", "9999", "--output", str(output)]
            with patch.object(resource, "UdpExchange", return_value=adapter) as udp, \
                    patch.object(resource, "fetch", return_value=data) as download, \
                    patch("sys.stdout", io.StringIO()):
                self.assertEqual(cli.main(args), 0)
            udp.assert_called_once_with("127.0.0.1", 9999, 2.0)
            download.assert_called_once_with(adapter.__enter__.return_value, "/telemetry/descriptor.bin")
            self.assertEqual(output.read_bytes(), data)
            with patch.object(resource, "UdpExchange", return_value=adapter), \
                    patch.object(resource, "fetch", side_effect=resource.ResourceError("failed")), \
                    patch("sys.stderr", io.StringIO()):
                self.assertEqual(cli.main(args), 1)
            self.assertEqual(output.read_bytes(), data)


class PublicationTests(unittest.TestCase):
    def test_unreadable_application_directory_refuses_before_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "out"
            first = self.files()
            cli.publish(first, output)
            private = output / "application"
            private.mkdir()
            canary = private / "keep.txt"
            canary.write_text("application data")
            real_scandir = os.scandir
            def denied(path):
                if str(path) == str(private):
                    raise PermissionError("application directory is unreadable")
                return real_scandir(path)
            with patch.object(cli.os, "scandir", side_effect=denied):
                with self.assertRaisesRegex(PermissionError, "unreadable"):
                    cli.publish(self.files(text="updated"), output)
            self.assertEqual(canary.read_text(), "application data")
            self.assertEqual((output / "js/device.js").read_bytes(), first["js/device.js"])
            self.assertEqual((output / cli.MANIFEST).read_bytes(), first[cli.MANIFEST])
            self.assertEqual(sorted(path.name for path in Path(temporary).iterdir()), ["out"])

    def test_legacy_windows_junction_detection(self):
        # Python 3.10/3.11 expose lstat reparse tags but no Path.is_junction().
        def legacy(tag=0, error=None):
            def lstat():
                if error is not None:
                    raise error
                return SimpleNamespace(st_reparse_tag=tag)
            return SimpleNamespace(is_symlink=lambda: False, lstat=lstat)

        self.assertTrue(cli._linked(legacy(0xa0000003)))
        self.assertFalse(cli._linked(legacy()))
        self.assertFalse(cli._linked(legacy(0xa000000c)))
        self.assertFalse(cli._linked(legacy(error=FileNotFoundError())))
        with self.assertRaises(PermissionError):
            cli._linked(legacy(error=PermissionError()))

    @unittest.skipUnless(os.name == "nt", "Windows directory junction control")
    def test_actual_junction_is_refused_with_legacy_path_interface(self):
        import _winapi
        with tempfile.TemporaryDirectory() as temporary:
            target, junction = Path(temporary) / "target", Path(temporary) / "junction"
            target.mkdir()
            (target / "application.txt").write_text("keep")
            _winapi.CreateJunction(str(target), str(junction))
            try:
                original = cli._linked
                def legacy_linked(path):
                    return original(SimpleNamespace(is_symlink=path.is_symlink, lstat=path.lstat))
                self.assertTrue(legacy_linked(junction))
                with patch.object(cli, "_linked", side_effect=legacy_linked):
                    for output in (junction, junction / "generated"):
                        with self.assertRaisesRegex(ValueError, "symlinks or junctions"):
                            cli.publish(self.files(), output)
                    with self.assertRaisesRegex(ValueError, "symlinks or junctions"):
                        cli._inventory(Path(temporary))
                    with self.assertRaisesRegex(ValueError, "symlinks or junctions"):
                        cli._write_descriptor(golden.fixture("empty"), junction / "descriptor.bin")
                self.assertEqual((target / "application.txt").read_text(), "keep")
                self.assertEqual(sorted(path.name for path in target.iterdir()), ["application.txt"])
            finally:
                junction.rmdir()

    def files(self, path="js/device.js", text="first\n"):
        data = text.encode()
        manifest = {"format": cli.FORMAT,
                    "generated_files": {path: hashlib.sha256(data).hexdigest()}}
        return {path: data, cli.MANIFEST: (json.dumps(manifest, sort_keys=True) + "\n").encode()}

    def test_update_preserves_application_files_and_removes_only_stale_owned(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "out"
            first = self.files()
            cli.publish(first, output)
            (output / "notes.txt").write_text("application data")
            second = self.files("cpp/Types.hpp", "second\n")
            cli.publish(second, output)
            self.assertEqual((output / "notes.txt").read_text(), "application data")
            self.assertFalse((output / "js/device.js").exists())
            cli.publish(second, output, check=True)
            before = {path.relative_to(output).as_posix(): path.read_bytes()
                      for path in output.rglob("*") if path.is_file()}
            with self.assertRaisesRegex(ValueError, "out of date"):
                cli.publish(first, output, check=True)
            self.assertEqual(before, {path.relative_to(output).as_posix(): path.read_bytes()
                                     for path in output.rglob("*") if path.is_file()})

    def test_modified_and_unowned_files_are_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "out"
            output.mkdir()
            (output / "user.txt").write_text("user")
            with self.assertRaisesRegex(ValueError, "unowned"):
                cli.publish(self.files("user.txt"), output)
            self.assertEqual((output / "user.txt").read_text(), "user")
            cli.publish(self.files(), output)
            (output / "js/device.js").write_text("edited")
            with self.assertRaisesRegex(ValueError, "modified"):
                cli.publish(self.files(text="new"), output)
            self.assertEqual((output / "js/device.js").read_text(), "edited")

    def test_directory_publication_rolls_back_on_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "out"
            first, second = self.files(), self.files(text="second")
            cli.publish(first, output)
            real_replace = cli.os.replace
            calls = 0

            def failing_replace(source, destination):
                nonlocal calls
                calls += 1
                if calls == 2:
                    raise OSError("simulated publication failure")
                return real_replace(source, destination)

            with patch.object(cli.os, "replace", side_effect=failing_replace):
                with self.assertRaisesRegex(OSError, "simulated"):
                    cli.publish(second, output)
            cli.publish(first, output, check=True)
            self.assertEqual(sorted(path.name for path in Path(temporary).iterdir()), ["out"])

    def test_application_directory_case_alias_refuses_before_publication(self):
        for application, generated in (("JS/application.txt", "js/device.js"),
                                       ("app/Cache/notes.txt", "app/cache/device.js"),
                                       ("JS", "js/device.js")):
            with self.subTest(application=application), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "out"
                first = self.files("old/owned.js")
                cli.publish(first, output)
                canary = output / application
                canary.parent.mkdir(parents=True, exist_ok=True)
                canary.write_bytes(b"application data\x00\xff")
                before = {path.relative_to(output).as_posix(): path.read_bytes()
                          for path in output.rglob("*") if path.is_file()}
                for check in (False, True):
                    with self.assertRaisesRegex(ValueError, "case-colliding"):
                        cli.publish(self.files(generated), output, check=check)
                    self.assertEqual(before, {path.relative_to(output).as_posix(): path.read_bytes()
                                             for path in output.rglob("*") if path.is_file()})
                    self.assertEqual(sorted(path.name for path in Path(temporary).iterdir()), ["out"])
                cli.publish(first, output, check=True)

    def test_planned_directory_aliases_and_file_prefixes_are_refused(self):
        for paths in (("js/a.js", "JS/b.js"), ("js", "js/device.js"),
                      ("manifest.json/extra.js",)):
            with self.subTest(paths=paths), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "out"
                files = {path: b"generated\n" for path in paths}
                files[cli.MANIFEST] = json.dumps({"format": cli.FORMAT, "generated_files":
                    {path: hashlib.sha256(data).hexdigest() for path, data in files.items()}}).encode()
                with self.assertRaisesRegex(ValueError, "case-colliding|file/directory"):
                    cli.publish(files, output)
                self.assertFalse(output.exists())
                self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_stale_owned_directory_case_can_change_without_application_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "out"
            cli.publish(self.files("JS/old.js"), output)
            second = self.files("js/device.js")
            cli.publish(second, output)
            cli.publish(second, output, check=True)
            self.assertEqual(sorted(path.relative_to(output).as_posix()
                                    for path in output.rglob("*") if path.is_file()),
                             ["js/device.js", cli.MANIFEST])

    def test_unsafe_paths_and_symlinks(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "out"
            for path in ("../outside", "/absolute", "C:/outside", "a/../outside", "a\\outside"):
                with self.assertRaises(ValueError):
                    cli.publish(self.files(path), output)
            outside = Path(temporary) / "outside"
            outside.mkdir()
            try:
                output.symlink_to(outside, target_is_directory=True)
            except OSError:
                self.skipTest("symlink creation unavailable for this user")
            with self.assertRaisesRegex(ValueError, "symlink"):
                cli.publish(self.files(), output)


if __name__ == "__main__":
    unittest.main()
