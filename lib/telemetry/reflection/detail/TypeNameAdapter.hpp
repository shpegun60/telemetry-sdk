/*
 * @file TypeNameAdapter.hpp
 * @brief Compiler-derived short names of enums and aggregates for descriptor v3.1.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Contains every direct dependency on compiler type spellings.
 *
 * Enums use the pinned magic_enum backend, which already drops namespaces and
 * enclosing classes. Aggregates use the same __PRETTY_FUNCTION__ technique in
 * an owned probe: the spelling of T is cut after its last top-level "::" and
 * copied into an array of exactly that length, so the long function signature
 * never reaches an image. The public facade validates the result; a template
 * specialization or an unnamed struct yields no identifier and so no name.
 * GCC, MinGW and ARM GCC share one spelling format; Clang has its own; a
 * compiler that disagrees on a name gets this parser fixed, never a fallback.
 */

#ifndef TELEMETRY_TYPE_NAME_ADAPTER_HPP
#define TELEMETRY_TYPE_NAME_ADAPTER_HPP
#pragma once

#include <magic_enum.hpp>

#include <array>
#include <cstddef>
#include <string_view>

namespace telemetry::reflection::detail {

// The probe's only template parameter is T and it returns a plain pointer, so
// no typedef is spelled after T: GCC prints "[with T = ns::Name]", Clang prints
// "[T = ns::Name]" and MSVC "TypeSignature<struct ns::Name>::text(void)".
// Public methods:
// - text(): Borrow the compiler's spelling of this function.
template<class T>
struct TypeSignature {
	static consteval const char* text() noexcept
	{
#if defined(__clang__) || defined(__GNUC__)
		return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
		return __FUNCSIG__;
#else
		return "";
#endif
	}
};

consteval std::string_view signatureText(const char* text) noexcept
{
	std::size_t size = 0;
	while (text[size] != '\0')
		++size;
	return {text, size};
}

// Extract the spelling of T from the probe's signature; empty if the format is
// not recognized, which leaves the type unnamed rather than misnamed.
consteval std::string_view typeSpelling(std::string_view signature) noexcept
{
#if defined(__clang__)
	constexpr std::string_view marker = "[T = ";
	const auto begin = signature.find(marker);
	if (begin == std::string_view::npos || signature.empty() || signature.back() != ']')
		return {};
	const auto first = begin + marker.size();
	return signature.substr(first, signature.size() - 1 - first);
#elif defined(__GNUC__)
	constexpr std::string_view marker = "[with T = ";
	const auto begin = signature.find(marker);
	if (begin == std::string_view::npos || signature.empty() || signature.back() != ']')
		return {};
	const auto first = begin + marker.size();
	return signature.substr(first, signature.size() - 1 - first);
#elif defined(_MSC_VER)
	constexpr std::string_view marker = "TypeSignature<";
	constexpr std::string_view tail = ">::text(void)";
	const auto begin = signature.find(marker);
	if (begin == std::string_view::npos || signature.size() < tail.size() ||
	    signature.substr(signature.size() - tail.size()) != tail)
		return {};
	auto spelling = signature.substr(begin + marker.size(),
	                                 signature.size() - tail.size() - begin - marker.size());
	for (std::string_view keyword : {"struct ", "class ", "union "})
		if (spelling.substr(0, keyword.size()) == keyword)
			spelling.remove_prefix(keyword.size());
	return spelling;
#else
	static_cast<void>(signature);
	return {};
#endif
}

// The segment after the last "::" outside <>, (), [] and {} groups. Groups
// keep template arguments, GCC's {anonymous}, Clang's (anonymous namespace)
// and (unnamed struct at file:line) from being split; such a segment is not
// an identifier and the facade leaves the type unnamed.
consteval std::string_view shortSpelling(std::string_view spelling) noexcept
{
	std::size_t depth = 0, begin = 0;
	for (std::size_t i = 0; i < spelling.size(); ++i) {
		const char c = spelling[i];
		if (c == '<' || c == '(' || c == '[' || c == '{')
			++depth;
		else if ((c == '>' || c == ')' || c == ']' || c == '}') && depth > 0)
			--depth;
		else if (depth == 0 && c == ':' && i + 1 < spelling.size() && spelling[i + 1] == ':') {
			begin = i + 2;
			++i;
		}
	}
	return spelling.substr(begin);
}

template<class T>
consteval std::string_view rawStructName() noexcept
{
	return shortSpelling(typeSpelling(signatureText(TypeSignature<T>::text())));
}

template<class E>
consteval std::string_view rawEnumTypeName() noexcept
{
	return magic_enum::enum_type_name<E>();
}

} // namespace telemetry::reflection::detail

#endif
