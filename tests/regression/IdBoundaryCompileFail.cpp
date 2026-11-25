// Invalid ID categories must fail before any implicit narrowing conversion.
// CASE0 verifies local enum positions and explicit integral extraction.
// Authors: Ruslan Kovtun (shpegun60), codexAi. License: MIT.
// Checks compile-time refusal of invalid identifier kinds, widths and local positions.
// Case zero supplies positive controls so failures are tied to the identifier contract being exercised.

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <atomic>

#ifndef TELEMETRY_ID_BOUNDARY_FAIL_CASE
#define TELEMETRY_ID_BOUNDARY_FAIL_CASE 0
#endif

using namespace telemetry;

namespace id_boundary_rejection {
enum class Position : std::uint64_t {
	First,
	TooWide = 0x100000001ULL
};

enum LegacyPosition {
	LegacyFirst
};

float readValue() noexcept
{
	return 1.f;
}

WriteResult writeValue(float) noexcept
{
	return WriteResult::Applied;
}

CommandResult run() noexcept
{
	return CommandResult::Executed;
}

constexpr FieldTable rows{field<&readValue, &writeValue>("value")};
constexpr testing::FieldCatalogTable fields{testing::group("fields", rows)};
constexpr const auto& fixed = fields;
constexpr CommandTable local{command<&run>("run")};
[[maybe_unused]] constexpr testing::CommandCatalogTable commands{testing::group("commands", local)};

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

// User-defined narrow identifier wrapper that must not be treated as an integral transport value.
// Public methods:
// - operator FieldId(): Expose narrowed value.
struct NarrowId {
	FieldId value;

	constexpr operator FieldId() const noexcept
	{
		return value;
	}
};

constexpr WireId wire{0x100000000ULL};
[[maybe_unused]] constexpr NarrowId narrow{0};
std::atomic<std::uint64_t> atomicWide{wire.value};
std::atomic<FieldId> atomicNarrow{0};
} // namespace id_boundary_rejection

using namespace id_boundary_rejection;

