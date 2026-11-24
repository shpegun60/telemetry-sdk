// Preserve ID widths until validation, including intentional runtime termination (MIT).
// Checks that transport identifiers retain every high bit until range validation finishes.
// Invalid runtime identifiers must neither select another row nor modify output or application state.

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include "SharedSupport.hpp"
#include <atomic>
#include <limits>
using namespace telemetry;
#ifndef TELEMETRY_ID_BOUNDARY_ABORT_CASE
#define TELEMETRY_ID_BOUNDARY_ABORT_CASE 0
#endif
enum class Position : std::uint64_t {
	First,
	TooWide = 0x100000001ULL
};
enum class Group : std::uint64_t {
	First,
	Sensor
};
float value = 21.5f;
unsigned calls = 0, writes = 0;

float readValue() noexcept
{
	return value;
}

WriteResult writeValue(float next) noexcept
{
	++writes;
	value = next;
	return WriteResult::Applied;
}

CommandResult callValue() noexcept
{
	++calls;
	return CommandResult::Executed;
}

constexpr FieldTable rows{field<&readValue, &writeValue>("value")};
[[maybe_unused]] constexpr testing::FieldCatalogTable fields{testing::group("first", rows),
                                                             testing::group("sensor", rows)};
constexpr CommandTable local{command<&callValue>("run")};
[[maybe_unused]] constexpr testing::CommandCatalogTable commands{testing::group("first", local),
                                                                 testing::group("sensor", local)};
#if TELEMETRY_ID_BOUNDARY_ABORT_CASE == 6
[[maybe_unused]] const PackedId invalidBeforeMain = detail::makeId(0, 70000);
#endif
int main()
{
#if TELEMETRY_ID_BOUNDARY_ABORT_CASE == 1
	volatile std::uint64_t id = 0x100020001ULL;
	return groupOf(id);
#elif TELEMETRY_ID_BOUNDARY_ABORT_CASE == 2
	volatile std::uint64_t id = 0x100020001ULL;
	return indexOf(id);
#elif TELEMETRY_ID_BOUNDARY_ABORT_CASE == 3
	volatile int id = -1;
	return groupOf(id);
#elif TELEMETRY_ID_BOUNDARY_ABORT_CASE == 4
	volatile int id = -1;
	return indexOf(id);
#elif TELEMETRY_ID_BOUNDARY_ABORT_CASE == 5
	return static_cast<int>(detail::makeId(0, 65537));
#else
	static_assert(detail::makeId(65535, 65535) == UINT32_MAX);
	static_assert(!detail::tryMakeId(0, 65536) && !detail::tryMakeId(-1, 0));
	static_assert(detail::tryMakeId(Group::Sensor, Position::First) == detail::makeId<1, 0>());
	CHECK(rows.read<Position::First>() == 21.5f);
	CHECK(rows.write<Position::First>(22.f) == WriteResult::Applied);
	CHECK(local.call<Position::First>() == CommandResult::Executed);
	CHECK(fields.read<UINT64_C(65536)>() == 22.f);
	CHECK(fields.write<INT64_C(65536)>(23.f) == WriteResult::Applied);
	CHECK(commands.call<UINT64_C(65536)>() == CommandResult::Executed);
	CHECK(groupOf(std::int64_t{65536}) == 1 && indexOf(std::int64_t{65536}) == 0);
	CHECK(tryGroupOf(0xffffffffULL) == 65535 && tryIndexOf(0xffffffffULL) == 65535);
	Workspace workspace{std::span<std::byte>{}};
	std::array<std::byte, 4> bytes{};
	const auto invalid = [&](auto id) {
		const auto beforeCalls = calls, beforeWrites = writes;
		CHECK(fields.index().find(id) == nullptr && commands.index().find(id) == nullptr);
		CHECK(!fields.readAs<float>(id));
		CHECK(fields.writeAs(id, 25.f) == WriteResult::NotFound);
		CHECK(!fields.visit(id, [](const auto&) noexcept {
		}));
		CHECK(!commands.visit(id, [](const auto&) noexcept {
		}));
		CHECK(fields.index().readEncoded(id, bytes, workspace).dispatch ==
		      DispatchStatus::NotFound);
		CHECK(fields.index().writeEncoded(id, bytes, workspace).dispatch ==
		      DispatchStatus::NotFound);
		CHECK(commands.index().executeEncoded(id, {}, workspace).dispatch ==
		      DispatchStatus::NotFound);
		CHECK(!tryGroupOf(id) && !tryIndexOf(id));
		CHECK(calls == beforeCalls && writes == beforeWrites && value == 23.f);
	};
	for (const auto id :
	     {UINT64_C(0x100000000), UINT64_C(0x100000001), UINT64_C(0x100020001), UINT64_MAX})
		invalid(id);
	invalid(-1);
	invalid(std::numeric_limits<std::int64_t>::min());
	std::atomic<PackedId> narrow{65536};
	CHECK(fields.readAs<float>(narrow.load()) == 23.f);
	CHECK(fields.writeAs(narrow.load(), 24.f) == WriteResult::Applied);
	CHECK(detail::tryMakeId(1, 65535) == 0x1ffffu && !detail::tryMakeId(65536, 0));
	reportChecks();
	return 0;
#endif
}
