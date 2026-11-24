/*
 * @file ArmModelCodegen.cpp
 * @brief Compare direct, handle and path native Service calls on Cortex-M7.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/model/Model.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace arm_model {

struct Request {
	std::uint32_t code;
};

struct Response {
	std::uint32_t result;
};

// Service owner supplies the same response to direct/handle/path offline ARM roots.
// API: read().
struct Device {
	std::uint32_t base;

	Response read(const Request& request) const noexcept
	{
		return {base + request.code};
	}
};

Device device{100};
inline constexpr telemetry::Model model{
    telemetry::group("device", telemetry::service<&Device::read>("Read", device))};
inline constexpr auto read = telemetry::endpoint<model, "device/Read">;

} // namespace arm_model

extern "C" __attribute__((noinline)) telemetry::ServiceResult<arm_model::Response>
model_direct(const arm_model::Request& request) noexcept
{
	return telemetry::ServiceResult<arm_model::Response>::successFrom([&]() -> arm_model::Response {
		return arm_model::device.read(request);
	});
}

extern "C" __attribute__((noinline)) telemetry::ServiceResult<arm_model::Response>
model_handle(const arm_model::Request& request) noexcept
{
	return arm_model::read.call(request);
}

extern "C" __attribute__((noinline)) telemetry::ServiceResult<arm_model::Response>
model_path(const arm_model::Request& request) noexcept
{
	return telemetry::call<arm_model::model, "device/Read">(request);
}

extern "C" __attribute__((noinline)) telemetry::EncodedCallResult
model_encoded(std::span<const std::byte> input, std::span<std::byte> output,
              telemetry::Workspace& workspace) noexcept
{
	return arm_model::model.serviceIndex().callEncoded(arm_model::read.id, input, output,
	                                                   workspace);
}
