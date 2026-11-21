"""Static, transport-independent JavaScript/TypeScript value codecs.

Both runtime targets are emitted from the same statements. Descriptor member
keys remain literal property keys; normalized names are only public symbols.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
from __future__ import annotations

import json


def _quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def _type_name(type_id: int) -> str:
    return f"Type_{type_id}"


def _integer_literal(value: int, big: bool) -> str:
    return str(value) + ("n" if big else "")


def _is_big(schema, type_id: int) -> bool:
    row = schema.types[type_id]
    if row["kind"] == 2:
        row = schema.types[row["underlying"]]
    return row["kind"] == 1 and row["code"] in (8, 9)


def _aliases(endpoint):
    for label, type_id in ((endpoint.value_alias, endpoint.value_type),
                           (endpoint.request_alias, endpoint.request_type),
                           (endpoint.response_alias, endpoint.response_type)):
        if label:
            yield label, type_id


def _reachable_types(schema):
    """Keep positional IDs while dropping definitions unreachable from endpoints."""
    pending = [0]
    for endpoint in schema.endpoints:
        pending += [endpoint.value_type, endpoint.request_type, endpoint.response_type]
    reached = set()
    while pending:
        type_id = pending.pop()
        if type_id in reached:
            continue
        reached.add(type_id)
        row = schema.types[type_id]
        if row["kind"] == 2:
            pending.append(row["underlying"])
        elif row["kind"] == 3:
            pending += [child for child, _ in row["members"]]
        elif row["kind"] == 4:
            pending.append(row["element"])
    return tuple(schema.types[type_id] for type_id in sorted(reached))


def _types(schema, wire_editing=False) -> list[str]:
    lines = [
        "export type ByteInput = ArrayBuffer | ArrayBufferView;",
        "export interface ValueCodec<T> {",
        "  readonly wireBytes: number;",
        "  encode(value: T): Uint8Array;",
        "  /** May write an aggregate prefix before a later member is rejected. */",
        "  encodeInto(value: T, output: ByteInput): void;",
        "  decode(input: ByteInput): T;",
        "}",
    ]
    if wire_editing:
        lines.insert(-1, "  encodePreserving(value: T, original: ByteInput): Uint8Array;")
    scalars = {1: "boolean", 8: "bigint", 9: "bigint"}
    for row in _reachable_types(schema):
        name = _type_name(row["id"])
        kind = row["kind"]
        if kind == 0:
            value = "undefined"
        elif kind == 1:
            value = scalars.get(row["code"], "number")
        elif kind == 2:
            value = _type_name(row["underlying"])
        elif kind == 4:
            value = (f"ReadonlyArray<{_type_name(row['element'])}> & "
                     f"{{ readonly length: {row['count']} }}")
        else:
            lines.append(f"export interface {name} {{")
            for child, member in row["members"]:
                lines.append(f"  {_quote(member)}: {_type_name(child)};")
            lines.append("}")
            continue
        lines.append(f"export type {name} = {value};")
    return lines


def _helpers(typed: bool) -> list[str]:
    a = lambda annotation: annotation if typed else ""
    return [
        "export class TelemetryCodecError extends Error {",
        f"  constructor(reason{a(': string')}) {{ super(reason); this.name = 'TelemetryCodecError'; }}",
        "}",
        f"function _require(condition{a(': unknown')}, reason{a(': string')}){a(': asserts condition')} {{",
        "  if (!condition) throw new TelemetryCodecError(reason);",
        "}",
        f"function _view(input{a(': ByteInput')}){a(': DataView')} {{",
        "  if (input instanceof DataView) return input;",
        "  if (input instanceof ArrayBuffer) return new DataView(input);",
        "  if (ArrayBuffer.isView(input)) return new DataView(input.buffer, input.byteOffset, input.byteLength);",
        "  throw new TelemetryCodecError('Expected an ArrayBuffer or byte view');",
        "}",
        f"function _object(value{a(': unknown')}, count{a(': number')}){a(': asserts value is Record<string, unknown>')} {{",
        "  _require(value !== null && typeof value === 'object' && !Array.isArray(value), 'Struct requires an object');",
        "  _require(Object.keys(value).length === count, 'Exact struct member count');",
        "}",
        f"function _number(value{a(': unknown')}, minimum{a(': number')}, maximum{a(': number')}){a(': void')} {{",
        "  _require(typeof value === 'number' && Number.isInteger(value) && value >= minimum && value <= maximum, 'Integer representation range');",
        "}",
        f"function _bigint(value{a(': unknown')}, minimum{a(': bigint')}, maximum{a(': bigint')}){a(': void')} {{",
        "  _require(typeof value === 'bigint' && value >= minimum && value <= maximum, 'U64/S64 requires BigInt in representation range');",
        "}",
    ]


def _scalar_checks(row) -> list[str]:
    kind = row["kind"]
    if kind == 0:
        return ["_require(value === undefined, 'Void has no value');"]
    if kind == 1:
        code = row["code"]
        if code == 1:
            return ["_require(typeof value === 'boolean', 'Bool requires a boolean');"]
        if code in (10, 11):
            result = ["_require(typeof value === 'number', 'Float requires a Number');"]
            if code == 10:
                result.append("_require(!Number.isFinite(value) || Number.isFinite(Math.fround(value)), 'F32 representation overflow');")
            return result
        bits = row["wire"] * 8
        signed = code in (3, 5, 7, 9)
        minimum = -(1 << (bits - 1)) if signed else 0
        maximum = (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1
        big = code in (8, 9)
        helper = "_bigint" if big else "_number"
        return [f"{helper}(value, {_integer_literal(minimum, big)}, {_integer_literal(maximum, big)});"]
    raise ValueError("scalar checks require Void or Scalar")


_ACCESSORS = {1: "Uint8", 2: "Uint8", 3: "Int8", 4: "Uint16", 5: "Int16",
              6: "Uint32", 7: "Int32", 8: "BigUint64", 9: "BigInt64",
              10: "Float32", 11: "Float64"}


def _write_body(schema, row, wire_editing=False) -> list[str]:
    kind = row["kind"]
    if kind == 0:
        return _scalar_checks(row)
    if kind == 1:
        code = row["code"]
        value = "value ? 1 : 0" if code == 1 else "value"
        endian = ", true" if row["wire"] > 1 else ""
        checks = _scalar_checks(row)
        if wire_editing:
            checks += ["if (original) {"]
            if code in (10, 11):
                checks += [f"  const previous = _read_{row['id']}(original, offset);",
                           "  if (Number.isNaN(value) && Number.isNaN(previous)) {",
                           f"    for (let i = 0; i < {row['wire']}; ++i) view.setUint8(offset + i, original.getUint8(offset + i));",
                           "    return;", "  }"]
            else:
                checks += [f"  _read_{row['id']}(original, offset);"]
            checks += ["}"]
        return checks + [f"view.set{_ACCESSORS[code]}(offset, {value}{endian});"]
    seed = ", original" if wire_editing else ""
    if kind == 2:
        return [f"_write_{row['underlying']}(view, offset, value{seed});"]
    if kind == 4:
        size = schema.types[row["element"]]["wire"]
        return [f"_require(Array.isArray(value) && value.length === {row['count']}, 'Exact array length');",
                f"for (let i = 0; i < {row['count']}; ++i) _write_{row['element']}(view, offset + i * {size}, value[i]{seed});"]
    result, offset = [f"_object(value, {len(row['members'])});"], 0
    for child, member in row["members"]:
        result.append(f"_require(Object.hasOwn(value, {_quote(member)}), 'Missing struct member');")
        result.append(f"_write_{child}(view, offset + {offset}, value[{_quote(member)}]{seed});")
        offset += schema.types[child]["wire"]
    return result


def _read_body(schema, row, typed: bool) -> list[str]:
    kind = row["kind"]
    if kind == 0:
        return ["return undefined;"]
    if kind == 1:
        code = row["code"]
        if code == 1:
            return ["const value = view.getUint8(offset);",
                    "_require(value <= 1, 'Invalid bool representation');", "return value !== 0;"]
        endian = ", true" if row["wire"] > 1 else ""
        return [f"return view.get{_ACCESSORS[code]}(offset{endian});"]
    if kind == 2:
        return [f"return _read_{row['underlying']}(view, offset);"]
    if kind == 4:
        child = row["element"]
        annotation = f": {_type_name(child)}[]" if typed else ""
        cast = f" as {_type_name(row['id'])}" if typed else ""
        return [f"const result{annotation} = new Array({row['count']});",
                f"for (let i = 0; i < {row['count']}; ++i) result[i] = _read_{child}(view, offset + i * {schema.types[child]['wire']});",
                f"return result{cast};"]
    cast = f" as {_type_name(row['id'])}" if typed else ""
    result, offset = [f"const result = Object.create(null){cast};"], 0
    for child, member in row["members"]:
        result.append(f"result[{_quote(member)}] = _read_{child}(view, offset + {offset});")
        offset += schema.types[child]["wire"]
    return result + ["return result;"]


def _codec_functions(schema, typed: bool, wire_editing=False) -> list[str]:
    a = lambda annotation: annotation if typed else ""
    lines = []
    for row in _reachable_types(schema):
        i, name, size = row["id"], _type_name(row["id"]), row["wire"]
        seed = ", original" + a("?: DataView") if wire_editing else ""
        lines.append(f"function _write_{i}(view{a(': DataView')}, offset{a(': number')}, value{a(': ' + name)}{seed}){a(': void')} {{")
        lines += ["  " + line for line in _write_body(schema, row, wire_editing)]
        lines += ["}", f"function _read_{i}(view{a(': DataView')}, offset{a(': number')}){a(': ' + name)} {{"]
        lines += ["  " + line for line in _read_body(schema, row, typed)]
        lines += ["}", f"function _encodeInto_{i}(value{a(': ' + name)}, output{a(': ByteInput')}){a(': void')} {{",
                  "  const view = _view(output);",
                  f"  _require(view.byteLength === {size}, 'Exact value payload length');",
                  f"  _write_{i}(view, 0, value);", "}",
                  f"function _encode_{i}(value{a(': ' + name)}){a(': Uint8Array')} {{",
                  f"  const bytes = new Uint8Array({size});", f"  _encodeInto_{i}(value, bytes);",
                  "  return bytes;", "}",
                  f"function _decode_{i}(input{a(': ByteInput')}){a(': ' + name)} {{",
                  "  const view = _view(input);",
                  f"  _require(view.byteLength === {size}, 'Exact value payload length');",
                  f"  return _read_{i}(view, 0);", "}"]
        if wire_editing:
            lines += [f"function _encodePreserving_{i}(value{a(': ' + name)}, input{a(': ByteInput')}){a(': Uint8Array')} {{",
                      "  const source = _view(input);",
                      f"  _require(source.byteLength === {size}, 'Exact original payload length');",
                      "  const seed = new Uint8Array(new Uint8Array(source.buffer, source.byteOffset, source.byteLength));",
                      "  const original = new DataView(seed.buffer);",
                      f"  const bytes = new Uint8Array({size});",
                      f"  _write_{i}(new DataView(bytes.buffer), 0, value, original);",
                      "  return bytes;", "}"]
    return lines


def _enum_expression(schema, row) -> str:
    big = _is_big(schema, row["id"])
    entries = [f"[{_quote(symbol)}]: {_integer_literal(code, big)}"
               for symbol, (code, _) in zip(schema.enum_names[row["id"]], row["entries"])]
    return "Object.freeze({" + ", ".join(entries) + "})"


def _read_many_scalar(schema, type_id: int) -> int | None:
    """Explicit scalar/open-enum eligibility; aggregate wire widths are irrelevant."""
    row = schema.types[type_id]
    if row["kind"] == 2:
        row = schema.types[row["underlying"]]
    return row["code"] if row["kind"] == 1 else None


def _endpoint_expression(schema, endpoint, wire_editing=False) -> str:
    parts = [f"id: {endpoint.id}", f"category: {endpoint.category}",
             f"catalogName: {_quote(endpoint.catalog_name)}", f"name: {_quote(endpoint.name)}"]
    # Every endpoint carries its section-15 path: adc/channel1/Gain, motor/Config.
    parts.append(f"path: {_quote(schema.endpoint_path(endpoint))}")
    if endpoint.category == 1:
        i = endpoint.value_type
        scalar = _read_many_scalar(schema, i)
        marker = f"readManyScalar: {scalar}, " if scalar is not None else ""
        parts += [f"valueTypeId: {i}", f"capabilities: {endpoint.capabilities}",
                  f"read: Object.freeze({{fingerprint, operation: Operation.FieldRead, id: {endpoint.id}, typeId: {i}, requestBytes: 0, responseBytes: {schema.types[i]['wire']}, {marker}decode: _decode_{i}}})"]
        if endpoint.capabilities == 3:
            seed = f", encodePreserving: _encodePreserving_{i}" if wire_editing else ""
            parts.append(f"write: Object.freeze({{fingerprint, operation: Operation.FieldWrite, id: {endpoint.id}, typeId: {i}, requestBytes: {schema.types[i]['wire']}, responseBytes: 0, encode: _encode_{i}, encodeInto: _encodeInto_{i}{seed}}})")
    else:
        i = endpoint.request_type
        operation = "Command" if endpoint.category == 2 else "Service"
        parts += ["fingerprint", f"operation: Operation.{operation}", f"requestTypeId: {i}",
                  f"requestBytes: {schema.types[i]['wire']}",
                  f"responseBytes: {schema.types[endpoint.response_type]['wire'] if endpoint.category == 3 else 0}",
                  f"encode: _encode_{i}", f"encodeInto: _encodeInto_{i}"]
        if endpoint.category == 3:
            parts += [f"responseTypeId: {endpoint.response_type}",
                      f"decodeResponse: _decode_{endpoint.response_type}"]
    return "Object.freeze({" + ", ".join(parts) + "})"


def _runtime_exports(schema, typed: bool, wire_editing=False) -> list[str]:
    a = lambda annotation: annotation if typed else ""
    header = schema.descriptor_header
    lines = [f"export const fingerprint = {schema.fingerprint}n;",
             f"export const descriptorSha256 = {_quote(schema.descriptor_sha256)};",
             f"export const wireVersion = Object.freeze({{major: 3, minor: {schema.minor}}});",
             "export const Operation = Object.freeze({FieldRead: 'FieldRead', FieldWrite: 'FieldWrite', Command: 'Command', Service: 'Service'}" + a(" as const") + ");",
             f"export function assertCompatibleFingerprint(peer{a(': bigint')}){a(': void')} {{",
             "  _require(typeof peer === 'bigint' && peer === fingerprint, 'Descriptor fingerprint mismatch');",
             "}",
             "const _descriptorHeader = Object.freeze([" + ", ".join(str(byte) for byte in header) + "]);",
             "// Compares declared header identity only; does not authenticate a peer or hash the body.",
             f"export function matchesHeader(input{a(': ByteInput')}){a(': boolean')} {{",
             "  const view = _view(input);",
             "  if (view.byteLength !== 64) return false;",
             "  for (let i = 0; i < 64; ++i) if (view.getUint8(i) !== _descriptorHeader[i]) return false;",
             "  return true;",
             "}", "export const codecs = Object.freeze({"]
    for row in _reachable_types(schema):
        i = row["id"]
        seed = f", encodePreserving: _encodePreserving_{i}" if wire_editing else ""
        expression = f"Object.freeze({{wireBytes: {row['wire']}, encode: _encode_{i}, encodeInto: _encodeInto_{i}, decode: _decode_{i}{seed}}})"
        lines.append(f"  {_type_name(i)}: {expression},")
    lines += ["});", "export const enums = Object.freeze({"]
    for row in _reachable_types(schema):
        if row["kind"] == 2:
            lines.append(f"  {_type_name(row['id'])}: {_enum_expression(schema, row)},")
    lines.append("});")
    lines += _named_types_runtime(schema, typed)
    return lines + (_tree_runtime_ts(schema, wire_editing) if typed
                    else _tree_runtime_js(schema, wire_editing))


def _type_source(schema, type_id) -> str:
    """The runtime value behind a type alias: an enum's dictionary, otherwise its codec."""
    return ("enums" if schema.types[type_id]["kind"] == 2 else "codecs") + "." + _type_name(type_id)


