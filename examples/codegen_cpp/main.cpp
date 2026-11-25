/*
 * @file main.cpp
 * @brief Standalone generated C++ codec with no Telemetry or RPC dependency.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include <device_api/Codec.hpp>
#include <cstdio>

int main()
{
	namespace api = device_api;
	api::device::types::InspectRequest request{};
	request.config.serial = UINT64_MAX;
	request.config.offset = INT64_MIN;
	request.config.gain = -0.0f;
	request.config.precise = 0.125;
	request.config.enabled = true;
	request.config.mode = static_cast<decltype(request.config.mode)>(-2);
	request.config.samples = {1, 256, 65535};
	request.multiplier = 4;
	std::array<std::byte, api::codec::wireSize<decltype(request)>> bytes{};
	const auto endpoint = api::device::Inspect;
	if (api::codec::encode(endpoint, request, bytes) != api::codec::Status::Ok)
		return 1;
	// A custom transport sends Operation::Service, endpoint.id and these bytes.
	// Its framing and response/application status policy belong to that transport.
	api::device::types::InspectRequest decoded{};
	if (api::codec::decode(bytes, decoded) != api::codec::Status::Ok || decoded.multiplier != 4)
		return 2;
	std::printf("codegen_cpp endpoint=%u bytes=%zu checks=2 failures=0\n", endpoint.id,
	            bytes.size());
}
