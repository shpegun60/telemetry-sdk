/*
 * @file Device.hpp
 * @brief Reflected DTOs and a fake device for the v3 client example.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Supply exact-width DTOs shared by the browser and Qt demonstrations.
 * Process-lifetime owners, tables and scratch make every borrowed binding
 * stable; service outcomes exercise application policy independently of wire types.
 */

#ifndef TELEMETRY_EXAMPLES_STRUCTURED_CLIENT_DEVICE_HPP
#define TELEMETRY_EXAMPLES_STRUCTURED_CLIENT_DEVICE_HPP
#pragma once

#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <integration/telemetry/v3/ValuesFile.hpp>
#include <array>
#include <limits>

namespace client_example {
namespace ts = telemetry;
namespace rs = resource::telemetry::v3;

enum class Mode : std::int16_t {
	Off = -1,
	On = 2
};

struct Config {
	std::uint64_t serial;
	std::int64_t offset;
	float gain;
	double precise;
	bool enabled;
	Mode mode;
	std::array<std::uint16_t, 3> samples;
};

struct Request {
	Config config;
	std::uint32_t multiplier;
};

struct Response {
	Config config;
	std::uint32_t checksum;
};

inline constexpr Config initial{
    UINT64_MAX, INT64_MIN, -0.0f, 0.125, true, static_cast<Mode>(-2), {1, 256, 65535}};
inline constexpr Request request{initial, 4};

// Public methods:
// - readConfig(): Copy current configuration.
// - writeConfig(): Replace current configuration.
// - apply(): Replace current configuration.
// - reset(): Restore example defaults.
// - inspect(): Inspect typed request.
// - ping(): Count statusless call.
struct Device {
	Config config = initial;
	unsigned calls = 0;

	Config readConfig() const noexcept
	{
		return config;
	}

	telemetry::WriteResult writeConfig(const Config& value) noexcept
	{
		++calls;
		config = value;
		return telemetry::WriteResult::Applied;
	}

	telemetry::CommandResult apply(const Config& value) noexcept
	{
		++calls;
		config = value;
		return telemetry::CommandResult::Executed;
	}

	telemetry::CommandResult reset() noexcept
	{
		++calls;
		config = initial;
		return telemetry::CommandResult::Executed;
	}

	ts::ServiceResult<Response> inspect(const Request& value) noexcept
	{
		++calls;
		// These are application decisions, not descriptor limits/defaults.
		switch (value.multiplier) {
			case 0:
				return ts::ServiceResult<Response>::failure(ts::ServiceStatus::InvalidArgument);
			case 1:
				return ts::ServiceResult<Response>::failure(ts::ServiceStatus::Busy);
			case 2:
				return ts::ServiceResult<Response>::failure(ts::ServiceStatus::Unavailable);
			case 3:
				return ts::ServiceResult<Response>::failure(ts::ServiceStatus::Failed);
			default:
				break;
		}
		std::uint32_t sum = 0;
		for (auto sample : value.config.samples)
			sum += sample;
		return ts::ServiceResult<Response>::success({value.config, sum * value.multiplier});
	}

	void ping() noexcept
	{
		++calls;
	}
};

inline Device device;
inline telemetry::FunctionSlot<Response(const Request&) noexcept> offline;
// One group, device: a Field, two Commands and three Services.
inline constexpr auto deviceGroup = ts::group(
    "device", ts::field<&Device::readConfig, &Device::writeConfig>("Config", device),
    ts::command<&Device::apply>("Apply", device), ts::command<&Device::reset>("Reset", device),
    ts::service<&Device::inspect>("Inspect", device), ts::service<&Device::ping>("Ping", device),
    ts::service("Offline", offline));
inline constexpr ts::Model model{deviceGroup};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = ::telemetry::descriptor::packDescriptor<descriptor>();

// The desktop fake owns its scratch explicitly, just like an MCU integrator.
inline std::array<std::byte, 2048> scratch;
inline ts::Workspace workspace{scratch};
inline constexpr rs::ValuesFile values{descriptor, workspace};
} // namespace client_example

#endif // TELEMETRY_EXAMPLES_STRUCTURED_CLIENT_DEVICE_HPP