def _named_scopes(schema):
    """Reachable named types grouped by scope symbol ("" for a name without a scope)."""
    scopes = {}
    for row in _reachable_types(schema):
        symbol = schema.type_symbol(row["id"])
        if symbol:
            scopes.setdefault(symbol.scope, []).append((symbol, row["id"]))
    return scopes


def _named_types_runtime(schema, typed: bool) -> list[str]:
    """v3.1: `types.adc.Config` is the codec (or enum dictionary) of the type named adc::Config."""
    scopes = _named_scopes(schema)
    if not scopes:
        return []
    lines = ["// Named types (descriptor v3.1), by their wire scope: adc::Config is types.adc.Config."]
    if typed:
        lines.append("export namespace types {")
        for scope, rows in scopes.items():
            pad = "    " if scope else "  "
            if scope:
                lines.append(f"  export namespace {scope} {{")
            for symbol, type_id in rows:
                lines += [f"{pad}export type {symbol.symbol} = {_type_name(type_id)};",
                          f"{pad}export const {symbol.symbol} = {_type_source(schema, type_id)};"]
            if scope:
                lines.append("  }")
        lines.append("}")
        lines += [f"Object.freeze(types.{scope});" for scope in scopes if scope]
        return lines + ["Object.freeze(types);"]
    lines.append("export const types = Object.freeze({")
    for scope, rows in scopes.items():
        pad = "    " if scope else "  "
        if scope:
            lines.append(f"  [{_quote(scope)}]: Object.freeze({{")
        for symbol, type_id in rows:
            lines.append(f"{pad}[{_quote(symbol.symbol)}]: {_type_source(schema, type_id)},")
        if scope:
            lines.append("  }),")
    return lines + ["});"]


