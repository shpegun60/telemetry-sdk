"""Descriptor v3.1 fixtures and C++ programs for the generator's names and group tree.

Writes C++ sources from a validated schema; the runner compiles and runs them:
- rebuild_source: a native Model built from the generated DTOs, bound to
  stateless functions; it must re-emit the input descriptor byte for byte.
- parity_source: RuntimeSchema (Dynamic) against generated Metadata.hpp and
  typeName<T>() of the generated types (Static), names and paths.
- alias_source: code written against the group form of a flat (catalog)
  model's generated API; it must compile unchanged against the SDK of the
  same groups as a tree. There are no per-family symbols to fall back on.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/structured/descriptor"
if str(FIXTURES) not in sys.path:
    sys.path.insert(0, str(FIXTURES))
import golden  # noqa: E402  Independent wire builder, also used by the decoder tables.


def fixture(name):
    """The committed producer goldens, plus derived v3.1 shapes built by golden.py."""
    if name == "tree_unnamed":
        return golden.tree_fixture(names={})
    if name in ("tree_flat_roots", "flat_roots"):
        return root_twins()[name == "tree_flat_roots"]
    return bytes.fromhex((FIXTURES / (name + ".hex")).read_text())


# The depth-0 part of the tree fixture: a device that moves from flat catalogs
# to the group form without moving anything (plan section 15.5).
ROOT_GROUPS = [golden.TREE_GROUPS[0], golden.TREE_GROUPS[3]]


def root_twins():
    """(flat, tree) descriptors of one model: the same groups, endpoints, IDs and names."""
    tree = golden.tree_fixture(groups=ROOT_GROUPS)
    types = tree[64:struct.unpack_from("<I", tree, 56)[0]]  # every type record, names included
    catalogs, endpoints, counts = [], [], []
    for category in range(3):
        first, rows = 0, []
        for group, (name, _, *families) in enumerate(ROOT_GROUPS):
            entries = families[category]
            if not entries:
                continue
            index = len(rows)
            catalogs.append(golden.record(2, bytes((category + 1, 0, 0, 0)) +
                                          golden.u32(index, first, len(entries)) + golden.text(name)))
            rows.append(name)
            for local, entry in enumerate(entries):
                payload = golden.u32(index << 16 | local, entry[0])
                if category == 0:
                    payload += bytes((entry[2], 0, 0, 0)) + golden.text(entry[1])
                elif category == 1:
                    payload += golden.text(entry[1])
                else:
                    payload += golden.u32(entry[1]) + golden.text(entry[2])
                endpoints.append(golden.record(category + 3, payload))
            first += len(entries)
        counts += [len(rows), first]
    type_count = struct.unpack_from("<I", tree, 24)[0]
    cat, ep = b"".join(catalogs), b"".join(endpoints)
    total = 64 + len(types) + len(cat) + len(ep)
    header = b"TDS3" + struct.pack("<4HIQ10I", 3, 1, 64, 0, total, 0, type_count, *counts,
                                   64, 64 + len(types), 64 + len(types) + len(cat))
    flat = bytearray(header + types + cat + ep)
    struct.pack_into("<Q", flat, 16, golden.fingerprint(flat))
    return bytes(flat), tree


GOLDENS = ROOT / "tests/codegen/goldens"
# The files that show names and the tree; manifest.json hashes every other file.
GOLDEN_FILES = ("cpp/device_api/Types.hpp", "cpp/device_api/Endpoints.hpp", "cpp/device_api/Schema.hpp",
                "cpp/device_api/TelemetryBindings.hpp", "js/device.d.ts", "metadata/schema.json",
                "manifest.json")


def golden_path(name, path):
    """Goldens keep a .golden suffix: generated text is not a maintained source file."""
    return GOLDENS / name / (path + ".golden")


def golden_render(name):
    """The generated SDK of one golden fixture: every target and the structural metadata."""
    sys.path.insert(0, str(ROOT))
    from tools.telemetry_codegen.cli import render
    from tools.telemetry_codegen.schema import load_schema
    return render(load_schema(fixture(name)), ("cpp", "js", "ts"), structural_metadata=True)


def write_goldens():
    """Refresh the reviewed goldens; run only for a deliberate generator change."""
    for name in ("named", "tree"):
        files = golden_render(name)
        for path in GOLDEN_FILES:
            target = golden_path(name, path)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(files[path])


def _include(schema, name):
    return f'#include <{schema.namespace.replace("::", "/")}/{name}.hpp>\n'


def _type(schema, type_id):
    return f"::{schema.namespace}::detail::Type_{type_id}"


def _string(value):
    return '"' + ''.join(f"\\x{byte:02x}" for byte in value.encode("utf-8")) + '"'


def _binding(schema, endpoint):
    """A ts::field/command/service bound to stateless functions of the generated types."""
    name = _string(endpoint.name)
    if endpoint.category == 1:
        value = _type(schema, endpoint.value_type)
        setter = f", &set<{value}>" if endpoint.capabilities == 3 else ""
        return f"ts::field<&get<{value}>{setter}>({name})"
    request = _type(schema, endpoint.request_type) if endpoint.request_type else None
    if endpoint.category == 2:
        return f"ts::command<&commandWith<{request}>>({name})" if request else f"ts::command<&command>({name})"
    response = _type(schema, endpoint.response_type) if endpoint.response_type else None
    if request and response:
        target = f"serviceWith<{request}, {response}>"
    elif request:
        target = f"serviceConsume<{request}>"
    elif response:
        target = f"serviceReturn<{response}>"
    else:
        target = "serviceVoid"
    return f"ts::service<&{target}>({name})"


REBUILD_PRELUDE = """#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <telemetry/testing/V30Model.hpp>

