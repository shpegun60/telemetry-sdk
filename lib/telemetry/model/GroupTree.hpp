/*
 * @file GroupTree.hpp
 * @brief One shared tree of groups mixing Fields, Commands and Services (descriptor v3.1).
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Builds a Model from branch expressions instead of per-family catalog tables.
 *
 * ts::group(name, items...) takes subgroups, Fields, Commands and Services in
 * any mix; Model{branchA, branchB, ...} concatenates branches. The tree is
 * flattened in pre-order at compile time; a group's flat position is the
 * group part of every PackedId in all three families, and its entries keep
 * declaration order inside each family. Each family keeps today's catalog
 * array, now of length G with an empty row where a group has none of that
 * family, so dispatch, ModelView and the structured ABI are unchanged. The
 * Model owns the per-group tables and is neither copyable nor movable.
 *
 * Tree names are the path segments of group/.../endpoint.member[i]: a group
 * or endpoint name in a tree contains no '/', '.', '[', ']' or '\', and names
 * are unique among everything one group holds (its subgroups and the
 * endpoints of all three families), like a folder. Both are checked when a
 * group is built; root branches are checked when the Model is built.
 */

#ifndef TELEMETRY_MODEL_GROUP_TREE_HPP
#define TELEMETRY_MODEL_GROUP_TREE_HPP
#pragma once

#include "Model.hpp"
#include "TreePath.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace telemetry {