def _named_types_declarations(schema) -> list[str]:
    scopes = _named_scopes(schema)
    if not scopes:
        return []
    lines = ["export declare namespace types {"]
    for scope, rows in scopes.items():
        pad = "    " if scope else "  "
        if scope:
            lines.append(f"  namespace {scope} {{")
        for symbol, type_id in rows:
            lines += [f"{pad}type {symbol.symbol} = {_type_name(type_id)};",
                      f"{pad}const {symbol.symbol}: typeof {_type_source(schema, type_id)};"]
        if scope:
            lines.append("  }")
    return lines + ["}"]


_FAMILIES = ((1, "fields"), (2, "commands"), (3, "services"))


def _tree_members(schema):
    """Per group index: its endpoints with their sibling symbols, and its child groups."""
    endpoints, children = {}, {}
    for number, (endpoint, group, symbol) in enumerate(zip(schema.endpoints, schema.api_group_of,
                                                           schema.api_symbols)):
        endpoints.setdefault(group, []).append((number, endpoint, symbol))
    for group in schema.api_groups:
        if group.parent is not None:
            children.setdefault(group.parent, []).append(group)
    return endpoints, children


def _group_comment(schema, separator):
    if schema.groups is not None:
        return f"// Group tree (descriptor v3.1): adc{separator}channel1{separator}Gain is the path adc/channel1/Gain."
    return f"// Groups: the catalogs of one name are one group; motor{separator}Config is the path motor/Config."


