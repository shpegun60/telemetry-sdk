/*
 * @file TreePath.hpp
 * @brief Compile-time access to a group-tree Model by path: group/.../endpoint.member[i].
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Resolves a path written in device code to a PackedId at compile time.
 *
 * The grammar is the clients' one: groups and the endpoint joined by '/',
 * then '.member' and '[i]' as needed. read<model, path>() takes a Field or a
 * member or element of one: it calls the whole Field's getter and returns the
 * selected part, through PFR's member names, in std::optional (empty when the
 * getter is unavailable). write<> takes a whole Field and call<> a Command or
 * Service; idOf<> returns the PackedId of any endpoint. Nothing is added to an
 * image: no string, no table, only the selected typed call.
 *
 * C++20 cannot read an object's names inside its own member function at
 * compile time, so the model is a template argument, a reference to a
 * constexpr Model with static storage:
 *   inline constexpr telemetry::Model model{adc::branch, metrology::branch};
 *   const auto gain = telemetry::read<model, "adc/channel1/Gain">();
 * model.idOf<"adc/channel1/Gain">() is the one member form, a consteval call
 * on a constexpr model. A path that names nothing, a group, a member of a
 * non-struct or an element past an array is a build error whose trace shows
 * the path as an EndpointPath<N>{"..."} template argument.
 *
 * Included by GroupTree.hpp; it needs only IDs and aggregate reflection.
 */

#ifndef TELEMETRY_MODEL_TREE_PATH_HPP
#define TELEMETRY_MODEL_TREE_PATH_HPP
#pragma once

#include "../core/Id.hpp"
#include "../reflection/Aggregate.hpp"
#include "../result/EndpointStatus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace telemetry {

// A path literal as a template argument: read<model, "adc/Config.gain">().
template<std::size_t N>
struct EndpointPath {
	char text[N];

	consteval EndpointPath(const char (&source)[N]) noexcept
	{
		for (std::size_t i = 0; i < N; ++i)
			text[i] = source[i];
	}

	[[nodiscard]] constexpr std::string_view view() const noexcept
	{
		return {text, N - 1};
	}
};

