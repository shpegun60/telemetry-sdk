"""Optional structural data from the validated IR, without UI policy or runtime state.

Type references and member offsets describe wire layouts, never native DTO memory.
All language emitters consume the same document; ordinary SDK output is unchanged.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
import json

from ..naming import allocate
from . import cpp

FORMAT = "telemetry-structural-metadata"
VERSION = 1


def document(schema):
    """Return a portable structural graph; signed/unsigned enum codes stay strings."""
    types = []
    for source in schema.types:
        row = dict(id=source["id"], kind=source["kind"], wireBytes=source["wire"])
        if source["kind"] == 1:
            row["scalarCode"] = source["code"]
        elif source["kind"] == 2:
            row["underlyingTypeId"] = source["underlying"]
            row["entries"] = [dict(code=str(code), name=name) for code, name in source["entries"]]
        elif source["kind"] == 3:
            offset = 0
            row["members"] = []
            for index, (type_id, name) in enumerate(source["members"]):
                row["members"].append(dict(typeId=type_id, name=name, wireOffset=offset,
                    cppName=schema.member_names[source["id"]][index], jsName=name))
                offset += schema.types[type_id]["wire"]
        elif source["kind"] == 4:
            row.update(elementTypeId=source["element"], elementCount=source["count"])
        symbol = schema.type_symbol(source["id"])
        if symbol:
            # v3.1 additions: the wire name and the generated spellings.
            row.update(name=symbol.name, cppName=symbol.cpp, jsName=symbol.js)
        types.append(row)
    if schema.groups is not None:
        symbols = None
    else:
        names = tuple(dict.fromkeys(catalog["name"] for group in schema.raw["catalogs"] for catalog in group))
        symbols = dict(zip(names, allocate(names)))
    catalogs = [dict(category=category, group=row["group"], firstInCategory=row["first"],
                     count=row["count"], name=row["name"],
                     symbol=symbols[row["name"]] if symbols is not None else schema.groups[row["group"]].qualified)
                for category, rows in enumerate(schema.raw["catalogs"], 1) for row in rows]
    endpoints = [dict(category=row.category, id=row.id, catalogIndex=row.catalog_index,
                      catalogName=row.catalog_name, name=row.name, groupSymbol=row.group,
                      symbol=row.symbol, valueType=row.value_type, requestType=row.request_type,
                      responseType=row.response_type, capabilities=row.capabilities)
                 for row in schema.endpoints]
    if schema.minor >= 1:
        # v3.1: every endpoint's section-15 path, and in a tree its symbol beside its siblings.
        for index, (row, endpoint) in enumerate(zip(endpoints, schema.endpoints)):
            row["path"] = schema.endpoint_path(endpoint)
            if schema.groups is not None:
                row["treeSymbol"] = schema.tree_symbols[index]
    result = dict(format=FORMAT, version=VERSION, wireVersion=dict(major=3, minor=schema.minor),
                  fingerprint=f"0x{schema.fingerprint:016x}", descriptorSha256=schema.descriptor_sha256,
                  descriptorHeader=list(schema.descriptor_header), namespace=schema.namespace,
                  types=types, catalogs=catalogs, endpoints=endpoints)
    if schema.groups is not None:
        result["groups"] = [dict(index=group.index, name=group.name, path=group.path, depth=group.depth,
                                 parent=group.parent, symbol=group.symbol, cppName=group.qualified,
                                 jsName=group.js)
                            for group in schema.groups]
    return result


DECLARATIONS = '''export interface StructuralMember {
  readonly typeId: number; readonly name: string; readonly wireOffset: number;
  readonly cppName: string; readonly jsName: string;
}
export interface StructuralEnumEntry { readonly code: string; readonly name: string; }
export interface StructuralTypeBase { readonly id: number; readonly wireBytes: number; }
export interface StructuralVoid extends StructuralTypeBase { readonly kind: 0; }
export interface StructuralScalar extends StructuralTypeBase {
  readonly kind: 1; readonly scalarCode: 1|2|3|4|5|6|7|8|9|10|11;
}
export interface StructuralEnum extends StructuralTypeBase {
  readonly kind: 2; readonly underlyingTypeId: number; readonly entries: readonly StructuralEnumEntry[];
}
export interface StructuralStruct extends StructuralTypeBase {
  readonly kind: 3; readonly members: readonly StructuralMember[];
}
export interface StructuralArray extends StructuralTypeBase {
  readonly kind: 4; readonly elementTypeId: number; readonly elementCount: number;
}
export type StructuralType = StructuralVoid|StructuralScalar|StructuralEnum|StructuralStruct|StructuralArray;
export interface StructuralCatalog {
  readonly category: 1|2|3; readonly group: number; readonly firstInCategory: number;
  readonly count: number; readonly name: string; readonly symbol: string;
}
export interface StructuralEndpoint {
  readonly category: 1|2|3; readonly id: number; readonly catalogIndex: number;
  readonly catalogName: string; readonly name: string; readonly groupSymbol: string;
  readonly symbol: string; readonly valueType: number; readonly requestType: number;
  readonly responseType: number; readonly capabilities: number;
}
export interface StructuralMetadata {
  readonly format: 'telemetry-structural-metadata'; readonly version: 1;
  readonly wireVersion: Readonly<{major: 3; minor: 0}>;
  readonly fingerprint: string; readonly descriptorSha256: string;
  readonly descriptorHeader: readonly number[]; readonly namespace: string;
  readonly types: readonly StructuralType[]; readonly catalogs: readonly StructuralCatalog[];
  readonly endpoints: readonly StructuralEndpoint[];
}
'''


NAMED_TYPE = "export interface StructuralTypeBase { readonly id: number; readonly wireBytes: number; }\n"
NAMED_TYPE_V31 = ("export interface StructuralTypeBase {\n"
                  "  readonly id: number; readonly wireBytes: number;\n"
                  "  readonly name?: string; readonly cppName?: string; readonly jsName?: string;\n}\n")
ENDPOINT_TAIL = "  readonly responseType: number; readonly capabilities: number;\n}\n"
GROUP_V31 = """export interface StructuralGroup {
  readonly index: number; readonly name: string; readonly path: string; readonly depth: number;
  readonly parent: number | null; readonly symbol: string; readonly cppName: string; readonly jsName: string;
}
"""


def _declarations(minor, tree=False):
    """The typings pin the descriptor's own minor, as wireVersion does.

    A v3.1 descriptor adds optional type names, endpoint paths and, for a
    group tree, the groups; v3.0 typings stay exactly as they were.
    """
    text = DECLARATIONS.replace("{major: 3; minor: 0}", f"{{major: 3; minor: {minor}}}")
    if minor >= 1:
        text = text.replace(NAMED_TYPE, NAMED_TYPE_V31)
        text = text.replace(ENDPOINT_TAIL, "  readonly responseType: number; readonly capabilities: number;\n"
                            "  readonly path: string;" + (" readonly treeSymbol: string;" if tree else "") +
                            "\n}\n")
    if tree:
        text = text.replace("export interface StructuralMetadata {", GROUP_V31 + "export interface StructuralMetadata {")
        text = text.replace("  readonly endpoints: readonly StructuralEndpoint[];\n}",
                            "  readonly endpoints: readonly StructuralEndpoint[];\n"
                            "  readonly groups: readonly StructuralGroup[];\n}")
    return text


def _web(data, typed):
    rows = ["// Optional generated structural wire metadata. SPDX-License-Identifier: MIT.",
            "// No native offsets, defaults, units, session or executable client state."]
    if typed:
        rows.append(_declarations(data["wireVersion"]["minor"], "groups" in data))
    rows += ["function freeze" + ("<T>" if typed else "") + "(value" +
             (": T): Readonly<T>" if typed else ")") + " {",
             "  if (value !== null && typeof value === 'object') {",
             "    for (const child of Object.values(value)) freeze(child);",
             "    Object.freeze(value);", "  }", "  return value;", "}",
             "const document = " + json.dumps(data, ensure_ascii=True, indent=2) +
             (" as const;" if typed else ";"),
             "export const structuralMetadata" + (": StructuralMetadata" if typed else "") +
             " = freeze(document);"]
    return "\n".join(rows) + "\n"


def _cpp(schema, data):
    v31 = schema.minor >= 1
    out = [cpp._header(schema, "Metadata", "#include <array>\n#include <cstdint>\n#include <string_view>\n"),
           "namespace metadata {\n",
           "// Wire-local offsets only. These records never describe native DTO memory.\n",
           "struct TypeRecord { std::uint32_t id; std::uint8_t kind, scalarCode; "
           "std::uint32_t wireBytes, relatedType, elementCount, memberFirst, memberCount, enumFirst, enumCount;" +
           (" std::string_view name, cppName, jsName;" if v31 else "") + " };\n",
           "struct MemberRecord { std::uint32_t ownerType, typeId, wireOffset; "
           "std::string_view name, cppName; };\n",
           "struct EnumRecord { std::uint32_t ownerType; std::uint64_t bits; std::string_view name; };\n",
           "struct CatalogRecord { std::uint8_t category; std::uint32_t group, firstInCategory, count; "
           "std::string_view name, symbol; };\n",
           "struct EndpointRecord { std::uint8_t category; std::uint32_t id, catalogIndex, "
           "valueType, requestType, responseType; std::uint8_t capabilities; "
           "std::string_view catalogName, name, groupSymbol, symbol;" + (" std::string_view path;" if v31 else "") +
           " };\n",
           f'inline constexpr char format[] = "{FORMAT}";\n',
           "inline constexpr std::uint32_t version = 1;\n",
           f"inline constexpr std::uint16_t wireMajor = 3, wireMinor = {schema.minor};\n",
           f'inline constexpr std::uint64_t fingerprint = {data["fingerprint"]}ULL;\n',
           f'inline constexpr char descriptorSha256[] = "{schema.descriptor_sha256}";\n',
           "inline constexpr std::array<std::uint8_t, 64> descriptorHeader{" +
           ", ".join(str(byte) for byte in data["descriptorHeader"]) + "};\n"]
    members, enums, types = [], [], []
    quote = cpp._string
    for row in data["types"]:
        first_member, first_enum = len(members), len(enums)
        for member in row.get("members", ()):
            members.append("{" + f'{row["id"]}, {member["typeId"]}, {member["wireOffset"]}, ' +
                           quote(member["name"]) + ", " + quote(member["cppName"]) + "}")
        for entry in row.get("entries", ()):
            bits = int(entry["code"]) & 0xffffffffffffffff
            enums.append("{" + f'{row["id"]}, 0x{bits:016x}ULL, ' + quote(entry["name"]) + "}")
        values = [row["id"], row["kind"], row.get("scalarCode", 0), row["wireBytes"],
                  row.get("underlyingTypeId", row.get("elementTypeId", 0)), row.get("elementCount", 0),
                  first_member, len(members) - first_member, first_enum, len(enums) - first_enum]
        names = ("".join(", " + (quote(row[key]) if key in row else "{}") for key in ("name", "cppName", "jsName"))
                 if v31 else "")
        types.append("{" + ", ".join(map(str, values)) + names + "}")
    catalogs = ["{" + ", ".join(str(row[key]) for key in ("category", "group", "firstInCategory", "count")) +
                ", " + quote(row["name"]) + ", " + quote(row["symbol"]) + "}" for row in data["catalogs"]]
    endpoints = ["{" + ", ".join(str(row[key]) for key in
                 ("category", "id", "catalogIndex", "valueType", "requestType", "responseType", "capabilities")) +
                 ", " + ", ".join(quote(row[key]) for key in ("catalogName", "name", "groupSymbol", "symbol") +
                                  (("path",) if v31 else ())) +
                 "}" for row in data["endpoints"]]
    for record, name, values in (("TypeRecord", "typeRecords", types), ("MemberRecord", "memberRecords", members),
                                 ("EnumRecord", "enumRecords", enums), ("CatalogRecord", "catalogs", catalogs),
                                 ("EndpointRecord", "endpoints", endpoints)):
        out.append(f"inline constexpr std::array<{record}, {len(values)}> {name}{{{{\n" +
                   ",\n".join("  " + value for value in values) + "\n}};\n")
    if "groups" in data:
        rows = ["{" + f'{row["index"]}, {row["depth"]}, ' +
                (str(row["parent"]) if row["parent"] is not None else "noParent") + ", " +
                ", ".join(quote(row[key]) for key in ("name", "path", "symbol", "cppName", "jsName")) + "}"
                for row in data["groups"]]
        out += ["// Group tree (v3.1): pre-order; the parent of a root is noParent.\n",
                "inline constexpr std::uint32_t noParent = 0xffffffffu;\n",
                "struct GroupRecord { std::uint32_t index, depth, parent; "
                "std::string_view name, path, symbol, cppName, jsName; };\n",
                f"inline constexpr std::array<GroupRecord, {len(rows)}> groups{{{{\n" +
                ",\n".join("  " + row for row in rows) + "\n}};\n"]
    out += ["} // namespace metadata\n", f"}} // namespace {schema.namespace}\n"]
    return "".join(out)


def generate(schema, targets):
    """Emit additional explicitly selected files, leaving normal SDK files untouched."""
    data = document(schema)
    files = {"metadata/schema.json": json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + "\n"}
    if "cpp" in targets:
        files["cpp/" + schema.namespace.replace("::", "/") + "/Metadata.hpp"] = _cpp(schema, data)
    if "js" in targets:
        files["js/device.metadata.js"] = _web(data, False)
        files["js/device.metadata.d.ts"] = ("// Optional structural wire metadata. MIT.\n" +
                                            _declarations(schema.minor, schema.groups is not None) +
                                            "export declare const structuralMetadata: StructuralMetadata;\n")
    if "ts" in targets:
        files["ts/device.metadata.ts"] = _web(data, True)
    return files