def _tree_runtime_js(schema, wire_editing=False) -> list[str]:
    """Nested frozen objects mirror the groups; the family objects keep the flat form.

    adc.channel1.Gain and adc.channel1.fields.Gain are the same endpoint object.
    Children are built before their parents, so every reference is defined.
    """
    endpoints, children = _tree_members(schema)
    lines = [_group_comment(schema, ".")]
    for group in reversed(schema.api_groups):
        rows = endpoints.get(group.index, ())
        for number, endpoint, _ in rows:
            lines.append(f"const _e{number} = {_endpoint_expression(schema, endpoint, wire_editing)};")
        aliases = dict(alias for _, endpoint, _ in rows for alias in _aliases(endpoint))
        lines += [f"const _g{group.index} = Object.freeze({{", "  types: Object.freeze({"]
        for alias, type_id in aliases.items():
            lines.append(f"    [{_quote(alias)}]: {_type_source(schema, type_id)},")
        if not aliases:
            lines.append("    _empty: undefined,")
        lines.append("  }),")
        for category, family in _FAMILIES:
            lines.append(f"  {family}: Object.freeze({{")
            for number, endpoint, _ in rows:
                if endpoint.category == category:
                    lines.append(f"    [{_quote(endpoint.symbol)}]: _e{number},")
            lines.append("  }),")
        for child in children.get(group.index, ()):
            lines.append(f"  [{_quote(child.symbol)}]: _g{child.index},")
        for number, _, symbol in rows:
            lines.append(f"  [{_quote(symbol)}]: _e{number},")
        lines.append("});")
    lines += [f"export const {group.symbol} = _g{group.index};" for group in schema.api_groups if group.parent is None]
    return lines


