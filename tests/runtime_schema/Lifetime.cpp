/*
 * @file Lifetime.cpp
 * @brief Borrowed schema copy/move and invalidation before released backing storage.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/runtime/DynamicValueView.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace rt = telemetry::runtime;
unsigned checks = 0;

void check(bool ok, int line)
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "lifetime check %u line %d failed\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

struct Storage {
	std::unique_ptr<rt::RuntimeType[]> types{new rt::RuntimeType[128]{}};
	std::unique_ptr<rt::RuntimeMember[]> members{new rt::RuntimeMember[128]{}};
	std::unique_ptr<rt::RuntimeEnumEntry[]> enums{new rt::RuntimeEnumEntry[32]{}};
	std::unique_ptr<rt::RuntimeCatalog[]> catalogs{new rt::RuntimeCatalog[16]{}};
	std::unique_ptr<rt::RuntimeEndpoint[]> endpoints{new rt::RuntimeEndpoint[32]{}};
	rt::RuntimeSchemaWorkspace workspace{{types.get(), 128},
	                                     {members.get(), 128},
	                                     {enums.get(), 32},
	                                     {catalogs.get(), 16},
	                                     {endpoints.get(), 32}};

	void releaseMetadata()
	{
		types.reset();
		members.reset();
		enums.reset();
		catalogs.reset();
		endpoints.reset();
	}
};

std::vector<std::byte> load(const char* path)
{
	std::ifstream file(path, std::ios::binary);
	CHECK(file.good());
	std::vector<char> raw{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{}};
	std::vector<std::byte> bytes(raw.size());
	for (std::size_t i = 0; i < raw.size(); ++i)
		bytes[i] = std::byte(static_cast<unsigned char>(raw[i]));
	return bytes;
}

void stale(const rt::RuntimeSchema& schema, const rt::DynamicValueView& view,
           const rt::PreparedHandle& handle, telemetry::TypeId root)
{
	CHECK(!schema.valid());
	CHECK(schema.type(root) == nullptr);
	CHECK(schema.members(root).empty());
	CHECK(schema.enumEntries(handle.leafType()).empty());
	CHECK(schema.catalogs(telemetry::descriptor::Category::Field).empty());
	CHECK(schema.endpoints(telemetry::descriptor::Category::Service).empty());
	CHECK(!schema.findField("dynamic", "Packet"));
	CHECK(!schema.findField("dynamic.Packet"));
	CHECK(!schema.findCommand("dynamic", "Apply"));
	CHECK(!schema.findCommand("dynamic.Apply"));
	CHECK(!schema.findService("dynamic", "Inspect"));
	CHECK(!schema.findService("dynamic.Inspect"));
	CHECK(!schema.findEndpoint(telemetry::descriptor::Category::Field, "dynamic.Packet"));
	CHECK(schema.descriptor().empty());
	CHECK(schema.fingerprint() == 0);
	CHECK(!schema.accepts(handle));
	CHECK(schema.resolve(root, "u64").status == rt::Status::Stale);
	CHECK(schema.resolve(root, std::array{rt::PathStep::member("u64")}).status ==
	      rt::Status::Stale);
	CHECK(!view.valid());
	CHECK(view.bytes().status == rt::Status::Stale);
	CHECK(view.member("u64").status == rt::Status::Stale);
	CHECK(view.member(0).status == rt::Status::Stale);
	CHECK(view.index(0).status == rt::Status::Stale);
	CHECK(view.get<std::uint64_t>().status == rt::Status::Stale);
	CHECK(view.get<std::uint64_t>(handle).status == rt::Status::Stale);
	CHECK(view.enumUnderlying<std::uint64_t>(handle).status == rt::Status::Stale);
	CHECK(view.enumName(handle).status == rt::Status::Stale);
	CHECK(schema.view(root, {}).status == rt::Status::Stale);
	CHECK(rt::DynamicValueView::create(schema, root, {}).status == rt::Status::Stale);
}

void copyMove(const std::vector<std::byte>& descriptor, const std::vector<std::byte>& payload)
{
	static_assert(std::is_trivially_copyable_v<rt::RuntimeSchema>);
	static_assert(std::is_trivially_copyable_v<rt::ParseResult>);
	static_assert(std::is_trivially_copyable_v<rt::DynamicValueView>);
	static_assert(!std::is_move_constructible_v<rt::RuntimeSchemaWorkspace>);
	Storage storage;
	// RuntimeSchema returned directly from a destroyed ParseResult temporary.
	auto schema = rt::RuntimeSchema::parse(descriptor, storage.workspace).schema;
	CHECK(schema.valid());
	const auto root = schema.findField("dynamic", "Packet")->typeId;
	auto handle = schema.resolve(root, "u64").value;
	auto view = schema.view(root, payload).value;
	CHECK(view.get<std::uint64_t>(handle).value == UINT64_MAX);
	auto schemaCopy = schema;
	auto schemaMoved = std::move(schemaCopy);
	schemaCopy = {};
	CHECK(schemaMoved.owner() == &storage.workspace);
	CHECK(schemaMoved.view(root, payload).value.get<std::uint64_t>(handle).value == UINT64_MAX);
	rt::ParseResult assigned;
	{
		auto parsed = rt::RuntimeSchema::parse(descriptor, storage.workspace);
		CHECK(parsed);
		auto copied = parsed;
		auto moved = std::move(copied);
		assigned = std::move(moved);
		parsed = {};
		copied = {};
		moved = {};
	}
	CHECK(assigned);
	CHECK(assigned.schema.valid());
	CHECK(!schema.valid());
	CHECK(view.get<std::uint64_t>(handle).status == rt::Status::Stale);
	const auto next = assigned.schema.resolve(root, "u64").value;
	auto viewCopy = assigned.schema.view(root, payload).value;
	auto movedView = std::move(viewCopy);
	viewCopy = {};
	assigned = {};
	CHECK(movedView.get<std::uint64_t>(next).value == UINT64_MAX);
	CHECK(storage.workspace.reset() == rt::Status::Ok);
	CHECK(movedView.get<std::uint64_t>(next).status == rt::Status::Stale);
}

void releasedAfterReset(const std::vector<std::byte>& original,
                        const std::vector<std::byte>& payload)
{
	Storage storage;
	auto descriptor = std::make_unique<std::vector<std::byte>>(original);
	auto value = std::make_unique<std::vector<std::byte>>(payload);
	const auto schema = rt::RuntimeSchema::parse(*descriptor, storage.workspace).schema;
	CHECK(schema.valid());
	const auto root = schema.findField("dynamic", "Packet")->typeId;
	const auto handle = schema.resolve(root, "u64").value;
	const auto view = schema.view(root, *value).value;
	CHECK(view.get<std::uint64_t>(handle).value == UINT64_MAX);
	CHECK(storage.workspace.reset() == rt::Status::Ok);
	value.reset();
	descriptor.reset();
	storage.releaseMetadata();
	// The workspace itself is still alive. Freed backing memory must never
	// be touched by stale operations. No detection of a freed workspace is claimed.
	stale(schema, view, handle, root);
}

void failedReparse(const std::vector<std::byte>& original, const std::vector<std::byte>& payload)
{
	for (unsigned mode = 0; mode < 2; ++mode) {
		Storage storage;
		auto descriptor = std::make_unique<std::vector<std::byte>>(original);
		auto value = std::make_unique<std::vector<std::byte>>(payload);
		const auto parsed = rt::RuntimeSchema::parse(*descriptor, storage.workspace);
		CHECK(parsed);
		const auto root = parsed.schema.findField("dynamic", "Packet")->typeId;
		const auto handle = parsed.schema.resolve(root, "u64").value;
		const auto view = parsed.schema.view(root, *value).value;
		if (mode == 0) {
			const std::array<std::byte, 63> shortDescriptor{};
			CHECK(rt::RuntimeSchema::parse(shortDescriptor, storage.workspace).status ==
			      rt::Status::InvalidDescriptor);
		} else {
			auto* bytes = reinterpret_cast<const std::byte*>(storage.types.get());
			CHECK(rt::RuntimeSchema::parse({bytes, original.size()}, storage.workspace).status ==
			      rt::Status::Overlap);
		}
		descriptor.reset();
		value.reset();
		storage.releaseMetadata();
		stale(parsed.schema, view, handle, root);
	}
}

void foreign(const std::vector<std::byte>& descriptor, const std::vector<std::byte>& payload)
{
	Storage first, second;
	const auto a = rt::RuntimeSchema::parse(descriptor, first.workspace).schema;
	const auto b = rt::RuntimeSchema::parse(descriptor, second.workspace).schema;
	CHECK(a.valid() && b.valid());
	const auto root = a.findField("dynamic", "Packet")->typeId;
	CHECK(root == b.findField("dynamic", "Packet")->typeId);
	CHECK(a.generation() == b.generation());
	const auto ha = a.resolve(root, "u64").value;
	const auto hb = b.resolve(root, "u64").value;
	CHECK(!a.accepts(hb) && !b.accepts(ha));
	CHECK(a.view(root, payload).value.get<std::uint64_t>(hb).status == rt::Status::Stale);
	CHECK(b.view(root, payload).value.get<std::uint64_t>(ha).status == rt::Status::Stale);
	CHECK(first.workspace.reset() == rt::Status::Ok);
	first.releaseMetadata();
	CHECK(b.view(root, payload).value.get<std::uint64_t>(ha).status == rt::Status::Stale);
	CHECK(b.view(root, payload).value.get<std::uint64_t>(hb).value == UINT64_MAX);
}

int main(int argc, char** argv)
{
	if (argc != 3)
		return 2;
	auto descriptor = load(argv[1]);
	auto payload = load(argv[2]);
	copyMove(descriptor, payload);
	releasedAfterReset(descriptor, payload);
	failedReparse(descriptor, payload);
	foreign(descriptor, payload);

	std::printf("independent lifetime: %u checks, 0 failures\n", checks);
}
