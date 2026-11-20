/*
 * @file StoragePolicy.hpp
 * @brief Compile-time budget for local encoded endpoint objects.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

/*
 * Chooses where encoded endpoint payload objects live at compile time.
 *
 * Small owning values can use local storage; larger objects use caller-owned
 * Workspace leases. A Service shares the budget between its simultaneously
 * live request and result wrapper. The setting must agree across translation
 * units and describes object bytes, not total call-stack use.
 */

#ifndef TELEMETRY_CODEC_STORAGE_POLICY_HPP
#define TELEMETRY_CODEC_STORAGE_POLICY_HPP
#pragma once

#include "../Config.hpp"

#include <cstddef>
#include <type_traits>

// TELEMETRY_STRUCTURED_LOCAL_BYTES and its default live in Config.hpp with
// every other project switch; a project sets it in telemetry_config.hpp.

namespace telemetry {

static_assert(TELEMETRY_STRUCTURED_LOCAL_BYTES >= 0,
              "Local endpoint object budget cannot be negative");
inline constexpr std::size_t maxLocalObjectBytes = TELEMETRY_STRUCTURED_LOCAL_BYTES;

namespace detail {

template<class T>
inline constexpr std::size_t objectBytes = [] {
	if constexpr (std::is_void_v<T>)
		return std::size_t{0};
	else
		return sizeof(T);
}();

// The decision depends on payload size only; object alignment is handled by
// the compiler for locals and by Workspace for leased storage. Void owns no
// object and therefore is never selected for local construction.
template<class T, std::size_t Budget = maxLocalObjectBytes>
inline constexpr bool localObject = !std::is_void_v<T> && objectBytes<T> <= Budget;

// Request and result can be alive together. Give the request first use of
// the budget, then decide the actual result wrapper against the remainder.
// Compiler frame padding/register spills and nested user calls are measured
// separately; sizeof(payload) cannot promise the total stack frame size.
template<class Request, class Result>
struct ServiceStorage {
	static constexpr bool requestLocal = localObject<Request>;
	static constexpr std::size_t requestBytes = requestLocal ? objectBytes<Request> : 0;
	static constexpr bool resultLocal = localObject<Result, maxLocalObjectBytes - requestBytes>;
	static constexpr std::size_t localBytes =
	    requestBytes + (resultLocal ? objectBytes<Result> : 0);
	static_assert(localBytes <= maxLocalObjectBytes);
};

} // namespace detail
} // namespace telemetry

#endif