def _tree_runtime_ts(schema, wire_editing=False) -> list[str]:
    """The TS form of _tree_runtime_js: nested namespaces with the same values."""
    endpoints, children = _tree_members(schema)
    lines = [_group_comment(schema, ".")]

    def emit(group, depth):
        pad = "  " * depth
        rows = endpoints.get(group.index, ())
        lines.extend((f"{pad}export namespace {group.symbol} {{", f"{pad}  export namespace types {{"))
        aliases = dict(alias for _, endpoint, _ in rows for alias in _aliases(endpoint))
        for alias, type_id in aliases.items():
            lines.extend((f"{pad}    export type {alias} = {_type_name(type_id)};",
                          f"{pad}    export const {alias} = {_type_source(schema, type_id)};"))
        if not aliases:
            lines.append(f"{pad}    export const _empty = undefined;")
        lines.append(f"{pad}  }}")
        for category, family in _FAMILIES:
            lines.append(f"{pad}  export const {family} = Object.freeze({{")
            for _, endpoint, _ in rows:
                if endpoint.category == category:
                    lines.append(f"{pad}    [{_quote(endpoint.symbol)}]: {_endpoint_expression(schema, endpoint, wire_editing)},")
            lines.append(f"{pad}  }});")
        for child in children.get(group.index, ()):
            emit(child, depth + 1)
        for _, endpoint, symbol in rows:
            family = _FAMILIES[endpoint.category - 1][1]
            lines.append(f"{pad}  export const {symbol} = {family}[{_quote(endpoint.symbol)}];")
        lines.append(f"{pad}}}")

    for group in schema.api_groups:
        if group.parent is None:
            emit(group, 0)
    for group in reversed(schema.api_groups):
        lines += [f"Object.freeze({group.js}.types);", f"Object.freeze({group.js});"]
    return lines