namespace detail {

// A Model of the group-tree form (GroupTree.hpp).
template<class M>
inline constexpr bool groupTreeModel = requires { requires M::groupTree; };

enum class PathError : std::uint8_t {
	None,
	Malformed,
	NoGroup,
	NamesGroup,
	NoEndpoint
};

// Where a path leads: the endpoint's family (1 Field, 2 Command, 3 Service),
// its group and entry, and the offset of the member suffix in the path.
struct PathLocation {
	PathError error = PathError::None;
	std::uint8_t category = 0;
	std::uint32_t group = 0, entry = 0;
	std::size_t memberStart = 0;
};

constexpr std::string_view pathNameView(const char* name) noexcept
{
	std::size_t size = 0;
	while (name[size] != '\0')
		++size;
	return {name, size};
}

template<class Catalog>
consteval bool findEntry(const Catalog& catalog, std::string_view name,
                         std::uint32_t& entry) noexcept
{
	for (std::uint32_t i = 0; i < catalog.count; ++i)
		if (pathNameView(catalog.entries[i].name) == name) {
			entry = i;
			return true;
		}
	return false;
}

// Tree names hold no '/', '.', '[', ']' or '\', so a tree path has no escapes.
template<class Tree>
consteval PathLocation locatePath(const Tree& tree, std::string_view path) noexcept
{
	PathLocation result{};
	std::size_t pos = 0;
	std::uint32_t parent = Tree::noParent;
	for (unsigned segments = 1;; ++segments) {
		const auto begin = pos;
		while (pos < path.size() && path[pos] != '/' && path[pos] != '.' && path[pos] != '[') {
			if (path[pos] == ']' || path[pos] == '\\')
				return {PathError::Malformed};
			++pos;
		}
		const auto segment = path.substr(begin, pos - begin);
		if (segment.empty())
			return {PathError::Malformed};
		std::uint32_t child = Tree::groupCount;
		for (std::uint32_t g = 0; g < Tree::groupCount; ++g)
			if (Tree::groupParents[g] == parent && pathNameView(tree.groupName(g)) == segment)
				child = g;
		if (pos < path.size() && path[pos] == '/') {
			if (child == Tree::groupCount)
				return {PathError::NoGroup};
			parent = child;
			++pos;
			continue;
		}
		if (segments == 1)
			return {child == Tree::groupCount ? PathError::NoEndpoint : PathError::NamesGroup};
		result.group = parent;
		result.memberStart = pos;
		if (findEntry(tree.fields()[parent], segment, result.entry))
			result.category = 1;
		else if (findEntry(tree.commands()[parent], segment, result.entry))
			result.category = 2;
		else if (findEntry(tree.services()[parent], segment, result.entry))
			result.category = 3;
		else
			result.error =
			    child == Tree::groupCount ? PathError::NoEndpoint : PathError::NamesGroup;
		return result;
	}
}

// The trace of a failed path shows P, the path, as a template argument.
template<EndpointPath P, PathError Error>
struct PathFound {
	static_assert(Error != PathError::Malformed,
	              "Malformed path: group/.../endpoint, then .member and [i]");
	static_assert(Error != PathError::NoGroup, "No group has this path");
	static_assert(Error != PathError::NamesGroup, "This path names a group, not an endpoint");
	static_assert(Error != PathError::NoEndpoint, "No endpoint has this path");
	static constexpr bool value = Error == PathError::None;
};

template<EndpointPath P, std::uint8_t Category, std::uint8_t... Allowed>
struct PathCategory {
	static_assert(((Category == Allowed) || ...),
	              "This path names another kind of endpoint: read<> and write<> take a Field, "
	              "call<> a Command or Service");
	static constexpr bool value = ((Category == Allowed) || ...);
};

template<EndpointPath P, bool Whole>
struct PathWhole {
	static_assert(Whole, "write<>, call<> and idOf<> take a whole endpoint: a member write would "
	                     "be a hidden read-modify-write, and the wire writes whole Fields");
	static constexpr bool value = Whole;
};

template<class T>
inline constexpr bool pathStdArray = false;

template<class E, std::size_t N>
inline constexpr bool pathStdArray<std::array<E, N>> = true;

template<class T>
inline constexpr bool pathDependentFalse = false;

template<class T>
consteval std::size_t pathMemberCount() noexcept
{
	if constexpr (std::is_class_v<T> && !pathStdArray<T>)
		return reflection::memberCount<T>;
	else
		return 0;
}

template<class T, std::size_t... I>
consteval std::size_t memberPosition(std::string_view name, std::index_sequence<I...>) noexcept
{
	std::size_t result = sizeof...(I);
	((reflection::memberName<I, T>() == name ? void(result = I) : void()), ...);
	return result;
}

// The end of a member name: the next '.' or '[', or the end of the path.
template<EndpointPath P, std::size_t Pos>
consteval std::size_t memberEnd() noexcept
{
	auto end = Pos;
	while (end < P.view().size() && P.view()[end] != '.' && P.view()[end] != '[')
		++end;
	return end;
}

template<class T, EndpointPath P, std::size_t Pos>
consteval std::size_t pathMemberIndex() noexcept
{
	if constexpr (pathMemberCount<T>() != 0)
		return memberPosition<T>(P.view().substr(Pos + 1, memberEnd<P, Pos + 1>() - Pos - 1),
		                         std::make_index_sequence<pathMemberCount<T>()>{});
	else
		return 0;
}

// The digits after '[': their end, and their decimal value.
template<EndpointPath P, std::size_t Pos>
consteval std::size_t digitsEnd() noexcept
{
	auto at = Pos;
	while (at < P.view().size() && P.view()[at] >= '0' && P.view()[at] <= '9')
		++at;
	return at;
}

template<EndpointPath P, std::size_t Pos>
consteval std::size_t digitsValue() noexcept
{
	std::size_t value = 0;
	for (auto at = Pos; at < digitsEnd<P, Pos>(); ++at)
		value = value * 10 + static_cast<std::size_t>(P.view()[at] - '0');
	return value;
}

enum class PathStep : std::uint8_t {
	End,
	Member,
	Element,
	Bad
};

template<EndpointPath P, std::size_t Pos>
consteval PathStep pathStep() noexcept
{
	if (Pos == P.view().size())
		return PathStep::End;
	if (P.view()[Pos] == '.')
		return PathStep::Member;
	if (P.view()[Pos] == '[')
		return PathStep::Element;
	return PathStep::Bad;
}

// The part of a T selected by the suffix of P from Pos on: Type and select().
template<class T, EndpointPath P, std::size_t Pos, PathStep Step = pathStep<P, Pos>()>
struct PathMember;

template<class T, EndpointPath P, std::size_t Pos>
struct PathMember<T, P, Pos, PathStep::End> {
	using Type = T;