namespace detail {

enum class TreeItemKind : std::uint8_t {
	None,
	Group,
	Field,
	Command,
	Service
};

template<class T>
inline constexpr TreeItemKind treeItemKind = TreeItemKind::None;

template<class... Items>
inline constexpr TreeItemKind treeItemKind<GroupBranch<Items...>> = TreeItemKind::Group;

template<class Getter, class Setter>
inline constexpr TreeItemKind treeItemKind<FieldDefinition<Getter, Setter>> = TreeItemKind::Field;

template<class Binding>
inline constexpr TreeItemKind treeItemKind<CommandDefinition<Binding>> = TreeItemKind::Command;

template<class Binding>
inline constexpr TreeItemKind treeItemKind<ServiceDefinition<Binding>> = TreeItemKind::Service;

template<class T>
inline constexpr bool groupTreeItem = treeItemKind<T> != TreeItemKind::None;

// A group or endpoint name in a tree is a path segment.
[[noreturn]] inline void
invalidGroupTreeName() noexcept // a tree name contains '/', '.', '[', ']' or a backslash
{
	std::abort();
}

// Two items of one group share a name.
[[noreturn]] inline void
duplicateGroupTreeName() noexcept // names are unique among a group's subgroups and endpoints
{
	std::abort();
}

constexpr std::string_view treeNameView(const char* name) noexcept
{
	std::size_t size = 0;
	while (name[size] != '\0')
		++size;
	return {name, size};
}

constexpr bool treeNameValid(std::string_view name) noexcept
{
	for (const char c : name)
		if (c == '/' || c == '.' || c == '[' || c == ']' || c == '\\')
			return false;
	return true;
}

template<class Item>
constexpr const char* treeItemName(const Item& item) noexcept
{
	if constexpr (groupBranch<Item>)
		return item.name;
	else
		return item.name();
}

// Every direct item name is a valid segment, and no two are equal. The scan
// is quadratic in one group's item count, which is a shard of about 32.
template<class... Items>
constexpr void checkGroupTreeItems(const char* name, const std::tuple<Items...>& items) noexcept
{
	if (!treeNameValid(treeNameView(name)))
		invalidGroupTreeName();
	std::array<std::string_view, sizeof...(Items)> names{};
	std::apply(
	    [&](const auto&... item) constexpr {
		    std::size_t i = 0;
		    ((names[i++] = treeNameView(treeItemName(item))), ...);
	    },
	    items);
	for (std::size_t i = 0; i < names.size(); ++i) {
		if (!treeNameValid(names[i]))
			invalidGroupTreeName();
		for (std::size_t j = 0; j < i; ++j)
			if (names[i] == names[j])
				duplicateGroupTreeName();
	}
}

// Where one group sits: its root branch, its depth and the item positions
// from the root down to it.
template<std::size_t Root, std::size_t Depth, std::size_t... Path>
struct GroupLocator {};

template<class Branch, std::size_t... Path>
struct BranchAt;

template<class Branch>
struct BranchAt<Branch> {
	using type = Branch;
};

template<class Branch, std::size_t First, std::size_t... Rest>
struct BranchAt<Branch, First, Rest...>
    : BranchAt<std::tuple_element_t<First, decltype(Branch::items)>, Rest...> {};

template<std::size_t... Path>
struct Descend;

template<>
struct Descend<> {
	template<class Branch>
	static constexpr const Branch& from(const Branch& branch) noexcept
	{
		return branch;
	}
};

template<std::size_t First, std::size_t... Rest>
struct Descend<First, Rest...> {
	template<class Branch>
	static constexpr const auto& from(const Branch& branch) noexcept
	{
		return Descend<Rest...>::from(std::get<First>(branch.items));
	}
};

// Pre-order: the group, then each subgroup in declaration order.
template<class Branch, class Locator>
struct FlattenBranch;

template<class Item, std::size_t Index, class Locator>
struct FlattenChild {
	using type = TypeList<>;
};

template<class... Items, std::size_t Index, std::size_t Root, std::size_t Depth,
         std::size_t... Path>
struct FlattenChild<GroupBranch<Items...>, Index, GroupLocator<Root, Depth, Path...>>
    : FlattenBranch<GroupBranch<Items...>, GroupLocator<Root, Depth + 1, Path..., Index>> {};

template<class... Items, std::size_t Root, std::size_t Depth, std::size_t... Path>
struct FlattenBranch<GroupBranch<Items...>, GroupLocator<Root, Depth, Path...>> {
	template<std::size_t... I>
	static auto children(std::index_sequence<I...>) -> typename ConcatLists<
	    TypeList<GroupLocator<Root, Depth, Path...>>,
	    typename FlattenChild<Items, I, GroupLocator<Root, Depth, Path...>>::type...>::type;
	using type = decltype(children(std::index_sequence_for<Items...>{}));
};

template<class Indices, class... Branches>
struct FlattenRoots;

template<std::size_t... I, class... Branches>
struct FlattenRoots<std::index_sequence<I...>, Branches...>
    : ConcatLists<typename FlattenBranch<Branches, GroupLocator<I, 0>>::type...> {};

// Positions of one family among a group's items, in declaration order.
template<std::size_t N>
struct ItemPositions {
	std::array<std::size_t, N> values{};
	std::size_t count = 0;
};

template<TreeItemKind Kind, class... Items>
consteval auto itemPositions() noexcept
{
	ItemPositions<sizeof...(Items)> result{};
	constexpr std::array<TreeItemKind, sizeof...(Items)> kinds{treeItemKind<Items>...};
	for (std::size_t i = 0; i < kinds.size(); ++i)
		if (kinds[i] == Kind)
			result.values[result.count++] = i;
	return result;
}

template<TreeItemKind Kind, class... Items>
struct KindIndices {
	static constexpr auto found = itemPositions<Kind, Items...>();

	template<std::size_t... I>
	static auto make(std::index_sequence<I...>) -> std::index_sequence<found.values[I]...>;

	using type = decltype(make(std::make_index_sequence<found.count>{}));
};

// One group's own tables, built from copies of its definitions.
template<class Branch, class FieldPositions, class CommandPositions, class ServicePositions>
struct GroupTables;

template<class... Items, std::size_t... F, std::size_t... C, std::size_t... S>
struct GroupTables<GroupBranch<Items...>, std::index_sequence<F...>, std::index_sequence<C...>,
                   std::index_sequence<S...>> {
	using ItemTuple = std::tuple<Items...>;
	using Fields = FieldTable<std::tuple_element_t<F, ItemTuple>...>;
	using Commands = CommandTable<std::tuple_element_t<C, ItemTuple>...>;
	using Services = ServiceTable<std::tuple_element_t<S, ItemTuple>...>;

	constexpr explicit GroupTables(const GroupBranch<Items...>& branch) noexcept
	    : name(branch.name), fields(std::get<F>(branch.items)...),
	      commands(std::get<C>(branch.items)...), services(std::get<S>(branch.items)...)
	{}

	const char* name;
	Fields fields;
	Commands commands;
	Services services;
};

template<class Branch>
struct GroupTablesFor;

template<class... Items>
struct GroupTablesFor<GroupBranch<Items...>> {
	using type = GroupTables<GroupBranch<Items...>,
	                         typename KindIndices<TreeItemKind::Field, Items...>::type,
	                         typename KindIndices<TreeItemKind::Command, Items...>::type,
	                         typename KindIndices<TreeItemKind::Service, Items...>::type>;
};

template<std::size_t Index, class... Types>
using PackElement = std::tuple_element_t<Index, std::tuple<Types...>>;

// One flattened group: its pre-order position, depth and owned tables.
template<std::size_t Index, class Locator, class... Branches>
struct GroupNode;

template<std::size_t Index, std::size_t Root, std::size_t Depth, std::size_t... Path,
         class... Branches>
struct GroupNode<Index, GroupLocator<Root, Depth, Path...>, Branches...> {
	using Branch = typename BranchAt<PackElement<Root, Branches...>, Path...>::type;
	using Tables = typename GroupTablesFor<Branch>::type;
	static constexpr std::size_t depth = Depth;