#include <cstdio>

namespace rebuild {
namespace ts = ::telemetry;
template<class T> T get() noexcept { return T{}; }
template<class T> ts::WriteResult set(const T&) noexcept { return ts::WriteResult::Applied; }
inline ts::CommandResult command() noexcept { return ts::CommandResult::Executed; }
template<class Q> ts::CommandResult commandWith(const Q&) noexcept { return ts::CommandResult::Executed; }
template<class Q, class R> R serviceWith(const Q&) noexcept { return R{}; }
template<class Q> void serviceConsume(const Q&) noexcept {}
template<class R> R serviceReturn() noexcept { return R{}; }
inline void serviceVoid() noexcept {}
"""

REBUILD_MAIN = """inline constexpr auto bytes = ::telemetry::descriptor::packDescriptor<descriptor>();
} // namespace rebuild

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	std::FILE* file = std::fopen(argv[1], "wb");
	if (!file)
		return 2;
	const bool written = std::fwrite(rebuild::bytes.data(), 1, rebuild::bytes.size(), file) ==
	                     rebuild::bytes.size();
	return std::fclose(file) == 0 && written ? 0 : 1;
}
"""


def rebuild_source(schema):
    """A native Model from the generated DTOs that mirrors the descriptor's tables.

    A v3.0 input is re-emitted through withoutTypeNames, as a device that keeps
    v3.0 bytes would build it; a v3.1 input through the default profile, its
    names coming from the generated TelemetryBindings.hpp.
    """
    rows = ["// Generated by tests/codegen/names.py. SPDX-License-Identifier: MIT\n",
            _include(schema, "Types"), _include(schema, "TelemetryBindings"), REBUILD_PRELUDE]
    if schema.groups is not None:
        members, children = {}, {}
        for endpoint in schema.endpoints:
            members.setdefault(endpoint.catalog_index, []).append(endpoint)
        for group in schema.groups:
            if group.parent is not None:
                children.setdefault(group.parent, []).append(group)
        for group in reversed(schema.groups):
            items = [f"group{child.index}" for child in children.get(group.index, ())]
            items += [_binding(schema, endpoint) for endpoint in members.get(group.index, ())]
            rows.append(f"inline constexpr auto group{group.index} = ts::group({_string(group.name)}" +
                        "".join(",\n    " + item for item in items) + ");\n")
        roots = ", ".join(f"group{group.index}" for group in schema.groups if group.parent is None)
        rows.append(f"inline constexpr ts::Model model{{{roots}}};\n")
    else:
        for category, (table, catalog) in enumerate((("FieldTable", "FieldCatalogTable"),
                                                     ("CommandTable", "CommandCatalogTable"),
                                                     ("ServiceTable", "ServiceCatalogTable")), 1):
            groups = []
            for index, row in enumerate(schema.raw["catalogs"][category - 1]):
                endpoints = [endpoint for endpoint in schema.endpoints
                             if endpoint.category == category and endpoint.catalog_index == row["group"]]
                if endpoints:
                    rows.append(f"inline constexpr ts::{table} table{category}_{index}{{" +
                                ",\n    ".join(_binding(schema, endpoint) for endpoint in endpoints) + "};\n")
                else:
                    rows.append(f"inline constexpr ts::{table}<> table{category}_{index}{{}};\n")
                groups.append(f"ts::testing::group({_string(row['name'])}, table{category}_{index})")
            rows.append(f"inline constexpr ts::testing::{catalog}{'<>' if not groups else ''} catalogs{category}{{" +
                        ", ".join(groups) + "};\n")
        # A catalog descriptor's per-family form is the testing header's v3.0 producer.
        rows.append("inline constexpr ts::testing::V30Model model{catalogs1, catalogs2, catalogs3};\n")
    profile = "::telemetry::descriptor::withoutTypeNames(model)" if schema.minor == 0 else \
        "::telemetry::descriptor::Descriptor{model}"
    rows.append(f"inline constexpr auto descriptor = {profile};\n")
    rows.append("static_assert(descriptor.valid());\n")
    rows.append(REBUILD_MAIN)
    return "".join(rows)


PARITY_MAIN = """#include <telemetry/runtime/RuntimeSchema.hpp>

