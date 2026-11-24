/* Exact types, bounds, conversions and borrowed iteration. MIT. */
#include "Fixture.hpp"
#include <cassert>
#include <bit>
#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>

using namespace fixture;
using telemetry::detail::makeId;
using std::string_view;

// Move-only visitor proves traversal borrows one visitor instead of copying it per row.
// API: MoveOnlyVisitor(), operator().
struct MoveOnlyVisitor {
	unsigned calls = 0;
	MoveOnlyVisitor() = default;
	MoveOnlyVisitor(const MoveOnlyVisitor&) = delete;

	template<class Definition>
	void operator()(const Definition&)
	{
		++calls;
	}
};

static_assert(
    std::is_same_v<
        decltype(localFields.get<Position::U16>()),
        const std::tuple_element_t<
            1, std::tuple<decltype(ts::field<&Device::readBool, &Device::setBool>("x", device)),
                          decltype(ts::field<&Device::readU16, &Device::setU16>("x", device))>>&>);
// The tree's group "repeat" (2) holds the local tables' definitions again.
template<class A, class B>
inline constexpr bool sameDefinition =
    std::is_same_v<std::remove_cvref_t<A>, std::remove_cvref_t<B>>;
static_assert(
    sameDefinition<decltype(fields.get<makeId<2, 7>()>()), decltype(localFields.get<7>())>);
static_assert(
    sameDefinition<decltype(commands.get<makeId<2, 0>()>()), decltype(localCommands.get<0>())>);
static_assert(
    sameDefinition<decltype(services.get<makeId<2, 0>()>()), decltype(localServices.get<0>())>);
static_assert(ts::idOf<model, "repeat/Config">() == makeId<2, 7>() &&
              ts::idOf<model, "repeat/Echo">() == makeId<2, 0>());
static_assert(noFields.empty() && noCommands.empty() && noServices.empty());
static_assert(noFields.begin() == noFields.end());
static_assert(noCommands.begin() == noCommands.end());
static_assert(noServices.begin() == noServices.end());
static_assert(noFieldCatalogs.empty() && noCommandCatalogs.empty() && noServiceCatalogs.empty());
static_assert([] {
	std::size_t entries = 0;
	localFields.forEach([&]<std::size_t I>(const auto& endpoint) {
		if (I != entries || endpoint.name() == nullptr)
			std::abort();
		++entries;
	});
	fields.forEach([&]<std::size_t G, std::size_t I>(string_view name, const auto& endpoint) {
		if (name != (G == 0 ? "first" : "repeat") || I >= 10 || endpoint.name() == nullptr)
			std::abort();
		++entries;
	});
	return entries == 30;
}());

template<class Table>
void checkLocal(const Table& table)
{
	std::size_t count = 0;
	table.forEach([&]<std::size_t I>(const auto& endpoint) {
		assert(I == count++);
		assert(endpoint.name() == table[I].name);
		unsigned calls = 0;
		auto visitor = [&](const auto& selected) {
			++calls;
			assert(static_cast<const void*>(&selected) == &endpoint);
		};
		assert(table.visit(I, visitor) && calls == 1);
	});
	assert(count == table.size());
	count = 0;
	table.forEach([&](const auto&) {
		++count;
		return false;
	}); // Results are ignored.
	assert(count == table.size());
	count = 0;
	for (const auto& entry : table)
		assert(&entry == table.data() + count++);
	assert(count == table.size());
}

template<class Table>
void checkGlobal(const Table& table)
{
	unsigned visits = 0;
	table.forEach([&]<std::size_t G, std::size_t I>(string_view name, const auto& endpoint) {
		assert(name == table[G].name && endpoint.name() == table[G].entries[I].name);
		auto selected = [&](const auto& definition) {
			++visits;
			assert(static_cast<const void*>(&definition) == &endpoint);
		};
		assert(table.visit(makeId<G, I>(), selected));
	});
	unsigned callbacks = 0;
	auto nothing = [&](const auto&) {
		++callbacks;
	};
	for (const auto id :
	     {std::uint64_t{UINT32_MAX}, std::uint64_t{1} << 32, (std::uint64_t{1} << 32) + 1})
		assert(!table.visit(id, nothing));
	assert(!table.visit(-1, nothing));
	assert(!table.visit(makeId<1, 0>(), nothing));
	assert(!table.visit(makeId<3, 0>(), nothing));
	assert(callbacks == 0);
	unsigned groups = 0;
	for (const auto& group : table)
		assert(&group == table.data() + groups++);
	assert(groups == 3 && visits == 2 * table[0].count);
}

