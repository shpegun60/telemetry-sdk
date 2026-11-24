"""Focused web emitter invariants; runtime parity lives in Check.mjs.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.telemetry_codegen.generators.web import generate_js, generate_ts, _reachable_types
from tools.telemetry_codegen.parser import fingerprint
from tools.telemetry_codegen.schema import load_schema


def hostile_descriptor():
    """Independent wire fixture with literal member keys and colliding symbols."""
    def u32(*values):
        return struct.pack("<" + "I" * len(values), *values)

    def text(value):
        data = value.encode("utf8")
        return u32(len(data)) + data

    def record(kind, payload):
        return struct.pack("<BBHI", kind, 1, 0, len(payload)) + payload

    def type_record(index, kind, wire, tail=b""):
        return record(1, u32(index) + bytes((kind, 0, 0, 0)) + u32(wire) + tail)

    rows = [type_record(0, 0, 0)]
    for code, size in enumerate((1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8), 1):
        rows.append(type_record(code, 1, size, bytes((code, 0, 0, 0))))
    keys = ("__proto__", "constructor", "a-b", "a b", "quote\"\\\n", "\u2028\u2029", "\U0001f525")
    rows.append(type_record(12, 3, len(keys), u32(len(keys)) + b"".join(u32(2) + text(key) for key in keys)))
    labels = ("__proto__", "constructor", "a-b", "a b", "a_b_2")
    rows.append(type_record(13, 2, 8, u32(9, len(labels)) + b"".join(
        struct.pack("<q", value) + text(label) for value, label in zip((-1, 0, 1, 2, 3), labels))))
    rows.append(type_record(14, 4, 14, u32(12, 2)))
    # Group names target support/global collisions as well as Unicode normalization.
    names = ("codecs", "Operation", "Type_12", "\u03bc", "a-b", "a b", "a_b_2",
             "Object", "Array", "Number", "Math", "ArrayBuffer", "DataView",
             "Uint8Array", "Error", "Record", "Readonly", "ReadonlyArray", "ArrayBufferView")
    catalogs, endpoints = [], []
    for index, name in enumerate(names):
        catalogs.append(record(2, bytes((1, 0, 0, 0)) + u32(index, index, 1) + text(name)))
        endpoints.append(record(3, u32(index << 16, 12 if index % 2 == 0 else 13) + bytes((3, 0, 0, 0)) + text(name)))
    type_bytes, catalog_bytes, endpoint_bytes = b"".join(rows), b"".join(catalogs), b"".join(endpoints)
    total = 64 + len(type_bytes) + len(catalog_bytes) + len(endpoint_bytes)
    result = bytearray(b"TDS3" + struct.pack("<4HIQ10I", 3, 0, 64, 0, total, 0,
        len(rows), len(names), len(names), 0, 0, 0, 0, 64,
        64 + len(type_bytes), 64 + len(type_bytes) + len(catalog_bytes)) +
        type_bytes + catalog_bytes + endpoint_bytes)
    struct.pack_into("<Q", result, 16, fingerprint(result))
    return bytes(result)


def consumer_source(schema):
    """Typecheck declarations against the emitted TS, including public aliases."""
    lines = ["import * as J from './js/device.js';", "import * as T from './ts/device.js';",
             "const codecs: typeof J.codecs = T.codecs;",
             "const enums: typeof J.enums = T.enums;",
             "const missingTS: Record<Exclude<keyof typeof J, keyof typeof T>, never> = {};",
             "const missingJS: Record<Exclude<keyof typeof T, keyof typeof J>, never> = {};"]
    for row in _reachable_types(schema):
        name = "Type_" + str(row["id"])
        lines += [f"const toTS_{name} = (value: J.{name}): T.{name} => value;",
                  f"const toJS_{name} = (value: T.{name}): J.{name} => value;"]
    for endpoint in schema.endpoints:
        group = endpoint.group.replace("::", ".")  # a group tree's group is a nested path
        for alias in (endpoint.value_alias, endpoint.request_alias, endpoint.response_alias):
            if alias:
                path = group + ".types." + alias
                lines += [f"const alias_{len(lines)}: typeof J.{path} = T.{path};",
                          f"const aliasType_{len(lines)} = (value: J.{path}): T.{path} => value;"]
    # v3.1: the named types scope, and in a tree each endpoint beside its subgroups.
    for row in _reachable_types(schema):
        symbol = schema.type_symbol(row["id"])
        if symbol:
            lines += [f"const named_{len(lines)}: typeof J.{symbol.js} = T.{symbol.js};",
                      f"const namedType_{len(lines)} = (value: J.{symbol.js}): T.{symbol.js} => value;",
                      f"const namedId_{len(lines)} = (value: T.{symbol.js}): T.Type_{row['id']} => value;"]
    for endpoint, symbol in zip(schema.endpoints, schema.tree_symbols):
        path = endpoint.group.replace("::", ".") + "." + symbol
        lines.append(f"const tree_{len(lines)}: typeof J.{path} = T.{path};")
    return "\n".join(lines) + "\n"


class WebGenerationTests(unittest.TestCase):
    def test_optional_async_client_preserves_standalone_codecs(self):
        from tools.telemetry_codegen.cli import render
        data = bytes.fromhex((ROOT / "tests/structured/descriptor/mixed.hex").read_text())
        schema = load_schema(data)
        plain = render(schema, ("js", "ts"))
        optional = render(schema, ("js", "ts"), client_runtime=True)
        for path, content in plain.items():
            if path != "manifest.json":
                self.assertEqual(optional[path], content)
        self.assertEqual(set(optional) - set(plain),
                         {"js/client.js", "js/client.d.ts", "ts/client.ts", "js/handles.js",
                          "js/handles.d.ts", "ts/handles.ts"})
        self.assertNotIn("import ", optional["js/device.js"].decode())
        self.assertNotIn("import ", optional["js/client.js"].decode())
        with self.assertRaisesRegex(ValueError, "requires a js or ts"):
            render(schema, ("cpp",), client_runtime=True)

    def test_determinism_and_output_contract(self):
        for name in ("empty", "mixed", "edge"):
            data = bytes.fromhex((ROOT / "tests/structured/descriptor" / (name + ".hex")).read_text())
            schema = load_schema(data)
            self.assertEqual(generate_js(schema), generate_js(load_schema(data)))
            self.assertEqual(generate_ts(schema), generate_ts(load_schema(data)))
            self.assertEqual(set(generate_js(schema)), {"js/device.js", "js/device.d.ts"})
            self.assertEqual(set(generate_ts(schema)), {"ts/device.ts"})
            if name == "empty":
                self.assertNotIn("function _encode_1(", generate_js(schema)["js/device.js"])
                self.assertNotIn("export type Type_1 ", generate_ts(schema)["ts/device.ts"])

    def test_static_runtime_and_literal_keys(self):
        schema = load_schema(hostile_descriptor())
        js = generate_js(schema)["js/device.js"]
        ts = generate_ts(schema)["ts/device.ts"]
        for source in (js, ts):
            for forbidden in ("eval(", "new Function(", "parseDescriptor(", "schema.types"):
                self.assertNotIn(forbidden, source)
            self.assertIn('value["__proto__"]', source)
            self.assertIn('result["__proto__"]', source)
            self.assertIn("Object.create(null)", source)
            self.assertIn("Object.hasOwn", source)
            self.assertIn("-1n", source)
            self.assertIn("Object.freeze", source)
        self.assertIn('"\\u2028\\u2029"', js)
        self.assertNotIn(" as const", js)
        self.assertIn(" as const", ts)


if __name__ == "__main__":
    unittest.main()