def _endpoint_declaration(schema, endpoint, wire_editing=False) -> str:
    """The declared type of one endpoint object, as the runtime builds it."""
    category = endpoint.category
    fields = [f"readonly id: {endpoint.id}", f"readonly category: {category}",
              "readonly catalogName: string", "readonly name: string", "readonly path: string"]
    i = endpoint.value_type if category == 1 else endpoint.request_type
    if category == 1:
        scalar = _read_many_scalar(schema, i)
        marker = f"readonly readManyScalar: {scalar}; " if scalar is not None else ""
        fields += [f"readonly valueTypeId: {i}", f"readonly capabilities: {endpoint.capabilities}",
                   f"readonly read: Readonly<{{readonly fingerprint: typeof fingerprint; operation: 'FieldRead'; id: {endpoint.id}; typeId: {i}; requestBytes: 0; responseBytes: {schema.types[i]['wire']}; {marker}decode: ValueCodec<Type_{i}>['decode']}}>" ]
        if endpoint.capabilities == 3:
            seed = f"; encodePreserving: ValueCodec<Type_{i}>['encodePreserving']" if wire_editing else ""
            fields.append(f"readonly write: Readonly<{{readonly fingerprint: typeof fingerprint; operation: 'FieldWrite'; id: {endpoint.id}; typeId: {i}; requestBytes: {schema.types[i]['wire']}; responseBytes: 0; encode: ValueCodec<Type_{i}>['encode']; encodeInto: ValueCodec<Type_{i}>['encodeInto']{seed}}}>")
    else:
        operation = "Command" if category == 2 else "Service"
        fields += ["readonly fingerprint: typeof fingerprint", f"readonly operation: '{operation}'", f"readonly requestTypeId: {i}",
                   f"readonly requestBytes: {schema.types[i]['wire']}",
                   f"readonly responseBytes: {schema.types[endpoint.response_type]['wire'] if category == 3 else 0}",
                   f"readonly encode: ValueCodec<Type_{i}>['encode']",
                   f"readonly encodeInto: ValueCodec<Type_{i}>['encodeInto']"]
        if category == 3:
            fields += [f"readonly responseTypeId: {endpoint.response_type}",
                       f"readonly decodeResponse: ValueCodec<Type_{endpoint.response_type}>['decode']"]
    return f"Readonly<{{{'; '.join(fields)}}}>"


