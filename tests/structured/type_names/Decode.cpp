/*
 * @file Decode.cpp
 * @brief C++ RuntimeSchema side of the three-language descriptor v3.1 decoder table.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Reads a pack of named descriptors and prints, per case, the ParseError
 * verdict and, for an accepted descriptor, the canonical model dump shared
 * with decode.mjs and run.py. Names are printed as UTF-8 hex, enum codes as
 * raw little-endian bits, so the three dumps compare byte for byte. The name
 * API (typeName, findType, minorVersion) is checked against the same model.
 */

#include <telemetry/runtime/RuntimeSchema.hpp>

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace rt = telemetry::runtime;

int failures = 0;
int checks = 0;

void check(bool condition, const char* what)
{
	++checks;
	if (!condition) {
		++failures;
		std::printf("FAIL %s\n", what);
	}
}

const char* verdict(rt::ParseError error) noexcept
{
	switch (error) {
		case rt::ParseError::None:
			return "Ok";
		case rt::ParseError::Header:
			return "Header";
		case rt::ParseError::Limit:
			return "Limit";
		case rt::ParseError::Fingerprint:
			return "Fingerprint";
		case rt::ParseError::Record:
			return "Record";
		case rt::ParseError::Reserved:
			return "Reserved";
		case rt::ParseError::Utf8:
			return "Utf8";
		case rt::ParseError::TypeReference:
			return "TypeReference";
		case rt::ParseError::TypeShape:
			return "TypeShape";
		case rt::ParseError::EnumOrder:
			return "EnumOrder";
		case rt::ParseError::DuplicateName:
			return "DuplicateName";
		case rt::ParseError::Catalog:
			return "Catalog";
		case rt::ParseError::Endpoint:
			return "Endpoint";
		case rt::ParseError::Trailing:
			return "Trailing";
	}
	return "Unknown";
}

std::string hex(std::string_view text)
{
	if (text.empty())
		return "-";
	std::string result;
	char digits[3];
	for (unsigned char c : text) {
		std::snprintf(digits, sizeof digits, "%02x", c);
		result += digits;
	}
	return result;
}

