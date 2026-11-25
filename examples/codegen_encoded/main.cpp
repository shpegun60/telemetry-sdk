/*
 * @file main.cpp
 * @brief Generated DTO and codec through the existing Encoded API, without RPC.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include "../structured_client/Device.hpp"
#include <device_api/EncodedBindings.hpp>
#include <cstdio>

int main()
{
	namespace ex = client_example;
	namespace api = device_api;
	if (!api::schema::matchesHeader(std::span{ex::descriptorBytes}.first(64)))
		return 1;
	std::array<std::byte, telemetry::wireSize<ex::Request>> input{};
	std::array<std::byte, telemetry::wireSize<ex::Response>> output{};
	if (telemetry::encode(ex::request, input) != telemetry::CodecStatus::Ok)
		return 2;
	api::device::types::InspectRequest request{};
	api::device::types::InspectResponse response{};
	if (api::codec::decode(input, request) != api::codec::Status::Ok)
		return 3;
	const auto result = api::encoded::callService(ex::model.view(), api::device::Inspect, request,
	                                              input, output, ex::workspace, response);
	if (!result.ok() || response.checksum != 263168)
		return 4;
	std::printf("codegen_encoded checksum=%u checks=4 failures=0\n", response.checksum);
}
