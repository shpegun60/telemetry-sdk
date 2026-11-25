/*
 * @file main.cpp
 * @brief Generated DTO and endpoint through the unchanged asynchronous RpcClient.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include "../structured_client/Device.hpp"
#include <device_api/RpcBindings.hpp>
#include <integration/telemetry/v3/RpcMailbox.hpp>
#include <resource/protocol/Protocol.hpp>
#include <cstdio>

int main()
{
	namespace ex = client_example;
	namespace api = device_api;
	namespace rpc = resource::telemetry::v3;
	if (!api::schema::matchesHeader(std::span{ex::descriptorBytes}.first(64)))
		return 1;
	std::array<std::byte, 256> clientRequest{}, serverRequest{}, serverResult{}, clientResult{};
	std::array<std::byte, 48> packet{};
	std::array<std::byte, 64> reply{};
	constexpr auto view = ex::model.view();
	rpc::RpcMailbox mailbox{view, ex::workspace, serverRequest, serverResult};
	resource::FileSystem files{std::array{resource::file("/rpc", mailbox)}};
	::telemetry::client::rpc::RpcClient client{
	    0, clientRequest, clientResult, packet, {5, 1000, 2, 2}};
	if (client.startSession(1) != ::telemetry::client::rpc::RpcClientStatus::Ok)
		return 2;
	std::array<std::byte, telemetry::wireSize<ex::Request>> bytes{};
	if (telemetry::encode(ex::request, bytes) != telemetry::CodecStatus::Ok)
		return 3;
	api::device::types::InspectRequest request{};
	if (api::codec::decode(bytes, request) != api::codec::Status::Ok)
		return 4;
	// This explicit trust assertion follows per-peer/session schema admission.
	const auto endpoint =
	    api::rpc::binding(api::device::Inspect, ::telemetry::client::rpc::trustedRpcMapping);
	if (client.beginService(endpoint, request) != ::telemetry::client::rpc::RpcClientStatus::Ok)
		return 5;
	for (unsigned now = 0; client.active() && now < 200; ++now) {
		client.process(now);
		const auto pending = client.nextPacket();
		if (pending) {
			if (!client.onSendAccepted(pending.generation, pending.requestId))
				return 6;
			const auto r = resource::protocol::process(files.view(), pending.bytes, reply);
			if (r.status != resource::Status::Ok ||
			    !client.onReply(std::span{reply}.first(r.written), pending.generation,
			                    pending.requestId))
				return 7;
		}
		mailbox.process(now);
	}
	api::device::types::InspectResponse response{};
	if (!client.completed() ||
	    client.decodeResult(response).status != ::telemetry::client::rpc::RpcClientStatus::Ok ||
	    response.checksum != 263168)
		return 8;
	std::printf("codegen_rpc checksum=%u checks=8 failures=0\n", response.checksum);
}