void conversions()
{
	constexpr auto u16 = ts::idOf<model, "repeat/U16">();
	assert((localFields.readAs<float, 1>() == 65535.0f));
	assert(fields.readAs<float>(u16) == 65535.0f);
	assert(fields.writeAs(u16, 12.75) == W::Applied && device.integer == 12);
	assert((fields.writeAs<u16>(-0.5) == W::Applied && device.integer == 0));
	const auto writes = device.writes;
	for (const auto value : {-1.0, 65536.0, std::numeric_limits<double>::infinity(),
	                         std::numeric_limits<double>::quiet_NaN()})
		assert(fields.writeAs(u16, value) == W::InvalidValue);
	assert(device.writes == writes);
	assert(fields.writeAs(u16, UINT64_MAX) == W::InvalidValue);
	assert(fields.writeAs(ts::idOf<model, "first/U64">(), INT64_C(-1)) == W::InvalidValue);
	assert(!fields.readAs<std::uint64_t>(ts::idOf<model, "first/S64">()));
	assert(!fields.readAs<std::int64_t>(ts::idOf<model, "first/U64">()));
	assert(fields.readAs<std::uint64_t>(ts::idOf<model, "first/U64">()) == UINT64_MAX);
	assert(fields.readAs<std::int64_t>(ts::idOf<model, "first/S64">()) == INT64_MIN);
	assert(fields.readAs<double>(ts::idOf<model, "first/U64">()) ==
	       static_cast<double>(UINT64_MAX));
	assert(fields.readAs<std::int16_t>(ts::idOf<model, "first/Mode">()) == 2);
	assert(fields.writeAs(ts::idOf<model, "first/Mode">(), -2) == W::Applied);
	assert(fields.readAs<Mode>(ts::idOf<model, "first/Mode">()) == static_cast<Mode>(-2));
	assert(fields.readAs<float>(ts::idOf<model, "first/Mode">()) == -2.0f);
	assert(fields.writeAs(ts::idOf<model, "first/Mode">(), INT32_MAX) == W::InvalidValue);
	device.real = std::numeric_limits<double>::quiet_NaN();
	assert(!fields.readAs<bool>(ts::idOf<model, "first/Float">()) &&
	       !fields.readAs<std::uint32_t>(ts::idOf<model, "first/Float">()));
	assert(std::isnan(*fields.readAs<float>(ts::idOf<model, "first/Float">())));
	device.real = std::numeric_limits<double>::infinity();
	assert(std::isinf(*fields.readAs<float>(ts::idOf<model, "first/Float">())));
	device.real = std::numeric_limits<double>::max();
	assert(!fields.readAs<float>(ts::idOf<model, "first/Float">()));
	device.real = -0.0;
	assert(std::bit_cast<std::uint64_t>(*fields.readAs<double>(ts::idOf<model, "first/Float">())) ==
	       UINT64_C(1) << 63);
	assert(fields.writeAs(ts::idOf<model, "first/Bool">(), -2) == W::Applied && device.boolean);
	assert(fields.writeAs(ts::idOf<model, "first/Bool">(),
	                      std::numeric_limits<double>::quiet_NaN()) == W::InvalidValue);
	assert(!fields.readAs<float>(ts::idOf<model, "first/Config">()));
	const auto reads = device.reads;
	assert(!fields.readAs<Config>(ts::idOf<model, "first/U16">()));
	assert((!fields.readAs<std::array<std::uint16_t, 2>>(ts::idOf<model, "first/Config">())));
	assert(device.reads == reads); // Structural mismatch does not touch the source.
	assert(fields.writeAs(ts::idOf<model, "first/Config">(), std::array<std::uint16_t, 2>{}) ==
	       W::InvalidValue);
	assert(fields.writeAs(ts::idOf<model, "first/Array">(), 42) == W::ReadOnly);
	assert(fields.writeAs(ts::idOf<model, "first/Config">(), Config{99, false}) == W::Applied);
	assert(fields.readAs<Config>(ts::idOf<model, "first/Config">())->code == 99);
	assert(!fields.readAs<Config>(ts::idOf<model, "first/Slot">()));
	assert(fields.writeAs(ts::idOf<model, "first/Slot">(), Config{}) == W::Unavailable);
	slot.bind(device);
	assert(fields.readAs<Config>(ts::idOf<model, "first/Slot">())->code == 99);
	assert(fields.writeAs(ts::idOf<model, "first/Slot">(), Config{43, true}) == W::Applied);
	slot.reset();
	assert(!fields.readAs<float>(-1) && !fields.readAs<float>(UINT64_C(0x100000001)));
	assert(fields.writeAs(UINT64_C(0x100000001), 1) == W::NotFound);
	assert(fields.writeAs(makeId<1, 0>(), 1) == W::NotFound);
	assert(!localFields.readAs<float>(UINT64_C(0x100000001)));
	assert(localFields.writeAs(-1, 1) == W::NotFound);
	assert(localFields.readAs<double>(Position::Float) == -0.0);
	const auto big = fields.readAs<Big>(ts::idOf<model, "first/Big">());
	assert(big->words[2] == 3 &&
	       fields.writeAs(ts::idOf<model, "first/Big">(), *big) == W::Applied);
}

int main()
{
	// Traversal itself never calls a getter, command or service.
	checkLocal(localFields);
	checkLocal(localCommands);
	checkLocal(localServices);
	checkGlobal(fields);
	checkGlobal(commands);
	checkGlobal(services);
	assert(device.reads == 0 && device.writes == 0 && device.commands == 0 && device.services == 0);
	unsigned count = 0;
	auto never = [&](const auto&) {
		++count;
	};
	assert(!noFields.visit(0, never) && !noCommands.visit(0, never) && !noServices.visit(0, never));
	assert(!noFieldCatalogs.visit(0, never) && !noCommandCatalogs.visit(0, never) &&
	       !noServiceCatalogs.visit(0, never));
	noFields.forEach(never);
	noCommands.forEach(never);
	noServices.forEach(never);
	assert(count == 0 && !noFields.readAs<float>(0) && noFields.writeAs(0, 1) == W::NotFound);
	assert(!noFieldCatalogs.readAs<float>(0) && noFieldCatalogs.writeAs(0, 1) == W::NotFound);
	MoveOnlyVisitor visitor;
	localFields.forEach(visitor);
	assert(localCommands.visit(0, visitor) && visitor.calls == 11);
	assert(services.visit(makeId<2, 0>(), std::move(visitor)) && visitor.calls == 12);
#if defined(__cpp_exceptions)
	bool caught = false;
	try {
		(void)fields.visit(0, [](const auto&) {
			throw 42;
		});
	} catch (int value) {
		caught = value == 42;
	}
	assert(caught);
#endif
	conversions();
}
