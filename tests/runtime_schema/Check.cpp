/*
 * @file Check.cpp
 * @brief Runtime-only reads of actual Native codec bytes and checked metadata.
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/runtime/DynamicValueView.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
namespace rt = telemetry::runtime;
unsigned checks = 0;

void require(bool condition, int line)
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "runtime check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) require(bool(__VA_ARGS__), __LINE__)

template<std::size_t N>
std::size_t load(const char* root, const char* name, std::array<std::byte, N>& out)
{
	char path[4096];
	CHECK(std::snprintf(path, sizeof(path), "%s/%s", root, name) < int(sizeof(path)));
	auto* file = std::fopen(path, "rb");
	CHECK(file != nullptr);
	const auto size = std::fread(out.data(), 1, out.size(), file);
	CHECK(std::fgetc(file) == EOF);
	CHECK(std::fclose(file) == 0);
	return size;
}

struct Storage {
	std::array<rt::RuntimeType, 128> types{};
	std::array<rt::RuntimeMember, 128> members{};
	std::array<rt::RuntimeEnumEntry, 32> enums{};
	std::array<rt::RuntimeCatalog, 16> catalogs{};
	std::array<rt::RuntimeEndpoint, 32> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
};

void endpointLookup(const char* directory)
{
	using C = telemetry::descriptor::Category;
	std::array<std::byte, 65536> descriptor{};
	const auto size = load(directory, "lookup-descriptor.bin", descriptor);
	Storage storage;
	const auto parsed = rt::RuntimeSchema::parse({descriptor.data(), size}, storage.workspace);
	CHECK(parsed);
	const auto& schema = parsed.schema;
	for (const auto category : {C::Field, C::Command, C::Service}) {
		const auto* left = schema.findEndpoint(category, "a", "b.c");
		const auto* right = schema.findEndpoint(category, "a.b", "c");
		const auto* unique = schema.findEndpoint(category, "a.b.unique");
		CHECK(left && right && unique);
		CHECK(left != right && left->id == 0 && right->id == 65536);
		CHECK(unique->category == category && unique->id == 65537);
		CHECK(schema.findEndpoint(category, "a.b", "unique") == unique);
		CHECK(!schema.findEndpoint(category, "a.b.c"));
		CHECK(schema.findEndpoint(category, "a.\xCE\xBC") ==
		      schema.findEndpoint(category, "a", "\xCE\xBC"));
		CHECK(schema.findEndpoint(category, "a.\xCE\xBC"));
		CHECK(schema.findEndpoint(category, "\xCE\xB1.unique"));
		for (const auto path : {"", "a", "a.", "a.b", "a.b.Unique", "xa.b.unique", "a.b.uniquex",
		                        "a.b.unique.", "a.b.unique.member", "a.*"})
			CHECK(!schema.findEndpoint(category, path));
		CHECK(!schema.findEndpoint(category, "missing", "unique"));
		CHECK(!schema.findEndpoint(category, "a.b", "missing"));
	}
	CHECK(schema.findField("a.b.unique") == schema.findEndpoint(C::Field, "a.b", "unique"));
	CHECK(schema.findCommand("a.b.unique") == schema.findEndpoint(C::Command, "a.b", "unique"));
	CHECK(schema.findService("a.b.unique") == schema.findEndpoint(C::Service, "a.b", "unique"));
	CHECK(schema.findField("a", "b.c") == schema.findEndpoint(C::Field, "a", "b.c"));
	CHECK(schema.findCommand("a", "b.c") == schema.findEndpoint(C::Command, "a", "b.c"));
	CHECK(schema.findService("a", "b.c") == schema.findEndpoint(C::Service, "a", "b.c"));
	CHECK(!schema.findField("a.b.c"));
	CHECK(!schema.findCommand("a.b.c"));
	CHECK(!schema.findService("a.b.c"));
	CHECK(schema.findField("a.b.unique") != schema.findCommand("a.b.unique"));
	CHECK(schema.findCommand("a.b.unique") != schema.findService("a.b.unique"));
	for (const auto invalid : {static_cast<C>(0), static_cast<C>(4), static_cast<C>(255)}) {
		CHECK(!schema.findEndpoint(invalid, "a.b.unique"));
		CHECK(!schema.findEndpoint(invalid, "a.b", "unique"));
	}
	CHECK(storage.workspace.reset() == rt::Status::Ok);
	CHECK(!schema.findField("a.b.unique"));
	CHECK(!schema.findCommand("a.b", "unique"));
	CHECK(!schema.findService("a.b.unique"));
}

template<class T>
T read(const rt::RuntimeSchema& schema, const rt::DynamicValueView& view, telemetry::TypeId root,
       std::string_view path)
{
	const auto handle = schema.resolve(root, path);
	CHECK(handle);
	const auto result = view.get<T>(handle.value);
	CHECK(result);
	return result.value;
}

void execute(const char* directory)
{
	std::array<std::byte, 65536> descriptor{};
	std::array<std::byte, 4096> payload{}, variants{};
	const auto size = load(directory, "descriptor.bin", descriptor);
	const auto valueSize = load(directory, "payload.bin", payload);
	CHECK(load(directory, "variants.bin", variants) == valueSize);
	const rt::Input input{descriptor.data(), size};
	Storage storage;
	const auto parsed = rt::RuntimeSchema::parse(input, storage.workspace);
	CHECK(parsed);
	CHECK(parsed.schema.valid());
	const auto& schema = parsed.schema;
	for (const auto category :
	     {telemetry::descriptor::Category::Field, telemetry::descriptor::Category::Command,
	      telemetry::descriptor::Category::Service}) {
		const auto catalogs = schema.catalogs(category);
		const auto endpoints = schema.endpoints(category);
		CHECK(catalogs.size() == 1);
		CHECK(!endpoints.empty());
		for (const auto& item : endpoints) {
			CHECK(item.category == category);
			CHECK(item.catalogIndex < catalogs.size());
			const auto& catalog = catalogs[item.catalogIndex];
			CHECK(catalog.group == item.catalogIndex);
			CHECK((item.id >> 16) == catalog.group);
			CHECK(catalog.name == "dynamic");
			CHECK((item.id & 65535u) < catalog.endpointCount);
			CHECK(&endpoints[catalog.firstEndpoint + (item.id & 65535u)] == &item);
		}
	}
	const auto* endpoint = schema.findField("dynamic", "Packet");
	CHECK(endpoint);
	CHECK(endpoint->id == 0);
	const auto root = endpoint->typeId;
	const auto result = schema.view(root, {payload.data(), valueSize});
	CHECK(result);
	const auto view = result.value;
	CHECK(view.valid());
	CHECK(view.typeId() == root);
	CHECK(view.rootTypeId() == root);
	CHECK(view.wireOffset() == 0);
	CHECK(view.bytes().value.size() == valueSize);
	CHECK(read<bool>(schema, view, root, "flag"));
	CHECK(read<std::uint8_t>(schema, view, root, "u8") == UINT8_MAX);
	CHECK(read<std::int8_t>(schema, view, root, "i8") == INT8_MIN);
	CHECK(read<std::uint16_t>(schema, view, root, "u16") == UINT16_MAX);
	CHECK(read<std::int16_t>(schema, view, root, "i16") == INT16_MIN);
	CHECK(read<std::uint32_t>(schema, view, root, "u32") == UINT32_MAX);
	CHECK(read<std::int32_t>(schema, view, root, "i32") == INT32_MIN);
	CHECK(read<std::uint64_t>(schema, view, root, "u64") == UINT64_MAX);
	CHECK(read<std::int64_t>(schema, view, root, "i64") == INT64_MIN);
	CHECK(std::bit_cast<std::uint32_t>(read<float>(schema, view, root, "f32")) == 0x80000000u);
	CHECK(read<double>(schema, view, root, "f64") == INFINITY);
	CHECK(read<std::uint16_t>(schema, view, root, "nested.code") == 0x1234);
	CHECK(read<std::int8_t>(schema, view, root, "rows[1].bins[2]") == -3);
	CHECK(read<std::uint32_t>(schema, view, root, "matrix[0][1]") == 0xaabbccddu);
	CHECK(read<std::uint32_t>(schema, view, root, "matrix[1][0]") == 42);
	const auto nested = view.member("nested");
	CHECK(nested);
	CHECK(nested.value.member(0).value.get<std::uint16_t>().value == 0x1234);
	CHECK(nested.value.member("bins").value.index(2).value.get<std::int8_t>().value == 127);
	const auto mode = schema.resolve(root, "nested.mode");
	CHECK(mode);
	CHECK(view.enumUnderlying<std::int16_t>(mode.value).value == -7);
	CHECK(view.enumName(mode.value).value.known);
	CHECK(view.enumName(mode.value).value.name == "Negative");
	const auto unknown = view.member("rows").value.index(1).value.member("mode");
	CHECK(unknown);
	CHECK(unknown.value.enumUnderlying<std::int16_t>().value == -2);
	CHECK(unknown.value.enumName());
	CHECK(!unknown.value.enumName().value.known);
	CHECK(unknown.value.enumName().value.name.empty());
	const auto wide = schema.resolve(root, "wide");
	CHECK(wide);
	CHECK(view.enumUnderlying<std::uint64_t>(wide.value).value == UINT64_MAX);
	CHECK(view.enumName(wide.value).value.name == "High");
	CHECK(view.get<std::int16_t>(mode.value).status == rt::Status::TypeMismatch);
	CHECK(view.enumUnderlying<std::uint16_t>(mode.value).status == rt::Status::TypeMismatch);
	CHECK(view.member("u64").value.get<std::int64_t>().status == rt::Status::TypeMismatch);
	CHECK(view.get<std::uint64_t>().status == rt::Status::TypeMismatch);
	CHECK(view.member("missing").status == rt::Status::MemberNotFound);
	CHECK(view.member(999).status == rt::Status::IndexOutOfRange);
	CHECK(view.index(0).status == rt::Status::TypeMismatch);
	CHECK(view.member("zero").value.index(0).status == rt::Status::IndexOutOfRange);
	CHECK(view.member("empty").value.index(1).value.bytes().value.empty());
	CHECK(schema.resolve(root, "rows[2]").status == rt::Status::IndexOutOfRange);
	CHECK(!schema.resolve(root, "rows[-1]"));
	CHECK(!schema.resolve(root, "rows[4294967296]"));
	CHECK(!schema.resolve(root, "nested..code"));
	CHECK(!schema.resolve(root, "nested.code."));
	CHECK(!schema.resolve(root, ".nested"));
	CHECK(!schema.resolve(root, "matrix[0]junk"));
	CHECK(!schema.resolve(root, "rows[0"));
	CHECK(schema.resolve(99999, "flag").status == rt::Status::TypeNotFound);
	CHECK(rt::DynamicValueView::create(schema, 99999, {}).status == rt::Status::TypeNotFound);
	CHECK(rt::DynamicValueView::create(schema, 0, {}).status == rt::Status::TypeMismatch);
	CHECK(!schema.findField("other", "Packet"));
	const auto* empty = schema.findField("dynamic", "Empty");
	const auto* zero = schema.findField("dynamic", "Zero");
	CHECK(empty && zero);
	CHECK(schema.view(empty->typeId, {}));
	CHECK(schema.view(zero->typeId, {}));
	const auto* hugeZero = schema.findField("dynamic", "HugeZero");
	CHECK(hugeZero);
	const auto zeroView = schema.view(hugeZero->typeId, {});
	CHECK(zeroView);
	CHECK(zeroView.value.index(65535));
	CHECK(zeroView.value.index(65536).status == rt::Status::IndexOutOfRange);
	CHECK(schema.view(empty->typeId, {}).value.get<bool>(mode.value).status ==
	      rt::Status::TypeMismatch);
	CHECK(schema.view(root, {payload.data(), valueSize - 1}).status == rt::Status::InvalidPayload);
	CHECK(schema.view(root, {payload.data(), valueSize + 1}).status == rt::Status::InvalidPayload);
	const auto flag = schema.resolve(root, "flag");
	CHECK(flag);
	// These sequential edits are defensive validation probes. Applications
	// keep borrowed payload immutable; concurrent mutation is not supported.
	payload[flag.value.wireOffset()] = std::byte{2};
	CHECK(schema.view(root, {payload.data(), valueSize}).status == rt::Status::InvalidPayload);
	CHECK(view.get<bool>(flag.value).status == rt::Status::InvalidPayload);
	payload[flag.value.wireOffset()] = std::byte{1};
	const auto nestedBool = schema.resolve(root, "rows[1].ready");
	CHECK(nestedBool);
	payload[nestedBool.value.wireOffset()] = std::byte{255};
	CHECK(schema.view(root, {payload.data(), valueSize}).status == rt::Status::InvalidPayload);
	payload[nestedBool.value.wireOffset()] = std::byte{1};
	const auto variant = schema.view(root, {variants.data(), valueSize});
	CHECK(variant);
	CHECK(!read<bool>(schema, variant.value, root, "flag"));
	CHECK(std::bit_cast<std::uint32_t>(read<float>(schema, variant.value, root, "f32")) ==
	      0x7fc12345u);
	CHECK(read<double>(schema, variant.value, root, "f64") == -INFINITY);
	// The actual scalar starts at byte 3; add another byte to the whole payload
	// to exercise independent unaligned views with exact little-endian content.
	std::array<std::byte, 4097> unaligned{};
	std::memcpy(unaligned.data() + 1, payload.data(), valueSize);
	const auto shifted = schema.view(root, {unaligned.data() + 1, valueSize});
	CHECK(shifted);
	CHECK(read<std::uint16_t>(schema, shifted.value, root, "nested.code") == 0x1234);
	CHECK(read<std::uint64_t>(schema, shifted.value, root, "u64") == UINT64_MAX);
	// Every scalar is also decoded from an explicitly unaligned standalone
	// representation. This exercises the opposite integer extrema and F64 NaN.
	alignas(8) std::array<std::byte, 17> scalarBytes{};
	auto scalarCheck = [&]<class T>(telemetry::TypeId type, T expected) {
		using Bits = std::conditional_t<
		    sizeof(T) == 1, std::uint8_t,
		    std::conditional_t<sizeof(T) == 2, std::uint16_t,
		                       std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>;
		const Bits bits = std::bit_cast<Bits>(expected);
		for (std::size_t i = 0; i < sizeof(T); ++i)
			scalarBytes[i + 1] = std::byte((bits >> (8 * i)) & 255);
		const auto scalar = schema.view(type, {scalarBytes.data() + 1, sizeof(T)});
		CHECK(scalar);
		const auto actual = scalar.value.template get<T>();
		CHECK(actual);
		CHECK(std::bit_cast<Bits>(actual.value) == bits);
	};
	scalarCheck(1, false);
	scalarCheck(2, std::uint8_t{0});
	scalarCheck(3, std::int8_t{INT8_MAX});
	scalarCheck(4, std::uint16_t{0});
	scalarCheck(5, std::int16_t{INT16_MAX});
	scalarCheck(6, std::uint32_t{0});
	scalarCheck(7, std::int32_t{INT32_MAX});
	scalarCheck(8, std::uint64_t{0});
	scalarCheck(9, std::int64_t{INT64_MAX});
	scalarCheck(10, std::numeric_limits<float>::infinity());
	scalarCheck(11, std::bit_cast<double>(UINT64_C(0x7ff8123456789abc)));
	const std::array path{rt::PathStep::member("rows"), rt::PathStep::index(1),
	                      rt::PathStep::member("bins"), rt::PathStep::index(2)};
	const auto stepped = schema.resolve(root, path);
	CHECK(stepped);
	CHECK(view.get<std::int8_t>(stepped.value).value == -3);
	CHECK(view.get<bool>(rt::PreparedHandle{}).status == rt::Status::Stale);
	const auto matrix = schema.resolve(root, "matrix[0][0]");
	CHECK(matrix);
	CHECK(payload[matrix.value.wireOffset()] == std::byte{4});
	CHECK(payload[matrix.value.wireOffset() + 3] == std::byte{1});
	// A handle outside a navigated subtree is refused, even when both belong
	// to the same root. Another workspace and reparsing invalidate identity.
	CHECK(nested.value.get<std::uint64_t>(wide.value).status == rt::Status::InvalidPayload);
	Storage other;
	const auto second = rt::RuntimeSchema::parse(input, other.workspace);
	CHECK(second);
	CHECK(
	    second.schema.view(root, {payload.data(), valueSize}).value.get<bool>(flag.value).status ==
	    rt::Status::Stale);
	const auto expiredPayload = [&] {
		std::array<std::byte, 4096> local{};
		std::memcpy(local.data(), payload.data(), valueSize);
		return schema.view(root, {local.data(), valueSize}).value;
	}();
	CHECK(storage.workspace.reset() == rt::Status::Ok);
	CHECK(!schema.valid());
	CHECK(!view.valid());
	CHECK(view.get<bool>(flag.value).status == rt::Status::Stale);
	CHECK(view.member("flag").status == rt::Status::Stale);
	CHECK(view.bytes().status == rt::Status::Stale);
	CHECK(expiredPayload.get<bool>(flag.value).status == rt::Status::Stale);
	const auto again = rt::RuntimeSchema::parse(input, storage.workspace);
	CHECK(again);
	CHECK(again.schema.view(root, {payload.data(), valueSize}).value.get<bool>(flag.value).status ==
	      rt::Status::Stale);
	const auto freshHandle = again.schema.resolve(root, "flag");
	CHECK(freshHandle);
	const auto failed = rt::RuntimeSchema::parse(input.first(63), storage.workspace);
	CHECK(!failed);
	CHECK(!again.schema.valid());
	CHECK(!again.schema.accepts(freshHandle.value));
	// Capacity is counted before filling any caller table. Exact capacities
	// succeed; reducing each independently by one must report Capacity.
	const auto needed = parsed.requirements;
	for (unsigned which = 0; which < 5; ++which) {
		Storage bounded;
		bounded.types[0].id = 0x11223344;
		bounded.members[0].wireOffset = 0x55667788;
		auto types = std::span{bounded.types}.first(needed.types);
		auto members = std::span{bounded.members}.first(needed.members);
		auto enums = std::span{bounded.enums}.first(needed.enumEntries);
		auto catalogs = std::span{bounded.catalogs}.first(needed.catalogs);
		auto endpoints = std::span{bounded.endpoints}.first(needed.endpoints);
		CHECK(!types.empty() && !members.empty() && !enums.empty() && !catalogs.empty() &&
		      !endpoints.empty());
		if (which == 0)
			types = types.first(types.size() - 1);
		if (which == 1)
			members = members.first(members.size() - 1);
		if (which == 2)
			enums = enums.first(enums.size() - 1);
		if (which == 3)
			catalogs = catalogs.first(catalogs.size() - 1);
		if (which == 4)
			endpoints = endpoints.first(endpoints.size() - 1);
		rt::RuntimeSchemaWorkspace small{types, members, enums, catalogs, endpoints};
		const auto shortParse = rt::RuntimeSchema::parse(input, small);
		CHECK(shortParse.status == rt::Status::Capacity);
		CHECK(shortParse.requirements.types == needed.types);
		CHECK(shortParse.requirements.members == needed.members);
		CHECK(shortParse.requirements.enumEntries == needed.enumEntries);
		CHECK(shortParse.requirements.catalogs == needed.catalogs);
		CHECK(shortParse.requirements.endpoints == needed.endpoints);
		CHECK(bounded.types[0].id == 0x11223344);
		CHECK(bounded.members[0].wireOffset == 0x55667788);
	}
	Storage bounded;
	rt::RuntimeSchemaWorkspace exact{std::span{bounded.types}.first(needed.types),
	                                 std::span{bounded.members}.first(needed.members),
	                                 std::span{bounded.enums}.first(needed.enumEntries),
	                                 std::span{bounded.catalogs}.first(needed.catalogs),
	                                 std::span{bounded.endpoints}.first(needed.endpoints)};
	CHECK(rt::RuntimeSchema::parse(input, exact));
	Storage overlap;
	CHECK(size <= sizeof(overlap.types));
	std::memcpy(overlap.types.data(), descriptor.data(), size);
	CHECK(rt::RuntimeSchema::parse({reinterpret_cast<const std::byte*>(overlap.types.data()), size},
	                               overlap.workspace)
	          .status == rt::Status::Overlap);
	// Raw member names with punctuation remain addressable through typed
	// PathStep spans; dotted paths intentionally have their own syntax.
	std::array<std::byte, 65536> punctuated{};
	const auto punctuatedSize = load(directory, "punctuated.bin", punctuated);
	Storage punctuation;
	const auto special =
	    rt::RuntimeSchema::parse({punctuated.data(), punctuatedSize}, punctuation.workspace);
	CHECK(special);
	const std::array rawPath{rt::PathStep::member("a.b")};
	const auto rawHandle = special.schema.resolve(12, rawPath);
	CHECK(rawHandle);
	CHECK(!special.schema.resolve(12, "a.b"));
	const std::array oneByte{std::byte{42}};
	CHECK(special.schema.view(12, oneByte).value.get<std::uint8_t>(rawHandle.value).value == 42);
}
} // namespace

int main(int argc, char** argv)
{
#ifdef RUNTIME_HEAP_CONTROL
	volatile auto* allocated = new int(42);
	delete allocated;
#endif
	if (argc != 2)
		return 2;
	endpointLookup(argv[1]);
	execute(argv[1]);
	std::printf("runtime schema: %u checks, 0 failures\n", checks);
	std::printf(
	    "runtime layout: Schema=%zu View=%zu Workspace=%zu Handle=%zu Type=%zu Member=%zu EnumEntry=%zu Catalog=%zu Endpoint=%zu\n",
	    sizeof(rt::RuntimeSchema), sizeof(rt::DynamicValueView), sizeof(rt::RuntimeSchemaWorkspace),
	    sizeof(rt::PreparedHandle), sizeof(rt::RuntimeType), sizeof(rt::RuntimeMember),
	    sizeof(rt::RuntimeEnumEntry), sizeof(rt::RuntimeCatalog), sizeof(rt::RuntimeEndpoint));
	return 0;
}