	constexpr explicit GroupNode(const std::tuple<const Branches&...>& roots) noexcept
	    : tables(Descend<Path...>::from(std::get<Root>(roots)))
	{}

	Tables tables;
};

template<class Locators, class Indices, class... Branches>
struct MakeGroupNodes;

template<class... Locators, std::size_t... I, class... Branches>
struct MakeGroupNodes<TypeList<Locators...>, std::index_sequence<I...>, Branches...> {
	using type = TypeList<GroupNode<I, Locators, Branches...>...>;
};

template<class List>
struct NodeDepths;

template<class... Nodes>
struct NodeDepths<TypeList<Nodes...>> {
	static constexpr std::array<std::uint16_t, sizeof...(Nodes)> values{
	    static_cast<std::uint16_t>(Nodes::depth)...};
};

// The pre-order layout of a set of root branches.
template<class... Branches>
struct GroupTreeLayout {
	using Locators = typename FlattenRoots<std::index_sequence_for<Branches...>, Branches...>::type;
	static constexpr std::size_t groupCount = Locators::size;
	using Nodes =
	    typename MakeGroupNodes<Locators, std::make_index_sequence<groupCount>, Branches...>::type;
	static constexpr auto depths = NodeDepths<Nodes>::values;
	static constexpr std::uint32_t noParent = 0xffffffffu;

	// In pre-order a parent is the nearest earlier group one level up.
	static constexpr std::array<std::uint32_t, groupCount> parents = [] {
		std::array<std::uint32_t, groupCount> result{};
		for (std::size_t i = 0; i < groupCount; ++i) {
			result[i] = noParent;
			for (std::size_t j = i; depths[i] != 0 && j-- > 0;)
				if (depths[j] + 1 == depths[i]) {
					result[i] = static_cast<std::uint32_t>(j);
					break;
				}
		}
		return result;
	}();
};

template<class List, class... Branches>
struct GroupTreeNodes;

template<class... Nodes, class... Branches>
struct GroupTreeNodes<TypeList<Nodes...>, Branches...> : Nodes... {
	constexpr explicit GroupTreeNodes(const std::tuple<const Branches&...>& roots) noexcept
	    : Nodes(roots)...
	{}
};

template<std::size_t I, class List>
struct NodeAt;

template<std::size_t I, class... Nodes>
struct NodeAt<I, TypeList<Nodes...>> {
	using type = PackElement<I, Nodes...>;
};

template<class List>
struct TreeCatalogTypes;

template<class... Nodes>
struct TreeCatalogTypes<TypeList<Nodes...>> {
	using Fields = FieldCatalogTable<TableGroup<typename Nodes::Tables::Fields>...>;
	using Commands = CommandCatalogTable<TableGroup<typename Nodes::Tables::Commands>...>;
	using Services = ServiceCatalogTable<TableGroup<typename Nodes::Tables::Services>...>;
};

// The Model's family tags in tree mode; they carry the branch types only.
template<class... Branches>
struct GroupTreeFields {};

template<class... Branches>
struct GroupTreeCommands {};

template<class... Branches>
struct GroupTreeServices {};

struct GroupTreeAccess;

} // namespace detail