#include <array>
#include <cstdio>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
namespace rt = ::telemetry::runtime;
namespace meta = ::@NS@::metadata;
int checks = 0, failures = 0;

void check(bool condition, const char* what, std::string_view detail = {})
{
	++checks;
	if (!condition) {
		++failures;
		std::printf("FAIL %s %.*s\\n", what, static_cast<int>(detail.size()), detail.data());
	}
}
} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	std::FILE* file = std::fopen(argv[1], "rb");
	if (!file)
		return 2;
	std::vector<std::byte> bytes;
	for (int c = std::fgetc(file); c != EOF; c = std::fgetc(file))
		bytes.push_back(static_cast<std::byte>(c));
	std::fclose(file);
	static std::array<rt::RuntimeType, 256> types{};
	static std::array<rt::RuntimeMember, 512> members{};
	static std::array<rt::RuntimeEnumEntry, 512> enums{};
	static std::array<rt::RuntimeCatalog, 256> catalogs{};
	static std::array<rt::RuntimeEndpoint, 256> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(bytes, workspace);
	check(static_cast<bool>(parsed), "RuntimeSchema parses the descriptor");
	if (!parsed)
		return 1;
	const auto& schema = parsed.schema;
	check(schema.minorVersion() == meta::wireMinor, "minor");
	for (const auto& record : meta::typeRecords) {
		check(schema.typeName(record.id) == record.name, "type name", record.name);
		if (!record.name.empty())
			check(schema.findType(record.name) && schema.findType(record.name)->id == record.id,
			      "findType", record.name);
	}
	std::array<char, 4096> path{};
	for (const auto& record : meta::endpoints) {
		const auto category = static_cast<::telemetry::descriptor::Category>(record.category);
		const ::telemetry::runtime::RuntimeEndpoint* found = nullptr;
		for (const auto& endpoint : schema.endpoints(category))
			if (endpoint.id == record.id)
				found = &endpoint;
		check(found != nullptr, "endpoint by id", record.name);
		if (!found)
			continue;
		const auto size = schema.endpointPath(*found, path);
		check(std::string_view{path.data(), size} == record.path, "endpointPath", record.path);
		const auto* resolved = schema.findEndpoint(record.path);
		check(resolved == found, "findEndpoint(path)", record.path);
	}
