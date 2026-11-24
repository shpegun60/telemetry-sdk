/*
 * @file Fixture.hpp
 * @brief Native rich-value model and real fixed-storage Inspector transport fixtures.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_TEST_INSPECTOR_FIXTURE_HPP
#define TELEMETRY_TEST_INSPECTOR_FIXTURE_HPP
#pragma once

#include "../runtime_schema/Fixture.hpp"
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <integration/inspector/InspectorModel.hpp>
#include <integration/rpc/Backend.hpp>
#include <integration/telemetry/v3/RpcSessionPool.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <variant>

namespace inspector_fixture {
namespace cl = telemetry::client;
namespace rt = telemetry::runtime;
namespace im = telemetry::integration::inspector;
namespace en = telemetry::integration::encoded;
namespace rpc = resource::telemetry::v3;
using Packet = runtime_fixture::Packet;
using Empty = runtime_fixture::Empty;
inline unsigned checks = 0;

inline void check(bool ok, int line) noexcept
{
	++checks;
	if (!ok) {
		std::fprintf(stderr, "inspector check %u failed at line %d\n", checks, line);
		std::exit(1);
	}
}

#define CHECK(...) ::inspector_fixture::check(bool(__VA_ARGS__), __LINE__)

struct Device {
	Packet packet = runtime_fixture::initial;
	unsigned reads = 0, writes = 0, commands = 0, services = 0;

	Packet read() noexcept
	{
		++reads;
		return packet;
	}

	telemetry::WriteResult write(const Packet& value) noexcept
	{
		++writes;
		packet = value;
		return telemetry::WriteResult::Applied;
	}

	Empty empty() noexcept
	{
		++reads;
		return {};
	}

	std::array<std::uint16_t, 0> zero() noexcept
	{
		++reads;
		return {};
	}

	std::array<Empty, 65536> hugeZero() noexcept
	{
		++reads;
		return {};
	}

	std::uint64_t wide() noexcept
	{
		++reads;
		return packet.u64;
	}

	std::int64_t signedWide() noexcept
	{
		++reads;
		return packet.i64;
	}

	bool flag() noexcept
	{
		++reads;
		return packet.flag;
	}

	telemetry::CommandResult command() noexcept
	{
		++commands;
		return telemetry::CommandResult::Executed;
	}

	telemetry::ServiceResult<Packet> service() noexcept
	{
		++services;
		return telemetry::ServiceResult<Packet>::success(packet);
	}
};

inline Device device;
inline constexpr telemetry::Model model{
    telemetry::group("inspect", telemetry::field<&Device::read, &Device::write>("Packet", device),
                     telemetry::field<&Device::empty>("Empty", device),
                     telemetry::field<&Device::zero>("Zero", device),
                     telemetry::field<&Device::hugeZero>("HugeZero", device),
                     telemetry::field<&Device::wide>("U64", device),
                     telemetry::field<&Device::signedWide>("S64", device),
                     telemetry::field<&Device::flag>("Flag", device),
                     telemetry::command<&Device::command>("Command", device),
                     telemetry::service<&Device::service>("Service", device))};
inline const auto modelView = model.view();
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto bytes = telemetry::descriptor::packDescriptor<descriptor>();

struct Metadata {
	std::array<rt::RuntimeType, 64> types{};
	std::array<rt::RuntimeMember, 128> members{};
	std::array<rt::RuntimeEnumEntry, 16> enums{};
	std::array<rt::RuntimeCatalog, 8> catalogs{};
	std::array<rt::RuntimeEndpoint, 32> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	rt::RuntimeSchema schema{};

	Metadata() noexcept
	{
		const auto parsed = rt::RuntimeSchema::parse(bytes, workspace);
		CHECK(parsed);
		schema = parsed.schema;
	}
};

template<std::size_t N = 4, std::size_t SnapshotBytes = 512>
struct Encoded {
	Metadata metadata;
	std::uint32_t now = 0;
	cl::Clock clock{this, +[](void* p) noexcept {
		                return static_cast<Encoded*>(p)->now;
	                }};
	std::array<std::byte, 1024> tx{}, rx{}, port{}, reply{};
	// The native producer materializes HugeZero; its wire payload and Inspector snapshot are empty.
	std::array<std::byte, 65552> scratch{};
	telemetry::Workspace workspace{scratch};
	en::EncodedBackend backend{tx, rx, clock};
	im::InspectorModel<N, 512, SnapshotBytes, 256> inspector{backend, clock};

	Encoded() noexcept
	{
		device = {};
		CHECK(inspector.admit(metadata.schema, cl::Input{bytes}.first(64), 17) == cl::Status::Ok);
	}

	auto bind(std::uint32_t id = 0) noexcept
	{
		const auto binding = inspector.bindField(id);
		CHECK(binding);
		return binding.endpoint;
	}

	cl::Packet offered() noexcept
	{
		auto packet = inspector.nextPacket();
		CHECK(packet && packet.bytes.size() <= port.size());
		std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
		packet.bytes = cl::Input{port}.first(packet.bytes.size());
		return packet;
	}

	void send(cl::Packet packet, bool deliver = true) noexcept
	{
		CHECK(inspector.prepareHandoff(packet.session, packet.requestId));
		CHECK(inspector.onSendAccepted(packet.session, packet.requestId));
		const auto result =
		    en::dispatchEncoded(modelView, workspace, packet.session, packet.bytes, reply);
		CHECK(result && result.written <= reply.size());
		if (deliver)
			CHECK(inspector.onReply(cl::Input{reply}.first(result.written), packet.session,
			                        packet.requestId));
		inspector.process(now);
	}

	void pump() noexcept
	{
		for (unsigned i = 0; i != 32 && inspector.pending(); ++i)
			send(offered());
		CHECK(inspector.pending() == 0);
	}
};

struct RpcHarness {
	Metadata metadata;
	std::uint32_t now = 0;
	cl::Clock clock{this, +[](void* p) noexcept {
		                return static_cast<RpcHarness*>(p)->now;
	                }};
	std::array<std::byte, 2048> scratch{}, serverRequest{}, serverResult{}, request{}, result{};
	telemetry::Workspace workspace{scratch};
	rpc::RpcMailbox mailbox{modelView, workspace, serverRequest, serverResult};
	const resource::FileSystem<1> files{resource::filesystem(resource::file("/rpc", mailbox))};
	rpc::RpcSessionPool<1> pool{std::array{rpc::RpcSessionBinding{mailbox, files.view()}}};
	rpc::RpcSessionHandle session = pool.acquire({7, 1, 0}).session;
	std::array<std::byte, 40> packet{}, port{};
	std::array<std::byte, 48> reply{};
	cl::rpc::RpcClient native{0, request, result, packet, {3, 40, 1, 1}};
	telemetry::integration::rpc::RpcBackend backend{native, clock};
	im::InspectorModel<4, 512, 512, 256> inspector{backend, clock};
	unsigned packets = 0, readPackets = 0, writePackets = 0;

	RpcHarness() noexcept
	{
		device = {};
		CHECK(pool.valid(session));
		CHECK(inspector.admit(metadata.schema, cl::Input{bytes}.first(64), 1) == cl::Status::Ok);
	}

	auto bind(std::uint32_t id = 0) noexcept
	{
		const auto binding = inspector.bindField(id);
		CHECK(binding);
		return binding.endpoint;
	}

	bool exchange() noexcept
	{
		auto offered = inspector.nextPacket();
		if (!offered) {
			(void)pool.process(++now);
			inspector.process(now);
			return false;
		}
		++packets;
		if (offered.bytes[0] == std::byte{static_cast<std::uint8_t>(resource::protocol::Op::Read)})
			++readPackets;
		if (offered.bytes[0] == std::byte{static_cast<std::uint8_t>(resource::protocol::Op::Write)})
			++writePackets;
		CHECK(offered.bytes.size() <= port.size());
		std::copy(offered.bytes.begin(), offered.bytes.end(), port.begin());
		CHECK(inspector.prepareHandoff(offered.session, offered.requestId));
		CHECK(inspector.onSendAccepted(offered.session, offered.requestId));
		const auto response =
		    pool.handle(session, cl::Input{port}.first(offered.bytes.size()), reply);
		CHECK(response.status == rpc::RpcSessionStatus::Ok &&
		      response.reply.status == resource::Status::Ok);
		if (response.ticket.serial)
			CHECK(pool.replyAccepted(session, response.ticket) == rpc::RpcSessionStatus::Ok);
		CHECK(inspector.onReply(cl::Input{reply}.first(response.reply.written), offered.session,
		                        offered.requestId));
		(void)pool.process(now);
		++now;
		return true;
	}

	void pump() noexcept
	{
		for (unsigned i = 0; i != 300 && inspector.pending(); ++i)
			(void)exchange();
		CHECK(inspector.pending() == 0);
	}
};

template<class Model>
auto child(Model& model, auto node, std::uint32_t index) noexcept
{
	const auto value = model.child(node, index);
	CHECK(value);
	return value.value;
}

template<class T, class Model>
T scalar(Model& model, auto node) noexcept
{
	const auto value = model.scalar(node);
	CHECK(value);
	const auto* exact = std::get_if<T>(&value.value.value);
	CHECK(exact);
	return *exact;
}
} // namespace inspector_fixture
#endif // TELEMETRY_TEST_INSPECTOR_FIXTURE_HPP
