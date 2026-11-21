/*
 * @file Device.hpp
 * @brief Native model for generated standalone, Encoded and RPC clients.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_TESTS_CODEGEN_DEVICE_HPP
#define TELEMETRY_TESTS_CODEGEN_DEVICE_HPP
#pragma once

#include "../../examples/structured_client/Device.hpp"
#include <integration/telemetry/v3/RpcMailbox.hpp>

namespace codegen_fixture {
namespace rs = ::resource::telemetry::v3;
namespace ts = telemetry;
namespace ex = client_example;

struct Empty {};

struct Scalars {
	bool flag;
	std::uint8_t u8;
	std::int8_t i8;
	std::uint16_t u16;
	std::int16_t i16;
	std::uint32_t u32;
	std::int32_t i32;
	std::uint64_t u64;
	std::int64_t i64;
	float f32;
	double f64;
	std::array<std::array<std::int8_t, 2>, 2> matrix;
	std::array<Empty, 2> empty;
};

inline constexpr Scalars initial{true,
                                 UINT8_MAX,
                                 INT8_MIN,
                                 UINT16_MAX,
                                 INT16_MIN,
                                 UINT32_MAX,
                                 INT32_MIN,
                                 UINT64_MAX,
                                 INT64_MIN,
                                 -0.0f,
                                 std::numeric_limits<double>::infinity(),
                                 {{{-1, 2}, {-3, 4}}},
                                 {}};

// These callbacks expose zero-byte outcomes independently of value decoding.
// Public methods:
// - read(): Return all canonical scalar values.
// - good(): Return an empty aggregate successfully.
// - fail(): Refuse an empty aggregate response.
// - consume(): Consume an empty aggregate and return no response.
// - accepted(): Accept work without claiming its physical completion.
struct EdgeDevice {
	unsigned calls = 0;

	Scalars read() const noexcept
	{
		return initial;
	}

	ts::ServiceResult<Empty> good() noexcept
	{
		++calls;
		return ts::ServiceResult<Empty>::success({});
	}

	ts::ServiceResult<Empty> fail() noexcept
	{
		++calls;
		return ts::ServiceResult<Empty>::failure(ts::ServiceStatus::Busy);
	}

	void consume(const Empty&) noexcept
	{
		++calls;
	}

	ts::CommandResult accepted(const Empty&) noexcept
	{
		++calls;
		return ts::CommandResult::Accepted;
	}
};

inline EdgeDevice edge;
// The client example's device group and an edge group: a tree of two groups.
inline constexpr ts::Model model{ex::deviceGroup,
                                 ts::group("edge", ts::field<&EdgeDevice::read>("Scalars", edge),
                                           ts::command<&EdgeDevice::accepted>("Accept", edge),
                                           ts::service<&EdgeDevice::good>("Good", edge),
                                           ts::service<&EdgeDevice::fail>("Fail", edge),
                                           ts::service<&EdgeDevice::consume>("Consume", edge))};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = ::telemetry::descriptor::packDescriptor<descriptor>();
inline std::array<std::byte, model.maxScratch()> scratch{};
inline ts::Workspace workspace{scratch};
} // namespace codegen_fixture
#endif // TELEMETRY_TESTS_CODEGEN_DEVICE_HPP