void dump(const rt::RuntimeSchema& schema, std::size_t typeCount)
{
	std::printf("minor %u\n", static_cast<unsigned>(schema.minorVersion()));
	std::printf("fingerprint %016llx\n", static_cast<unsigned long long>(schema.fingerprint()));
	for (telemetry::TypeId id = 0; id < typeCount; ++id) {
		const auto& type = *schema.type(id);
		std::printf("type %u %u %u %s", id, static_cast<unsigned>(type.kind), type.wireBytes,
		            hex(type.name).c_str());
		switch (type.kind) {
			case telemetry::TypeKind::Void:
				std::printf(" V");
				break;
			case telemetry::TypeKind::Scalar:
				std::printf(" S %u", static_cast<unsigned>(type.scalarCode));
				break;
			case telemetry::TypeKind::Enum:
				std::printf(" E %u", type.relatedTypeId);
				for (const auto& entry : schema.enumEntries(id))
					std::printf(" %llx=%s", static_cast<unsigned long long>(entry.codeBits),
					            hex(entry.name).c_str());
				break;
			case telemetry::TypeKind::Struct:
				std::printf(" M");
				for (const auto& member : schema.members(id))
					std::printf(" %u=%s", member.typeId, hex(member.name).c_str());
				break;
			case telemetry::TypeKind::Array:
				std::printf(" A %u %u", type.relatedTypeId, type.elementCount);
				break;
		}
		std::printf("\n");
		// The name API agrees with the published row.
		check(schema.typeName(id) == type.name, "typeName(id) equals RuntimeType::name");
		if (!type.name.empty()) {
			const auto* found = schema.findType(type.name);
			check(found != nullptr && found->id == id, "findType(name) finds the named type");
		}
	}
	for (unsigned c = 1; c <= 3; ++c) {
		const auto category = static_cast<telemetry::descriptor::Category>(c);
		for (const auto& catalog : schema.catalogs(category))
			std::printf("catalog %u %u %u %u %s\n", c, catalog.group, catalog.firstEndpoint,
			            catalog.endpointCount, hex(catalog.name).c_str());
	}
	for (unsigned c = 1; c <= 3; ++c) {
		const auto category = static_cast<telemetry::descriptor::Category>(c);
		for (const auto& endpoint : schema.endpoints(category))
			std::printf("endpoint %u %u %u %u %u %s\n", c, endpoint.id, endpoint.typeId,
			            endpoint.responseTypeId, static_cast<unsigned>(endpoint.capabilities),
			            hex(endpoint.name).c_str());
	}
	std::array<char, 8192> path{};
	for (const auto& group : schema.groups()) {
		const auto size = schema.groupPath(group.group, path);
		const std::string_view text{path.data(), size};
		std::printf("group %u %u ", group.group, group.depth);
		if (group.parent == rt::noParentGroup)
			std::printf("- %s\n", hex(text).c_str());
		else
			std::printf("%u %s\n", group.parent, hex(text).c_str());
		check(schema.findGroup(text) == &group, "findGroup(groupPath) finds the group");
	}
	for (unsigned c = 1; c <= 3; ++c) {
		const auto category = static_cast<telemetry::descriptor::Category>(c);
		for (const auto& endpoint : schema.endpoints(category)) {
			const auto size = schema.endpointPath(endpoint, path);
			const std::string_view text{path.data(), size};
			std::printf("path %u %u %s\n", c, endpoint.id, hex(text).c_str());
			const auto* found = schema.findEndpoint(text);
			check(!schema.groupTree() || found == &endpoint,
			      "findEndpoint(endpointPath) finds the endpoint in a tree");
			check(found == nullptr || found == &endpoint,
			      "findEndpoint(endpointPath) finds this endpoint or none");
			const auto target = schema.resolvePath(text);
			check(found == nullptr || (target && target.value.endpoint == &endpoint),
			      "resolvePath(endpointPath) finds the endpoint");
		}
	}
	check(schema.findType({}) == nullptr, "findType(empty) finds nothing");
	check(schema.findType("NoSuchTypeName") == nullptr, "findType(unknown) finds nothing");
	check(schema.typeName(static_cast<telemetry::TypeId>(typeCount)).empty(),
	      "typeName(out of range) is empty");
}

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	std::FILE* file = std::fopen(argv[1], "rb");
	if (!file)
		return 2;
	std::vector<unsigned char> pack;
	for (int c; (c = std::fgetc(file)) != EOF;)
		pack.push_back(static_cast<unsigned char>(c));
	std::fclose(file);

	std::array<rt::RuntimeType, 64> types{};
	std::array<rt::RuntimeMember, 64> members{};
	std::array<rt::RuntimeEnumEntry, 64> enums{};
	std::array<rt::RuntimeCatalog, 64> catalogs{};
	std::array<rt::RuntimeEndpoint, 64> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};

	const auto u32 = [&](std::size_t at) {
		return static_cast<std::size_t>(pack[at]) | static_cast<std::size_t>(pack[at + 1]) << 8 |
		       static_cast<std::size_t>(pack[at + 2]) << 16 |
		       static_cast<std::size_t>(pack[at + 3]) << 24;
	};
	std::size_t cursor = 0, cases = 0;
	while (cursor + 4 <= pack.size()) {
		const auto nameBytes = u32(cursor);
		const std::string name(reinterpret_cast<const char*>(pack.data() + cursor + 4), nameBytes);
		cursor += 4 + nameBytes;
		const auto size = u32(cursor);
		cursor += 4;
		std::vector<std::byte> bytes(size);
		for (std::size_t i = 0; i < size; ++i)
			bytes[i] = static_cast<std::byte>(pack[cursor + i]);
		cursor += size;
		const auto result = rt::RuntimeSchema::parse(bytes, workspace);
		std::printf("case %s %s\n", name.c_str(), verdict(result.error));
		if (result)
			dump(result.schema, result.requirements.types);
		else
			check(result.status == rt::Status::InvalidDescriptor, "refusal is InvalidDescriptor");
		++cases;
	}
	std::printf("C++ decoder: %zu cases, %d checks, %d failures\n", cases, checks, failures);
	return failures == 0 && cursor == pack.size() ? 0 : 1;
}