def _tree_declarations(schema, wire_editing=False) -> list[str]:
    endpoints, children = _tree_members(schema)
    lines = []

    def emit(group, depth):
        pad = "  " * depth
        rows = endpoints.get(group.index, ())
        lines.extend((f"{pad}{'export declare ' if depth == 0 else ''}namespace {group.symbol} {{",
                      f"{pad}  namespace types {{"))
        aliases = dict(alias for _, endpoint, _ in rows for alias in _aliases(endpoint))
        for alias, type_id in aliases.items():
            lines.extend((f"{pad}    type {alias} = {_type_name(type_id)};",
                          f"{pad}    const {alias}: typeof {_type_source(schema, type_id)};"))
        if not aliases:
            lines.append(f"{pad}    const _empty: undefined;")
        lines.append(f"{pad}  }}")
        for category, family in _FAMILIES:
            lines.append(f"{pad}  const {family}: Readonly<{{")
            for _, endpoint, _ in rows:
                if endpoint.category == category:
                    lines.append(f"{pad}    readonly {_quote(endpoint.symbol)}: {_endpoint_declaration(schema, endpoint, wire_editing)};")
            lines.append(f"{pad}  }}>;")
        for child in children.get(group.index, ()):
            emit(child, depth + 1)
        for _, endpoint, symbol in rows:
            family = _FAMILIES[endpoint.category - 1][1]
            lines.append(f"{pad}  const {symbol}: (typeof {family})[{_quote(endpoint.symbol)}];")
        lines.append(f"{pad}}}")

    for group in schema.api_groups:
        if group.parent is None:
            emit(group, 0)
    return lines