	static constexpr const T& select(const T& value) noexcept
	{
		return value;
	}
};

template<bool Valid, class T, EndpointPath P, std::size_t Pos, std::size_t Index>
struct MemberNext {
	using type = PathMember<T, P, P.view().size()>;
};

template<class T, EndpointPath P, std::size_t Pos, std::size_t Index>
struct MemberNext<true, T, P, Pos, Index> {
	using type = PathMember<std::remove_cvref_t<reflection::MemberType<Index, T>>, P,
	                        memberEnd<P, Pos + 1>()>;
};

template<class T, EndpointPath P, std::size_t Pos>
struct PathMember<T, P, Pos, PathStep::Member> {
	static_assert(pathMemberCount<T>() != 0,
	              "This path takes a member of a value that is not a struct");
	static constexpr std::size_t index = pathMemberIndex<T, P, Pos>();
	static constexpr bool valid = index < pathMemberCount<T>();
	static_assert(pathMemberCount<T>() == 0 || valid, "No member of this struct has this name");
	using Next = typename MemberNext<valid, T, P, Pos, index>::type;
	using Type = typename Next::Type;

	static constexpr const Type& select(const T& value) noexcept
	{
		if constexpr (valid)
			return Next::select(reflection::get<index>(value));
		else
			return Next::select(value);
	}
};

template<bool Valid, class T, EndpointPath P, std::size_t Close>
struct ElementNext {
	using type = PathMember<T, P, P.view().size()>;
};

template<class T, EndpointPath P, std::size_t Close>
struct ElementNext<true, T, P, Close> {
	using type = PathMember<typename T::value_type, P, Close + 1>;
};

template<class T, EndpointPath P, std::size_t Pos>
struct PathMember<T, P, Pos, PathStep::Element> {
	static_assert(pathStdArray<T>, "This path takes an element of a value that is not an array");
	static constexpr std::size_t close = digitsEnd<P, Pos + 1>();
	static constexpr bool wellFormed = close > Pos + 1 &&
	                                   close < P.view().size() && P.view()[close] == ']';
	static_assert(wellFormed, "Malformed path: [i] takes a decimal index");
	static constexpr std::size_t element = digitsValue<P, Pos + 1>();
	static constexpr bool inside = [] {
		if constexpr (pathStdArray<T>)
			return element < std::tuple_size_v<T>;
		else
			return false;
	}();
	static_assert(!pathStdArray<T> || inside,
	              "This path takes an element past the end of the array");
	static constexpr bool valid = pathStdArray<T> && wellFormed && inside;
	using Next = typename ElementNext<valid, T, P, close>::type;
	using Type = typename Next::Type;

	static constexpr const Type& select(const T& value) noexcept
	{
		if constexpr (valid)
			return Next::select(value[element]);
		else
			return Next::select(value);
	}
};

template<class T, EndpointPath P, std::size_t Pos>
struct PathMember<T, P, Pos, PathStep::Bad> {
	static_assert(pathDependentFalse<T>, "Malformed path: .member and [i] after the endpoint");
	using Type = T;