@STATIC@	std::printf("names parity checks=%d failures=%d\\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
"""


def parity_source(schema, reachable):
    """Static names: typeName<T>() of every generated enum/struct and its public alias."""
    rows = ["// Generated by tests/codegen/names.py. SPDX-License-Identifier: MIT\n",
            _include(schema, "Types"), _include(schema, "TelemetryBindings"), _include(schema, "Metadata")]
    static = []
    for type_id in reachable:
        item = schema.types[type_id]
        if item["kind"] not in (2, 3):
            continue
        native = _type(schema, type_id)
        static.append(f"static_assert(::telemetry::reflection::typeName<{native}>() == "
                      f"::{schema.namespace}::metadata::typeRecords[{type_id}].name);\n")
        symbol = schema.type_symbol(type_id)
        if symbol:
            static.append(f"static_assert(std::is_same_v<::{schema.namespace}::{symbol.cpp}, {native}>);\n")
    body = PARITY_MAIN.replace("@NS@", schema.namespace).replace("@STATIC@", "")
    head, tail = body.split("int main(int argc, char** argv)", 1)
    return "".join(rows) + head + "".join(static) + "\nint main(int argc, char** argv)" + tail


def alias_source(schema):
    """Every endpoint of a flat model's SDK in the group form, used the way static client code uses it."""
    ns = "::" + schema.namespace
    rows = ["// Generated by tests/codegen/names.py: code written for the flat SDK's group form.\n",
            _include(schema, "Codec"), "#include <array>\n#include <cstddef>\n#include <cstdio>\n\n",
            "int main()\n{\n\tint failures = 0;\n"]
    for endpoint in schema.endpoints:
        constant = f"{ns}::{endpoint.group}::{endpoint.symbol}"
        rows.append(f"\tstatic_assert({constant}.id == {endpoint.id}u);\n")
        if endpoint.category == 1:
            alias = f"{ns}::{endpoint.group}::types::{endpoint.value_alias}"
            rows.append(f"\t{{ {alias} value{{}}; std::array<std::byte, {ns}::codec::wireSize<{alias}>> out{{}};\n"
                        f"\t  failures += {ns}::codec::encode({constant}, value, out) != {ns}::codec::Status::Ok; }}\n")
        else:
            alias = f"{ns}::{endpoint.group}::types::{endpoint.request_alias}"
            rows.append(f"\tstatic_assert(std::is_same_v<{alias}, typename decltype({constant})::Request>);\n")
            if endpoint.category == 3:
                response = f"{ns}::{endpoint.group}::types::{endpoint.response_alias}"
                rows.append(f"\tstatic_assert(std::is_same_v<{response}, typename decltype({constant})::Response>);\n")
    rows.append('\tstd::printf("flat aliases checks=%d failures=%d\\n", ' + str(len(schema.endpoints)) +
                ", failures);\n\treturn failures;\n}\n")
    return "".join(rows)


def alias_typescript(schema):
    """The flat SDK's TS symbols in the group form, as static client code uses them."""
    rows = ["import * as api from './ts/device.js';"]
    for number, endpoint in enumerate(schema.endpoints):
        group = endpoint.group.replace("::", ".")
        rows.append(f"const e{number}: number = api.{group}.{endpoint.symbol}.id;")
        aliases = [endpoint.value_alias, endpoint.request_alias, endpoint.response_alias]
        for alias in filter(None, aliases):
            rows.append(f"type t{number}_{alias} = api.{endpoint.group}.types.{alias};")
            rows.append(f"const v{number}_{alias} = api.{endpoint.group}.types.{alias};")
    return "\n".join(rows) + "\n"


def tree_source(schema):
    """The group form in C++: each endpoint constant beside its subgroups carries its
    descriptor ID and value type, and a named value type is the public types:: alias."""
    ns = "::" + schema.namespace
    rows = ["// Generated by tests/codegen/names.py: the group tree's C++ form.\n",
            _include(schema, "Codec"), "#include <array>\n#include <cstddef>\n#include <cstdio>\n\n",
            "int main()\n{\n\tint failures = 0;\n"]
    for endpoint, symbol in zip(schema.endpoints, schema.tree_symbols):
        direct = f"{ns}::{endpoint.group}::{symbol}"
        rows.append(f"\tstatic_assert({direct}.id == {endpoint.id}u && {direct}.category == "
                    f"{ns}::endpoint::Category{{{endpoint.category}}});\n")
        value = endpoint.value_type if endpoint.category == 1 else endpoint.request_type
        named = schema.type_symbol(value)
        if named:
            rows.append(f"\tstatic_assert(std::is_same_v<{ns}::{named.cpp}, {ns}::detail::Type_{value}>);\n")
        if endpoint.category == 1:
            rows.append(f"\t{{ typename decltype({direct})::Value value{{}}; std::array<std::byte, {schema.types[value]['wire']}> out{{}};\n"
                        f"\t  failures += {ns}::codec::encode({direct}, value, out) != {ns}::codec::Status::Ok; }}\n")
    rows.append('\tstd::printf("tree form checks=%d failures=%d\\n", ' + str(len(schema.endpoints)) +
                ", failures);\n\treturn failures;\n}\n")
    return "".join(rows)


def handles_source(schema):
    """The generated device of any v3.1 model: every path form is its nested handle."""
    from tools.telemetry_codegen.generators.handles import handle_tree
    _, leaves = handle_tree(schema)
    ns = "::" + schema.namespace
    rows = ["// Generated by tests/codegen/names.py: endpoint handles of the generated device.\n",
            _include(schema, "ClientBindings"), "#include <integration/encoded/Backend.hpp>\n#include <cstdio>\n\n",
            "std::uint32_t now = 0;\nint main()\n{\n\tint failures = 0;\n",
            "\tstd::array<std::byte, 64> tx{}, rx{};\n",
            "\tconst ::telemetry::client::Clock clock{nullptr, +[](void*) noexcept { return now; }};\n",
            "\t::telemetry::integration::encoded::EncodedBackend backend{tx, rx, clock};\n",
            f"\t{ns}::client::DeviceClient<> client{{backend, clock}};\n",
            f"\tconst {ns}::client::Device<{ns}::client::DeviceClient<>> device{{client}};\n"]
    for leaf in leaves:
        if not leaf.registered:
            continue
        path = _string(leaf.path)
        rows.append(f"\tstatic_assert(std::is_same_v<decltype(device.endpoint<{path}>()), decltype(device.{leaf.access})>);\n")
        rows.append(f"\tfailures += !(device.endpoint<{path}>() == device.{leaf.access} && "
                    f"{ns}::client::endpoint<{path}>(device).id() == {leaf.endpoint.id}u);\n")
    count = sum(leaf.registered for leaf in leaves)
    rows.append(f'\tstd::printf("device handles checks=%d failures=%d\\n", {count}, failures);\n\treturn failures;\n}}\n')
    return "".join(rows)


def usage_typescript(schema):
    """Nested access with the SDK's own client: what an editor completes, strictly typed."""
    rows = ["import * as api from './ts/device.js';", "import type {DeviceClient} from './ts/client.js';",
            "export async function usage(client: DeviceClient): Promise<void> {"]
    for number, (endpoint, symbol) in enumerate(zip(schema.endpoints, schema.tree_symbols)):
        group = "api." + endpoint.group.replace("::", ".")
        direct = f"{group}.{symbol}"
        if endpoint.category == 1:
            value = f"{group}.types.{endpoint.value_alias}"
            rows.append(f"  const r{number} = await client.read({direct});")
            rows.append(f"  const v{number}: {value} | undefined = r{number}.value;")
            if endpoint.capabilities == 3:
                rows.append(f"  if (v{number} !== undefined) await client.write({direct}, v{number});")
            named = schema.type_symbol(endpoint.value_type)
            if named:
                rows.append(f"  const n{number}: api.{named.js} | undefined = v{number};")
        elif endpoint.category == 2:
            request = f"{group}.types.{endpoint.request_alias}"
            rows.append(f"  await client.command({direct}, null as unknown as {request});")
        else:
            request = f"{group}.types.{endpoint.request_alias}"
            response = f"{group}.types.{endpoint.response_alias}"
            rows.append(f"  const s{number} = await client.service({direct}, null as unknown as {request});")
            rows.append(f"  const w{number}: {response} | undefined = s{number}.value;")
            rows.append(f"  void w{number};")
    rows.append("}")
    return "\n".join(rows) + "\n"


if __name__ == "__main__":
    if sys.argv[1:] != ["write-goldens"]:
        raise SystemExit("usage: names.py write-goldens")
    write_goldens()
