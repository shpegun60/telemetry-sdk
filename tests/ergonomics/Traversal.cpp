/* Ordered typed and erased traversal with exact continuation results. MIT. */
#include "../structured/traversal/Fixture.hpp"
#include <cassert>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace fixture;

// Every overload borrows the same visitor; only its original counter changes.
struct MoveOnlyVisitor {
	std::size_t calls = 0;
	MoveOnlyVisitor() = default;
	MoveOnlyVisitor(const MoveOnlyVisitor&) = delete;

	template<class... Args>
	bool operator()(Args&&...) &
	{
		++calls;
		return true;
	}
};

template<class Table>
constexpr bool checkLocal(const Table& table)
{
	std::size_t calls = 0;
	const bool complete = table.forEachWhile([&]<std::size_t I>(const auto& definition) {
		if (I != calls++ || definition.name() != table[I].name)
			std::abort();
		return true;
	});
	if (!complete || calls != table.size())
		return false;
	for (std::size_t stop = 0; stop < table.size(); ++stop) {
		calls = 0;
		if (table.forEachWhile([&](const auto&) {
			    return calls++ < stop;
		    }) ||
		    calls != stop + 1)
			return false;
	}
	calls = 0;
	// Existing complete traversal continues discarding callback results.
	table.forEach([&](const auto&) {
		++calls;
		return false;
	});
	return calls == table.size();
}

template<class Table>
constexpr bool checkCatalog(const Table& table)
{
	std::size_t expected = 0;
	for (const auto& catalog : table)
		expected += catalog.count;
	std::size_t calls = 0;
	if (!table.forEachWhile(
	        [&]<std::size_t G, std::size_t I>(std::string_view name, const auto& definition) {
		        if (name != table[G].name || definition.name() != table[G].entries[I].name)
			        std::abort();
		        ++calls;
		        return true;
	        }) ||
	    calls != expected)
		return false;
	for (std::size_t stop = 0; stop < expected; ++stop) {
		calls = 0;
		if (table.forEachWhile([&](std::string_view, const auto&) {
			    return calls++ < stop;
		    }) ||
		    calls != stop + 1)
			return false;
	}
	calls = 0;
	table.forEach([&](std::string_view, const auto&) {
		++calls;
		return false;
	});
	return calls == expected;
}

template<class Table, class Index>
constexpr bool checkErased(const Table& table, const Index& index)
{
	std::size_t calls = 0;
	std::uint32_t group = 0, position = 0;
	std::size_t expected = 0;
	for (const auto& catalog : table)
		expected += catalog.count;
	auto visitor = [&](ts::PackedId id, std::string_view name, const auto& entry) {
		while (group < table.size() && position == table[group].count) {
			++group;
			position = 0;
		}
		if (id != ts::detail::makeId(group, position) || name != table[group].name ||
		    &entry != &table[group].entries[position] || index.find(id) != &entry)
			std::abort();
		++calls;
		++position;
		return false; // Complete erased traversal also ignores results.
	};
	index.forEachEntry(visitor);
	if (calls != expected)
		return false;
	group = position = 0;
	calls = 0;
	table.forEachEntry(visitor);
	if (calls != expected)
		return false;
	for (std::size_t stop = 0; stop < expected; ++stop) {
		calls = 0;
		if (index.forEachEntryWhile([&](ts::PackedId, std::string_view, const auto&) {
			    return calls++ < stop;
		    }) ||
		    calls != stop + 1)
			return false;
		calls = 0;
		if (table.forEachEntryWhile([&](ts::PackedId, std::string_view, const auto&) {
			    return calls++ < stop;
		    }) ||
		    calls != stop + 1)
			return false;
	}
	calls = 0;
	return index.forEachEntryWhile([&](ts::PackedId, std::string_view, const auto&) {
		++calls;
		return true;
	}) && calls == expected;
}

static_assert(checkLocal(localFields) && checkLocal(localCommands) && checkLocal(localServices));
static_assert(checkLocal(noFields) && checkLocal(noCommands) && checkLocal(noServices));
static_assert(checkCatalog(fields) && checkCatalog(commands) && checkCatalog(services));
static_assert(checkCatalog(noFieldCatalogs) && checkCatalog(noCommandCatalogs) &&
              checkCatalog(noServiceCatalogs));
static_assert(checkErased(fields, model.view().fields) &&
              checkErased(commands, model.view().commands) &&
              checkErased(services, model.view().services));
static_assert(checkErased(noFieldCatalogs, noFieldCatalogs.index()) &&
              checkErased(noCommandCatalogs, noCommandCatalogs.index()) &&
              checkErased(noServiceCatalogs, noServiceCatalogs.index()));
static_assert(ts::FieldIndex{nullptr, 0}.forEachEntryWhile([](ts::PackedId, std::string_view,
                                                              const ts::FieldEntry&) {
	return true;
}));
static_assert(ts::CommandIndex{nullptr, 0}.forEachEntryWhile([](ts::PackedId, std::string_view,
                                                                const ts::CommandEntry&) {
	return true;
}));
static_assert(ts::ServiceIndex{nullptr, 0}.forEachEntryWhile([](ts::PackedId, std::string_view,
                                                                const ts::ServiceEntry&) {
	return true;
}));

int main()
{
	assert(checkLocal(localFields) && checkLocal(localCommands) && checkLocal(localServices));
	assert(checkCatalog(fields) && checkCatalog(commands) && checkCatalog(services));
	assert(checkErased(fields, model.view().fields));
	assert(checkErased(commands, model.view().commands));
	assert(checkErased(services, model.view().services));
	MoveOnlyVisitor visitor;
	assert(localFields.forEachWhile(visitor));
	assert(commands.forEachWhile(visitor));
	services.forEachEntry(visitor);
	assert(model.view().fields.forEachEntryWhile(visitor));
	assert(visitor.calls == 10 + 6 + 8 + 20);
	assert(localCommands.forEachWhile(MoveOnlyVisitor{}));
	assert(services.forEachEntryWhile(MoveOnlyVisitor{}));
	assert(device.reads == 0 && device.writes == 0 && device.commands == 0 && device.services == 0);
#if defined(__cpp_exceptions)
	unsigned calls = 0;
	try {
		(void)fields.forEachWhile([&](std::string_view, const auto&) -> bool {
			++calls;
			throw 7;
		});
		assert(false);
	} catch (int value) {
		assert(value == 7 && calls == 1);
	}
	calls = 0;
	try {
		model.view().services.forEachEntry([&](ts::PackedId, std::string_view, const auto&) {
			++calls;
			throw 9;
		});
		assert(false);
	} catch (int value) {
		assert(value == 9 && calls == 1);
	}
#endif
}