	static constexpr const T& select(const T& value) noexcept
	{
		return value;
	}
};

[[noreturn]] inline void
noEndpointAtPath() noexcept // model.idOf<path>(): no endpoint has this path
{
	std::abort();
}

} // namespace detail

// The PackedId of the endpoint at path, any family.
template<const auto& M, EndpointPath P>
[[nodiscard]] consteval PackedId idOf() noexcept
{
	using Tree = std::remove_cvref_t<decltype(M)>;
	static_assert(detail::groupTreeModel<Tree>, "idOf<model, path> takes a group-tree Model");
	constexpr auto at = detail::locatePath(M, P.view());
	static_assert(detail::PathFound<P, at.error>::value);
	static_assert(detail::PathWhole<P, at.memberStart == P.view().size()>::value);
	return makeId(static_cast<GroupId>(at.group), static_cast<EntryOffset>(at.entry));
}

// Read a Field, or one member or element of it, by path. The whole Field's
// getter runs; the result is empty when the getter is unavailable.
template<const auto& M, EndpointPath P>
[[nodiscard]] auto read() noexcept
{
	using Tree = std::remove_cvref_t<decltype(M)>;
	static_assert(detail::groupTreeModel<Tree>, "read<model, path> takes a group-tree Model");
	constexpr auto at = detail::locatePath(M, P.view());
	static_assert(detail::PathFound<P, at.error>::value);
	static_assert(detail::PathCategory<P, at.category, 1>::value);
	constexpr auto id = makeId(static_cast<GroupId>(at.group), static_cast<EntryOffset>(at.entry));
	if constexpr (at.memberStart == P.view().size()) {
		return M.fields().template read<id>();
	} else {
		const auto whole = M.fields().template read<id>();
		using Value = std::remove_cvref_t<decltype(*whole)>;
		using Selected = detail::PathMember<Value, P, at.memberStart>;
		using Part = std::remove_cvref_t<typename Selected::Type>;
		if (!whole)
			return std::optional<Part>{};
		return std::optional<Part>{Selected::select(*whole)};
	}
}

// Write a whole Field by path; the value type is checked as for fields().write<id>().
template<const auto& M, EndpointPath P, class Argument>
[[nodiscard]] WriteResult write(Argument&& value) noexcept
{
	using Tree = std::remove_cvref_t<decltype(M)>;
	static_assert(detail::groupTreeModel<Tree>, "write<model, path> takes a group-tree Model");
	constexpr auto at = detail::locatePath(M, P.view());
	static_assert(detail::PathFound<P, at.error>::value);
	static_assert(detail::PathCategory<P, at.category, 1>::value);
	static_assert(detail::PathWhole<P, at.memberStart == P.view().size()>::value);
	constexpr auto id = makeId(static_cast<GroupId>(at.group), static_cast<EntryOffset>(at.entry));
	return M.fields().template write<id>(std::forward<Argument>(value));
}

// Call a Command or Service by path, with its native arguments.
template<const auto& M, EndpointPath P, class... Args>
[[nodiscard]] decltype(auto) call(Args&&... args) noexcept
{
	using Tree = std::remove_cvref_t<decltype(M)>;
	static_assert(detail::groupTreeModel<Tree>, "call<model, path> takes a group-tree Model");
	constexpr auto at = detail::locatePath(M, P.view());
	static_assert(detail::PathFound<P, at.error>::value);
	static_assert(detail::PathCategory<P, at.category, 2, 3>::value);
	static_assert(detail::PathWhole<P, at.memberStart == P.view().size()>::value);
	constexpr auto id = makeId(static_cast<GroupId>(at.group), static_cast<EntryOffset>(at.entry));
	if constexpr (at.category == 2)
		return M.commands().template call<id>(std::forward<Args>(args)...);
	else
		return M.services().template call<id>(std::forward<Args>(args)...);
}

} // namespace telemetry

#endif
