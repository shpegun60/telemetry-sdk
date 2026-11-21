"""Descriptor v3.1 in the generator: type names, the group tree, attributes, v3.0 identity.

The goldens are the reviewed output of names.write_goldens(); every other file
is pinned by the golden manifest's hashes. v3.0 identity is pinned by the
frozen hashes of the UX extension suite, independently of this generator.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
import contextlib
import difflib
import hashlib
import io
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(Path(__file__).resolve().parent)]
import names  # noqa: E402
from names import golden  # noqa: E402
from tools.telemetry_codegen import cli  # noqa: E402
from tools.telemetry_codegen.cli import render  # noqa: E402
from tools.telemetry_codegen.generators.cpp import generate as generate_cpp  # noqa: E402
from tools.telemetry_codegen.schema import load_schema  # noqa: E402

LEGACY = ROOT / "tests/ux_codegen_extension/legacy_output_hashes.json"


def text(data):
    """Committed text files check out with CRLF on Windows; generated output is LF."""
    return data.replace(b"\r\n", b"\n")


class V30IdentityTests(unittest.TestCase):
    def test_v30_input_gives_the_frozen_output(self):
        """Every core profile of the three v3.0 goldens matches the frozen hashes of the group form."""
        baseline = json.loads(LEGACY.read_text())["profiles"]
        for fixture in ("empty", "mixed", "edge"):
            schema = load_schema(names.fixture(fixture))
            self.assertEqual(schema.minor, 0)
            for profile, options in (("core", False), ("core-options", True)):
                files = render(schema, ("cpp", "js", "ts"), client_runtime=options, structural_metadata=options)
                hashes = {path: hashlib.sha256(data).hexdigest() for path, data in files.items()}
                self.assertEqual(hashes, baseline[f"{fixture}-{profile}"]["generated"], fixture + "-" + profile)

    def test_v30_input_gets_the_group_form(self):
        """A v3.0 descriptor generates the group form beside its per-family symbols."""
        files = render(load_schema(names.fixture("mixed")), ("cpp", "js", "ts"), client_runtime=True)
        endpoints = files["cpp/device_api/Endpoints.hpp"].decode()
        self.assertIn("namespace motor {\ninline constexpr auto Enabled = fields::Enabled;", endpoints)
        self.assertIn("namespace motor::fields {", endpoints)  # kept until the old forms go
        declarations = files["js/device.d.ts"].decode()
        self.assertIn("export declare namespace motor {", declarations)
        self.assertIn("""  const Enabled: (typeof fields)["Enabled"];""", declarations)
        self.assertIn("readonly motor: Readonly<{", files["js/handles.d.ts"].decode())


class GoldenTests(unittest.TestCase):
    def test_named_flat_and_tree_goldens(self):
        for name in ("named", "tree"):
            files = names.golden_render(name)
            for path in names.GOLDEN_FILES:
                expected = text(names.golden_path(name, path).read_bytes())
                self.assertEqual(files[path], expected, f"{name}/{path}; refresh with names.py write-goldens")

    def test_named_definitions_keep_type_n(self):
        types = names.golden_render("named")["cpp/device_api/Types.hpp"].decode()
        self.assertIn("namespace fixture {\nenum class Mode : Type_2 {", types)
        self.assertIn("using Type_12 = fixture::Mode;", types)
        self.assertIn("using Type_15 = fixture::MotorConfig;", types)
        self.assertIn("using Type_13 = std::array<Type_4, 3>;", types)  # arrays are never named
        self.assertIn("namespace types {\nnamespace fixture {\nusing Mode = ::device_api::detail::fixture::Mode;", types)
        self.assertIn("namespace motor::types {\nusing Enabled = detail::Type_1;", types)

    def test_tree_nested_scopes_and_aliases(self):
        files = names.golden_render("tree")
        endpoints = files["cpp/device_api/Endpoints.hpp"].decode()
        self.assertIn("namespace adc::channel1::fields {\ninline constexpr endpoint::Field<detail::Type_10> Gain{65536,",
                      endpoints)
        self.assertIn("namespace adc::channel1 {\ninline constexpr auto Gain = fields::Gain;", endpoints)
        self.assertIn("namespace adc {\ninline constexpr auto Config = fields::Config;", endpoints)
        self.assertIn("inline constexpr auto Calibrate = services::Calibrate;", endpoints)
        declarations = files["js/device.d.ts"].decode()
        self.assertIn("export declare namespace adc {", declarations)
        self.assertIn("  namespace channel1 {", declarations)
        self.assertIn("""    const Gain: (typeof fields)["Gain"];""", declarations)
        self.assertIn("export declare namespace types {\n  namespace adc {\n    type Config = Type_12;", declarations)
        schema = json.loads(files["metadata/schema.json"])
        self.assertEqual([group["path"] for group in schema["groups"]],
                         ["adc", "adc/channel1", "adc/channel2", "metrology"])
        self.assertEqual(schema["types"][12]["name"], "adc::Config")
        self.assertEqual(schema["types"][16]["cppName"], "types::metrology::Config")
        self.assertEqual(schema["endpoints"][2]["path"], "adc/channel1/Gain")

    def test_exact_header_and_real_minor(self):
        for name in ("named", "tree", "attrs", "tree_unnamed", "flat_roots"):
            data = names.fixture(name)
            schema = load_schema(data)
            self.assertEqual(schema.minor, 1)
            self.assertEqual(schema.descriptor_header, data[:64], name)
            files = render(schema, ("cpp", "js", "ts"), structural_metadata=True)
            manifest = json.loads(files["manifest.json"])
            self.assertEqual(manifest["descriptor_wire_version"], [3, 1])
            header = ", ".join(f"std::byte{{0x{byte:02x}}}" for byte in data[:64])
            self.assertIn(header, files["cpp/device_api/Schema.hpp"].decode())
            self.assertIn("minor = 1;", files["cpp/device_api/Schema.hpp"].decode())
            self.assertIn("{major: 3, minor: 1}", files["js/device.js"].decode())
            self.assertIn("[" + ", ".join(map(str, data[:64])) + "]", files["js/device.js"].decode())


class SymbolTests(unittest.TestCase):
    def test_type_scope_symbols(self):
        """Support names stay (Status), keywords and the scope hazards are protected."""
        data = golden.fixture("mixed", names={12: "adc", 14: "adc::Status", 15: "std::class"})
        symbols = load_schema(data).type_symbols
        self.assertEqual((symbols[12].scope, symbols[12].symbol), ("", "adc"))
        self.assertEqual((symbols[14].scope, symbols[14].symbol), ("adc_2", "Status"))
        self.assertEqual((symbols[15].scope, symbols[15].symbol), ("std_2", "class_value"))
        self.assertEqual(symbols[15].cpp, "types::std_2::class_value")
        self.assertEqual(symbols[15].js, "types.std_2.class_value")
        data = golden.fixture("mixed", names={12: "codecs", 14: "Type_3", 15: "then"})
        self.assertEqual([load_schema(data).type_symbols[i].symbol for i in (12, 14, 15)],
                         ["codecs_2", "Type_3_value", "then_2"])
        types = generate_cpp(load_schema(data))["cpp/device_api/Types.hpp"]
        self.assertIn("struct Type_3_value {", types)
        self.assertIn("using Type_14 = Type_3_value;", types)

    def test_tree_sibling_symbols(self):
        """Subgroups and endpoints of one group share a scope; support names are protected."""
        groups = [("types", 0, [(10, "fields", 3), (10, "a b", 1)], [(0, "Status")], []),
                  ("a-b", 1, [(10, "x", 1)], [], []),
                  ("root", 0, [(10, "Gain", 3)], [], [])]
        schema = load_schema(golden.tree_fixture(groups=groups, names={}))
        self.assertEqual([group.qualified for group in schema.groups],
                         ["types_value", "types_value::a_b", "root"])
        self.assertEqual(schema.tree_symbols, ("fields_value", "a_b_2", "x", "Gain", "Status_value"))
        self.assertEqual([endpoint.group for endpoint in schema.endpoints],
                         ["types_value", "types_value", "types_value::a_b", "root", "types_value"])
        endpoints = generate_cpp(schema)["cpp/device_api/Endpoints.hpp"]
        self.assertIn("namespace types_value {\ninline constexpr auto fields_value = fields::fields_value;\n"
                      "inline constexpr auto a_b_2 = fields::a_b;", endpoints)


class HandleTests(unittest.TestCase):
    def test_handles_for_every_descriptor_with_the_client_runtime(self):
        for name in ("tree", "named", "mixed"):
            schema = load_schema(names.fixture(name))
            files = render(schema, ("cpp", "js", "ts"), client_runtime=True)
            self.assertIn("js/handles.js", files, name)
            self.assertIn("ts/handles.ts", files, name)
            self.assertIn("template<class Client> class Device {",
                          files["cpp/device_api/ClientBindings.hpp"].decode(), name)
            self.assertNotIn("js/handles.js", render(schema, ("js",)), name)

    def test_handle_tree_paths(self):
        from tools.telemetry_codegen.generators.handles import handle_tree
        roots, leaves = handle_tree(load_schema(names.fixture("tree")))
        self.assertEqual([symbol for symbol, _ in roots], ["adc", "metrology"])
        self.assertEqual([(leaf.path, leaf.access) for leaf in leaves][:3],
                         [("adc/Config", "adc.Config"), ("adc/Status", "adc.Status_value"),
                          ("adc/channel1/Gain", "adc.channel1.Gain")])
        # A catalog descriptor has the same form: one group per catalog name.
        roots, leaves = handle_tree(load_schema(names.fixture("named")))
        self.assertEqual(leaves[7].access, "motor.Config")
        self.assertEqual(leaves[10].access, "motor.Echo")
        # A path naming endpoints of two families has no path form; their
        # symbols take the kind, and the root group is not a device method.
        groups = ((("read", [(2, "Reset"), (2, "Mode")]),), (("read", [(0, "Reset")]),), ())
        from test_core import descriptor
        for minor in (0, 1):
            flat = bytearray(descriptor((golden.enum_type(12, 2, 1, [(0, "A")], name="Kind"),), groups))
            flat[6] = minor  # the name makes it v3.1; without it the input is v3.0
            if minor == 0:
                flat = bytearray(descriptor((golden.enum_type(12, 2, 1, [(0, "A")]),), groups))
            schema = load_schema(golden.rehash(bytes(flat)))
            self.assertEqual(schema.minor, minor)
            roots, leaves = handle_tree(schema)
            self.assertEqual([symbol for symbol, _ in roots], ["read_2"])
            self.assertEqual([(leaf.path, leaf.registered, leaf.access) for leaf in leaves],
                             [("read/Reset", False, "read_2.Reset_Field"), ("read/Mode", True, "read_2.Mode"),
                              ("read/Reset", False, "read_2.Reset_Command")])
            endpoints = generate_cpp(schema)["cpp/device_api/Endpoints.hpp"]
            self.assertIn("namespace read {\ninline constexpr auto Reset_Field = fields::Reset;\n"
                          "inline constexpr auto Mode = fields::Mode;\n"
                          "inline constexpr auto Reset_Command = commands::Reset;\n}", endpoints)


class AttributeTests(unittest.TestCase):
    def test_raw_attributes_and_inspect(self):
        schema = load_schema(names.fixture("attrs"))
        kept = [(item["record"], item["id"], item["tag"], item["value"]) for item in schema.attributes]
        self.assertIn(("type", 15, 0x40, b"app"), kept)
        self.assertIn(("catalog", 0, 0x3E, b"group"), kept)
        self.assertIn(("command", 0, 0x3E, b"c"), kept)
        self.assertEqual(schema.types[15]["attributes"][2], (0x40, b"app"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "attrs.bin"
            path.write_bytes(names.fixture("attrs"))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(cli.main(["inspect", str(path)]), 0)
        report = json.loads(output.getvalue())
        self.assertIn({"record": "type 15", "tag": "0x40", "class": "application", "value": "617070"},
                      report["attributes"])
        self.assertIn("motor/Config -> Field, 0x00000007, fixture::MotorConfig", report["endpoints"])
        self.assertIn("motor/Echo -> Service, 0x00000000, (fixture::MotorConfig) -> fixture::MotorConfig",
                      report["endpoints"])

    def test_inspect_lists_tree_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "tree.bin"
            path.write_bytes(names.fixture("tree"))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(cli.main(["inspect", str(path)]), 0)
        report = json.loads(output.getvalue())
        self.assertTrue(report["group_tree"])
        self.assertEqual(report["endpoints"], [
            "adc/Config -> Field, 0x00000000, adc::Config",
            "adc/Status -> Field, 0x00000001, adc::Status",
            "adc/channel1/Gain -> Field, 0x00010000, f32",
            "adc/channel2/Gain -> Field, 0x00020000, f32",
            "metrology/Config -> Field, 0x00030000, metrology::Config",
            "adc/Reset -> Command, 0x00000000, ()",
            "adc/Calibrate -> Service, 0x00000000, (adc::Reference) -> adc::Offset"])
        self.assertEqual(report["type_names"]["15"], "metrology::Window")

    def test_attributes_change_only_header_literals_and_refuse_bindings(self):
        """attrs.hex gives the host outputs of its stripped form (named.hex) except the
        descriptor identity literals; TelemetryBindings.hpp is refused, naming the tags."""
        attrs, named = names.fixture("attrs"), names.fixture("named")
        left = render(load_schema(attrs), ("cpp", "js", "ts"), structural_metadata=True)
        right = render(load_schema(named), ("cpp", "js", "ts"), structural_metadata=True)
        self.assertEqual(set(left), set(right))
        a, b = load_schema(attrs), load_schema(named)
        literals = [(a.descriptor_sha256, b.descriptor_sha256), (f"{a.fingerprint:016x}", f"{b.fingerprint:016x}"),
                    (str(a.fingerprint), str(b.fingerprint))]
        identity = re.compile(r"^[\s\d,\[\]]*$|descriptorHeader")
        for path in sorted(set(left) - {"manifest.json", "cpp/device_api/TelemetryBindings.hpp"}):
            mine = left[path].decode()
            for old, new in literals:
                mine = mine.replace(old, new)
            changed = [line for line in difflib.unified_diff(right[path].decode().splitlines(), mine.splitlines(),
                                                             lineterm="", n=0)
                       if line[:1] in "+-" and not line.startswith(("+++", "---"))]
            self.assertTrue(all(identity.search(line[1:]) for line in changed), path + "\n" + "\n".join(changed))
        refusal = left["cpp/device_api/TelemetryBindings.hpp"].decode()
        self.assertIn('#error "TelemetryBindings.hpp is refused: the descriptor carries attribute tags 0x3e '
                      '(SDK advisory, first on type 13), 0x3f (SDK advisory, first on type 12), 0x40 '
                      '(application, first on type 15)', refusal)
        self.assertIn("RpcBindings.hpp includes this file", refusal)
        self.assertIn("TypeReflection<::device_api::detail::Type_15>",
                      right["cpp/device_api/TelemetryBindings.hpp"].decode())


class SourceTests(unittest.TestCase):
    def test_rebuild_sources_mirror_the_tables(self):
        tree = names.rebuild_source(load_schema(names.fixture("tree")))
        self.assertIn("inline constexpr ts::Model model{group0, group3};", tree)
        self.assertIn("ts::service<&serviceWith<::device_api::detail::Type_17, ::device_api::detail::Type_18>>", tree)
        self.assertIn("::telemetry::descriptor::Descriptor{model}", tree)
        flat = names.rebuild_source(load_schema(names.fixture("edge")))
        self.assertIn("::telemetry::descriptor::withoutTypeNames(model)", flat)
        self.assertIn("ts::FieldTable<> table1_1{};", flat)
        flat, tree = names.root_twins()
        self.assertEqual([e.id for e in load_schema(flat).endpoints], [e.id for e in load_schema(tree).endpoints])


if __name__ == "__main__":
    unittest.main()