def _declarations(schema, wire_editing=False) -> str:
    lines = ["// Generated telemetry v3.0 contract. SPDX-License-Identifier: MIT."]
    lines += _types(schema, wire_editing)
    lines += ["export declare class TelemetryCodecError extends Error { constructor(reason: string); }",
              f"export declare const fingerprint: {schema.fingerprint}n;",
              "export declare const descriptorSha256: string;",
              f"export declare const wireVersion: Readonly<{{major: 3; minor: {schema.minor}}}>;",
              "export declare const Operation: Readonly<{FieldRead: 'FieldRead'; FieldWrite: 'FieldWrite'; Command: 'Command'; Service: 'Service'}>;",
              "export declare function assertCompatibleFingerprint(peer: bigint): void;",
              "export declare function matchesHeader(input: ByteInput): boolean;",
              "export declare const codecs: Readonly<{"]
    for row in _reachable_types(schema):
        lines.append(f"  {_type_name(row['id'])}: ValueCodec<{_type_name(row['id'])}>;")
    lines += ["}>;", "export declare const enums: Readonly<{"]
    for row in _reachable_types(schema):
        if row["kind"] == 2:
            fields = "; ".join(f"readonly {_quote(symbol)}: {_integer_literal(code, _is_big(schema, row['id']))}"
                               for symbol, (code, _) in zip(schema.enum_names[row["id"]], row["entries"]))
            lines.append(f"  {_type_name(row['id'])}: Readonly<{{{fields}}}>;")
    lines.append("}>;")
    lines += _named_types_declarations(schema)
    return "\n".join(lines + _tree_declarations(schema, wire_editing)) + "\n"


def _runtime(schema, typed: bool, wire_editing=False) -> str:
    lines = ["// Generated telemetry v3.0 codec. SPDX-License-Identifier: MIT.",
             "// Pure payloads only: transport status envelopes are owned by the application.",
             "// Arrays are ordinary JS arrays. Decoded aggregates own their data.",
             "// Aggregate encodeInto may write a prefix before a later member is rejected.",
             "// encodeInto reuses caller payload storage; DataView output also reuses its view."]
    if typed:
        lines += _types(schema, wire_editing)
    lines += _helpers(typed)
    lines += _codec_functions(schema, typed, wire_editing)
    lines += _runtime_exports(schema, typed, wire_editing)
    return "\n".join(lines) + "\n"


def generate_js(schema, client_runtime=False, wire_editing=False) -> dict[str, str]:
    """Return plain ESM plus declarations, without runtime descriptor parsing."""
    files = {"js/device.js": _runtime(schema, False, wire_editing), "js/device.d.ts": _declarations(schema, wire_editing)}
    if client_runtime:
        files.update(_client_files("js", ("js", "d.ts")))
        # Endpoint handles over the bundled client, nested by group and by path.
        from .handles import web_handles, web_handles_declarations
        files["js/handles.js"] = web_handles(schema, False)
        files["js/handles.d.ts"] = web_handles_declarations(schema)
    return files


def generate_ts(schema, client_runtime=False, wire_editing=False) -> dict[str, str]:
    """Return strict ES2022 TypeScript, sharing all runtime codec statements."""
    files = {"ts/device.ts": _runtime(schema, True, wire_editing)}
    if client_runtime:
        files.update(_client_files("ts", ("ts",)))
        from .handles import web_handles
        files["ts/handles.ts"] = web_handles(schema, True)
    return files


def _client_files(target, extensions):
    """Copy the optional self-contained runtime; codecs keep no runtime import."""
    from importlib.resources import files
    from pathlib import Path
    packaged = files("tools.telemetry_codegen").joinpath("_runtime")
    if packaged.is_dir():
        if not all(packaged.joinpath(f"Client.{extension}").is_file() for extension in extensions):
            raise ValueError("Installed codegen client runtime assets are incomplete")
        return {f"{target}/client.{extension}": packaged.joinpath(f"Client.{extension}").read_text(encoding="utf-8")
                for extension in extensions}
    source = Path(__file__).resolve().parents[3]
    if not (source / "VERSION").is_file() or not (source / "pyproject.toml").is_file():
        raise ValueError("Installed codegen client runtime assets are missing")
    runtime = source / "lib/client/web"
    return {f"{target}/client.{extension}": (runtime / f"Client.{extension}").read_text(encoding="utf-8")
            for extension in extensions}
