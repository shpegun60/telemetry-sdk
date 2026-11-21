// An explicitly named template type must not narrow an ID before validation.
// Authors: Ruslan Kovtun (shpegun60), codexAi. License: MIT.
// Each CASE=1..9 must fail at compile time. These calls once selected rows by
// converting wide, enum or class-type values to narrow explicit template types.
//   1 index.find<std::uint16_t>(runtime 65537)               would narrow to 1
//   2 index.find<FieldId>(WireId{2^32 + 1})                  class type, u64 narrowed to 1
//   3 index.read<float, FieldId>(unscoped enum packed ID)    enum accepted as packed ID
//   4 commands.index().execute<unsigned short>(WireId{2^32 + 1}, ...)
//   5 table.call<std::uint16_t>(runtime 65536)               local position 65536 -> row 0
//   6 index.catalog<GroupId>(runtime 65536)                  group 65536 -> group 0
// Checks that explicit template arguments cannot bypass full-width transport identifier validation.
// Each selected case exercises a public access route before any narrowing or local indexing.

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <cstdint>
#include <cstdio>

using namespace telemetry;

namespace {
int hits[2] = {};

float r0() noexcept
{
	return 10.f;
}

float r1() noexcept
{
	return 11.f;
}

CommandResult c0() noexcept
{
	++hits[0];
	return CommandResult::Executed;
}

CommandResult c1() noexcept
{
	++hits[1];
	return CommandResult::Executed;
}

constexpr FieldTable g0{field<&r0>("a"), field<&r1>("b")};
constexpr testing::FieldCatalogTable fields{testing::group("g0", g0)};
constexpr CommandTable k0{command<&c0>("c0"), command<&c1>("c1")};
constexpr testing::CommandCatalogTable commands{testing::group("k0", k0)};

// User-defined wide identifier conversion used to check exact public argument contracts.
// Public methods:
// - operator std::uint64_t(): Expose wide value.
struct WireId {
	std::uint64_t value;

	constexpr operator std::uint64_t() const noexcept
	{
		return value;
	}
};

enum PackedIds : std::uint32_t {
	SecondRow = 1
};
} // namespace

#ifdef PACKING
int packing(int argc);
#endif
int main(int argc, char**)
{
#ifdef PACKING
	return packing(argc);
#endif
	const auto index = fields.index();
	const unsigned runtime = static_cast<unsigned>(argc); // 1 at run time
	const WireId wire{(std::uint64_t{1} << 32) | runtime};
	bool reached = false;
	Workspace workspace{std::span<std::byte>{}};
	(void)workspace;
#if CASE == 1
	const FieldEntry* f = index.find<std::uint16_t>(65536 + static_cast<int>(runtime));
	reached = f != nullptr;
	std::printf("find<uint16_t>(65537): %s\n", f ? f->name : "null");
#elif CASE == 2
	const FieldEntry* f = index.find<FieldId>(wire);
	reached = f != nullptr;
	std::printf("find<FieldId>(2^32+1): %s\n", f ? f->name : "null");
#elif CASE == 3
	const auto v = fields.readAs<float, FieldId>(SecondRow);
	reached = v.has_value();
	std::printf("read<float, FieldId>(unscoped enum 1): %g\n",
	            static_cast<double>(v.value_or(-1.f)));
#elif CASE == 4
	const CommandResult result =
	    commands.index().executeEncoded<unsigned short>(wire, {}, workspace).endpointStatus;
	reached = result == CommandResult::Executed;
	std::printf("execute<unsigned short>(2^32+1): %d hits=%d,%d\n", static_cast<int>(result),
	            hits[0], hits[1]);
#elif CASE == 5
	const CommandResult result = k0.visit<std::uint16_t>(65535u + runtime,
	                                                     [](const auto&) noexcept {
	                                                     })
	                                 ? CommandResult::Executed
	                                 : CommandResult::NotFound;
	reached = result == CommandResult::Executed;
	std::printf("call<uint16_t>(65536): %d hits=%d,%d\n", static_cast<int>(result), hits[0],
	            hits[1]);
#elif CASE == 6
	const bool g = fields.visit<GroupId>(65535u + runtime, [](const auto&) noexcept {
	});
	reached = g;
	std::printf("catalog<GroupId>(65536): %s\n", g ? "found" : "null");
#endif
	(void)wire;
	(void)index;
	return reached ? 1 : 0;
}

// Same mechanism for the packing helpers (-DCASE=7..9 with -DPACKING):
//   7 detail::makeId<std::uint16_t, std::uint16_t>(runtime 65537, 0)   wraps to 1
//   8 detail::tryMakeId<std::uint16_t, std::uint16_t>(runtime 65537, 0) returns a value
//   9 groupOf<std::uint32_t>(runtime 2^32 + (2 << 16))          returns group 2
#ifdef PACKING
int packing(int argc)
{
	const unsigned runtime = static_cast<unsigned>(argc);
#if CASE == 7
	const PackedId id = detail::makeId<std::uint16_t, std::uint16_t>(65536u + runtime, 0u);
	std::printf("detail::makeId<u16,u16>(65537, 0) = %08lx\n", static_cast<unsigned long>(id));
	return id == detail::makeId(GroupId{1}, EntryOffset{0}) ? 1 : 0;
#elif CASE == 8
	const auto id = detail::tryMakeId<std::uint16_t, std::uint16_t>(65536u + runtime, 0u);
	std::printf("detail::tryMakeId<u16,u16>(65537, 0) = %s\n", id ? "value" : "nullopt");
	return id ? 1 : 0;
#elif CASE == 9
	const std::uint64_t wide = (std::uint64_t{1} << 32) | (std::uint64_t{2} << 16) | runtime;
	const GroupId g = groupOf<std::uint32_t>(wide);
	std::printf("groupOf<u32>(2^32 + 0x20001) = %u\n", static_cast<unsigned>(g));
	return g == 2 ? 1 : 0;
#else
	(void)runtime;
	return 0;
#endif
}
#endif
