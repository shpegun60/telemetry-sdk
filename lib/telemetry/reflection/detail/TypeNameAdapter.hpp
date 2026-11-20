/*
 * @file TypeNameAdapter.hpp
 * @brief Compiler-derived scoped names of enums and aggregates for descriptor v3.1.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Contains every direct dependency on compiler type spellings.
 *
 * Enums and aggregates use one __PRETTY_FUNCTION__ probe (magic_enum's type
 * name drops the scope, so it is not used). The automatic name is the type's
 * innermost enclosing scope and its own name, "adc::Config", or the name
 * alone at global or anonymous-namespace scope; only one scope level is kept.
 * The result is a substring of the spelling and is copied by the facade into
 * an array of exactly its length, so the long function signature never
 * reaches an image. A template specialization or an unnamed type has no
 * identifier and so no name; a type declared inside a function (a local
 * class) has no stable scope and is reported as such. GCC, MinGW and ARM GCC
 * share one spelling format; Clang has its own; a compiler that disagrees on
 * a name gets this parser fixed, never a fallback.
 */

#ifndef TELEMETRY_TYPE_NAME_ADAPTER_HPP
#define TELEMETRY_TYPE_NAME_ADAPTER_HPP
#pragma once

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
	for (std::string_view keyword : {"struct ", "class ", "union ", "enum "})
		if (spelling.substr(0, keyword.size()) == keyword)
			spelling.remove_prefix(keyword.size());
	return spelling;
#else
	static_cast<void>(signature);
	return {};
#endif
}

enum class SpelledScope : unsigned char {
	None,     // the name alone: global scope or an anonymous namespace
	Scoped,   // one enclosing namespace or class: "scope::name"
	Template, // a class template specialization encloses it: no name
	Function  // a function or lambda encloses it (a local class)
};

// The type's own name and its innermost enclosing scope, split at "::"
// outside <>, (), [] and {} groups. Groups keep template arguments, GCC's
// {anonymous} and localType(), and Clang's (anonymous namespace) and
// (unnamed struct at file:line) from being split.
struct SpelledName {
	std::string_view text; // "name" or "scope::name", a substring of the spelling
	std::string_view name; // the last segment
	SpelledScope scope = SpelledScope::None;
};

consteval SpelledName spelledName(std::string_view spelling) noexcept
{
	std::size_t depth = 0, last = 0, previous = 0;
	bool scoped = false;
	for (std::size_t i = 0; i < spelling.size(); ++i) {
		const char c = spelling[i];
		if (c == '<' || c == '(' || c == '[' || c == '{')
			++depth;
		else if ((c == '>' || c == ')' || c == ']' || c == '}') && depth > 0)
			--depth;
		else if (depth == 0 && c == ':' && i + 1 < spelling.size() && spelling[i + 1] == ':') {
			previous = last;
			last = i + 2;
			scoped = true;
			++i;
		}
	}
	SpelledName result{spelling.substr(last), spelling.substr(last), SpelledScope::None};
	if (!scoped)
		return result;
	const auto scope = spelling.substr(previous, last - 2 - previous);
	if (scope == "{anonymous}" || scope == "(anonymous namespace)" ||
	    scope == "`anonymous-namespace'")
		return result;
	if (scope.find('(') != std::string_view::npos) {
		result.scope = SpelledScope::Function;
		return result;
	}
	if (scope.find('<') != std::string_view::npos || scope.find('{') != std::string_view::npos ||
	    scope.find('`') != std::string_view::npos) {
		result.scope = SpelledScope::Template;
		return result;
	}
	result.text = spelling.substr(previous);
	result.scope = SpelledScope::Scoped;
	return result;
}

template<class T>
consteval SpelledName rawTypeName() noexcept
{
	return spelledName(typeSpelling(signatureText(TypeSignature<T>::text())));
}

} // namespace telemetry::reflection::detail

#endif