int main()
{
	Workspace workspace{std::span<std::byte>{}};
	const auto visitor = [](const auto&) noexcept {
	};
	(void)workspace;
	(void)visitor;
#if TELEMETRY_ID_BOUNDARY_FAIL_CASE == 1
	(void)fields.index().find(Position::First);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 2
	(void)fields.index().readEncoded(Position::First, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 3
	(void)fields.readAs<float>(Position::First);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 4
	(void)fields.writeAs(Position::First, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 5
	(void)fields.index().find(Position::First);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 6
	(void)fields.index().readEncoded(Position::First, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 7
	(void)fields.readAs<float>(Position::First);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 8
	(void)fields.writeAs(Position::First, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 9
	(void)fixed.index().find(Position::First);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 10
	(void)fixed.index().readEncoded(Position::First, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 11
	(void)fixed.readAs<float>(Position::First);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 12
	(void)fixed.writeAs(Position::First, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 13
	(void)commands.index().find(Position::First);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 14
	(void)commands.index().executeEncoded(Position::First, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 15
	(void)commands.visit(Position::First, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 16
	(void)commands.index().find(Position::First);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 17
	(void)commands.index().executeEncoded(Position::First, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 18
	(void)commands.visit(Position::First, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 19
	(void)groupOf(Position::First);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 20
	(void)indexOf(Position::First);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 21
	(void)fields.index().find(LegacyFirst);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 22
	(void)commands.index().executeEncoded(LegacyFirst, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 23
	(void)fields.index().find(wire);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 24
	(void)fields.index().readEncoded(wire, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 25
	(void)fields.readAs<float>(wire);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 26
	(void)fields.writeAs(wire, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 27
	(void)fields.visit(wire, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 28
	(void)fields.index().find(wire);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 29
	(void)fields.index().readEncoded(wire, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 30
	(void)fields.readAs<float>(wire);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 31
	(void)fields.writeAs(wire, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 32
	(void)fixed.index().find(wire);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 33
	(void)fixed.index().readEncoded(wire, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 34
	(void)fixed.readAs<float>(wire);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 35
	(void)fixed.writeAs(wire, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 36
	(void)fields.visit(wire, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 37
	(void)commands.index().find(wire);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 38
	(void)commands.index().executeEncoded(wire, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 39
	(void)commands.visit(wire, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 40
	(void)commands.visit(wire, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 41
	(void)commands.index().find(wire);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 42
	(void)commands.index().executeEncoded(wire, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 43
	(void)commands.visit(wire, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 44
	(void)local.visit(wire, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 45
	(void)local.visit(wire, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 46
	(void)local.visit(wire, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 47
	(void)local.visit(wire, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 48
	(void)groupOf(wire);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 49
	(void)indexOf(wire);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 50
	(void)detail::makeId(wire, 0);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 51
	(void)fields.index().find(atomicWide);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 52
	(void)fields.index().readEncoded(atomicWide, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 53
	(void)fields.readAs<float>(atomicWide);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 54
	(void)fields.writeAs(atomicWide, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 55
	(void)fields.index().find(atomicWide);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 56
	(void)fixed.index().find(atomicWide);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 57
	(void)commands.index().executeEncoded(atomicWide, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 58
	(void)local.visit(atomicWide, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 59
	(void)local.visit(atomicWide, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 60
	(void)groupOf(atomicWide);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 61
	(void)indexOf(atomicWide);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 62
	(void)fields.visit(atomicWide, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 63
	(void)fields.index().find(0.0);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 64
	(void)local.visit(0.0, visitor);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 65
	constexpr auto invalid = groupOf(0x100020001ULL);
	(void)invalid;
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 66
	constexpr auto invalid = indexOf(0x100020001ULL);
	(void)invalid;
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 67
	constexpr auto invalid = groupOf(-1);
	(void)invalid;
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 68
	constexpr auto invalid = indexOf(-1);
	(void)invalid;
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 69
	(void)detail::makeId<-1, 0>();
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 70
	(void)detail::makeId<0, 65536>();
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 71
	(void)detail::makeId<65536, 0>();
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 72
	(void)detail::makeId<0, Position::TooWide>();
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 73
	constexpr auto invalid = detail::makeId(0, 65536);
	(void)invalid;
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 74
	constexpr auto invalid = detail::makeId(-1, 0);
	(void)invalid;
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 75
	(void)detail::makeId(0.0, 0);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 76
	(void)detail::tryMakeId(wire, 0);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 77
	(void)fields.index().find(atomicNarrow);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 78
	(void)fields.readAs<float>(atomicNarrow);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 79
	(void)fields.writeAs(atomicNarrow, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 80
	(void)commands.index().executeEncoded(atomicNarrow, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 81
	(void)fields.index().find(narrow);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 82
	(void)fields.readAs<float>(narrow);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 83
	(void)fields.writeAs(narrow, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 84
	(void)commands.index().executeEncoded(narrow, {}, workspace);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 85
	(void)testing::CatalogAccess::read<Position::First>(fields);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 86
	(void)testing::CatalogAccess::readAs<float, Position::First>(fields);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 87
	(void)testing::CatalogAccess::write<Position::First>(fields, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 88
	(void)fixed.read<Position::First>();
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 89
	(void)fixed.readAs<float, Position::First>();
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 90
	(void)fixed.write<Position::First>(2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 91
	(void)testing::CatalogAccess::call<Position::First>(commands);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 92
	(void)testing::CatalogAccess::read<LegacyFirst>(fields);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 93
	(void)testing::CatalogAccess::readAs<float, LegacyFirst>(fields);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 94
	(void)testing::CatalogAccess::write<LegacyFirst>(fields, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 95
	(void)fixed.read<LegacyFirst>();
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 96
	(void)fixed.readAs<float, LegacyFirst>();
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 97
	(void)fixed.write<LegacyFirst>(2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 98
	(void)testing::CatalogAccess::call<LegacyFirst>(commands);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 99
	(void)testing::CatalogAccess::read<0x100000000ULL>(fields);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 100
	(void)fixed.read<-1>();
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 101
	(void)testing::CatalogAccess::call<0x100000000ULL>(commands);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 102
	(void)testing::CatalogAccess::readAs<float, -1>(fields);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 103
	(void)testing::CatalogAccess::write<0x100000000ULL>(fields, 2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE == 104
	(void)fixed.write<0x100000000ULL>(2.f);
#elif TELEMETRY_ID_BOUNDARY_FAIL_CASE != 0
#error "Unknown TELEMETRY_ID_BOUNDARY_FAIL_CASE"
#endif
	return rows.read<Position::First>() == 1.f &&
	               local.call<Position::First>() == CommandResult::Executed
	           ? 0
	           : 1;
}