// A group of the shared tree. Items are subgroups, Fields, Commands and
// Services in any mix; a group may be empty. Subgroups come first in the
// tree, then Fields, Commands and Services, each in declaration order. A
// group can mount another branch: group("power", adc::branch, motor::branch).
template<class... Explicit, class... Items>
    requires(sizeof...(Explicit) == 0 && (detail::groupTreeItem<std::remove_cvref_t<Items>> && ...))
[[nodiscard]] constexpr auto group(detail::Name name, Items&&... items) noexcept
{
	GroupBranch<std::remove_cvref_t<Items>...> result{name, {std::forward<Items>(items)...}};
	detail::checkGroupTreeItems(result.name, result.items);
	return result;
}

// A Model built from root branches (descriptor v3.1 GroupTree mode). It owns
// one Field, Command and Service table per group and the three catalog tables
// over them, and exposes one registry, the runtime indexes and a ModelView. A
// group's flat pre-order position is the group part of its IDs; paths and
// handles (TreePath.hpp) name its endpoints.
// Public methods:
// - Model(): Flatten and own the branches.
// - types(): Borrow type registry.
// - typeId(): Resolve exact type.
// - serviceIndex(), fieldIndex(), commandIndex(): Borrow indexes.
// - view(): Borrow runtime view.
// - fields(), commands(), services(): Runtime-ID access and traversal.
// - groupName(): Borrow one group's name.
// - idOf(): Resolve a path at compile time (consteval, constexpr model).
// - max*(): Bound scratch and payloads.
template<class... Branches>
class Model<detail::GroupTreeFields<Branches...>, detail::GroupTreeCommands<Branches...>,
            detail::GroupTreeServices<Branches...>>
    : private detail::GroupTreeNodes<typename detail::GroupTreeLayout<Branches...>::Nodes,
                                     Branches...> {
	using Layout = detail::GroupTreeLayout<Branches...>;
	using NodeList = typename Layout::Nodes;
	using Nodes = detail::GroupTreeNodes<NodeList, Branches...>;
	using Catalogs = detail::TreeCatalogTypes<NodeList>;
	friend struct detail::GroupTreeAccess;

public:
	using FieldCatalogs = typename Catalogs::Fields;
	using CommandCatalogs = typename Catalogs::Commands;
	using ServiceCatalogs = typename Catalogs::Services;
	using Registry =
	    typename detail::CatalogModel<FieldCatalogs, CommandCatalogs, ServiceCatalogs>::Registry;

	static constexpr bool groupTree = true;
	static constexpr std::uint32_t groupCount = static_cast<std::uint32_t>(Layout::groupCount);
	static constexpr std::uint32_t noParent = Layout::noParent;
	// Pre-order outline: depth of every group, and its parent (noParent at a root).
	static constexpr auto groupDepths = Layout::depths;
	static constexpr auto groupParents = Layout::parents;

	static_assert(sizeof...(Branches) > 0, "A group-tree Model needs at least one branch");

	constexpr Model(const Branches&... branches) noexcept
	    : Model(std::make_index_sequence<groupCount>{}, std::tuple<const Branches&...>{branches...})
	{}

	Model(const Model&) = delete;
	Model& operator=(const Model&) = delete;
	Model(Model&&) = delete;
	Model& operator=(Model&&) = delete;

	[[nodiscard]] static constexpr TypeRegistryView types() noexcept
	{
		return Registry::view();
	}

	template<class T>
	[[nodiscard]] static consteval TypeId typeId() noexcept
	{
		return Registry::template typeId<T>();
	}

	[[nodiscard]] constexpr ServiceIndex serviceIndex() const noexcept
	{
		return flat_.serviceIndex();
	}

	[[nodiscard]] constexpr FieldIndex fieldIndex() const noexcept
	{
		return flat_.fieldIndex();
	}

	[[nodiscard]] constexpr CommandIndex commandIndex() const noexcept
	{
		return flat_.commandIndex();
	}

	[[nodiscard]] constexpr ModelView view() const noexcept
	{
		return flat_.view();
	}

	[[nodiscard]] constexpr const FieldCatalogs& fields() const noexcept
	{
		return fields_;
	}

	[[nodiscard]] constexpr const CommandCatalogs& commands() const noexcept
	{
		return commands_;
	}

	[[nodiscard]] constexpr const ServiceCatalogs& services() const noexcept
	{
		return services_;
	}

	// The name of the group at flat pre-order position group (< groupCount).
	[[nodiscard]] constexpr const char* groupName(std::uint32_t group) const noexcept
	{
		return fields_[group].name;
	}

	// The PackedId of the endpoint at group/.../endpoint, resolved at compile
	// time on a constexpr model: model.idOf<"adc/channel1/Gain">(). A path that
	// names no endpoint fails the build (noEndpointAtPath). Typed access by path
	// is telemetry::read/write/call<model, path>() or a handle, in TreePath.hpp.
	template<EndpointPath P>
	[[nodiscard]] consteval PackedId idOf() const noexcept
	{
		const auto at = detail::locatePath(*this, P.view());
		if (at.error != detail::PathError::None || at.memberStart != P.view().size())
			detail::noEndpointAtPath();
		return detail::makeId(static_cast<GroupId>(at.group), static_cast<EntryOffset>(at.entry));
	}

	[[nodiscard]] constexpr std::uint32_t maxFieldScratch() const noexcept
	{
		return flat_.maxFieldScratch();
	}

	[[nodiscard]] constexpr std::uint32_t maxCommandScratch() const noexcept
	{
		return flat_.maxCommandScratch();
	}

	[[nodiscard]] constexpr std::uint32_t maxScratch() const noexcept
	{
		return flat_.maxScratch();
	}

	[[nodiscard]] constexpr std::uint32_t maxFieldWireSize() const noexcept
	{
		return flat_.maxFieldWireSize();
	}

	[[nodiscard]] constexpr std::uint32_t maxCommandRequestWireSize() const noexcept
	{
		return flat_.maxCommandRequestWireSize();
	}

	[[nodiscard]] constexpr std::uint32_t maxServiceRequestWireSize() const noexcept
	{
		return flat_.maxServiceRequestWireSize();
	}

	[[nodiscard]] constexpr std::uint32_t maxServiceScratch() const noexcept
	{
		return flat_.maxServiceScratch();
	}

	[[nodiscard]] constexpr std::uint32_t maxServiceResponseWireSize() const noexcept
	{
		return flat_.maxServiceResponseWireSize();
	}

private:
	template<std::size_t I>
	using NodeAt = typename detail::NodeAt<I, NodeList>::type;

	template<std::size_t I>
	constexpr const auto& tables() const noexcept
	{
		return static_cast<const NodeAt<I>&>(*this).tables;
	}

	template<std::size_t... I>
	constexpr Model(std::index_sequence<I...>, const std::tuple<const Branches&...>& roots) noexcept
	    : Nodes(roots), fields_(detail::TableGroup<typename NodeAt<I>::Tables::Fields>{
	                        tables<I>().name, std::addressof(tables<I>().fields)}...),
	      commands_(detail::TableGroup<typename NodeAt<I>::Tables::Commands>{
	          tables<I>().name, std::addressof(tables<I>().commands)}...),
	      services_(detail::TableGroup<typename NodeAt<I>::Tables::Services>{
	          tables<I>().name, std::addressof(tables<I>().services)}...),
	      flat_(fields_, commands_, services_)
	{
		// Root branches are siblings: their names are unique too.
		checkRoots(roots, std::index_sequence_for<Branches...>{});
	}

	template<std::size_t... R>
	static constexpr void checkRoots(const std::tuple<const Branches&...>& roots,
	                                 std::index_sequence<R...>) noexcept
	{
		const std::array<std::string_view, sizeof...(R)> names{
		    detail::treeNameView(std::get<R>(roots).name)...};
		for (std::size_t i = 0; i < names.size(); ++i)
			for (std::size_t j = 0; j < i; ++j)
				if (names[i] == names[j])
					detail::duplicateGroupTreeName();
	}

	using Flat = detail::CatalogModel<FieldCatalogs, CommandCatalogs, ServiceCatalogs>;

	FieldCatalogs fields_;
	CommandCatalogs commands_;
	ServiceCatalogs services_;
	Flat flat_;
};

// Model{branchA, branchB, ...}: the one form of a Model.
template<class... Branches>
    requires(sizeof...(Branches) > 0 && (detail::groupBranch<Branches> && ...))
Model(const Branches&...)
    -> Model<detail::GroupTreeFields<Branches...>, detail::GroupTreeCommands<Branches...>,
             detail::GroupTreeServices<Branches...>>;

} // namespace telemetry

#endif
