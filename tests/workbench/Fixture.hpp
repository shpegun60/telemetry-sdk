/* Real rich-value Encoded/RPC Workbench fixtures. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
#ifndef TELEMETRY_TEST_WORKBENCH_FIXTURE_HPP
#define TELEMETRY_TEST_WORKBENCH_FIXTURE_HPP
#pragma once
#include "../inspector/Fixture.hpp"
#include <integration/workbench/WorkbenchModel.hpp>

namespace workbench_fixture {
namespace cl = inspector_fixture::cl;
namespace rt = inspector_fixture::rt;
namespace im = inspector_fixture::im;
namespace en = inspector_fixture::en;
namespace rpc = inspector_fixture::rpc;
using inspector_fixture::Packet;
using inspector_fixture::child;
using inspector_fixture::scalar;
namespace wb = telemetry::integration::workbench;
namespace dy = telemetry::integration::dynamic;
namespace wire = telemetry::protocol::encoded;

struct Owner {
	Packet packet = runtime_fixture::initial;
	unsigned reads = 0, writes = 0, commands = 0, services = 0;
	bool refuse = false;

	Packet read() noexcept
	{
		++reads;
		return packet;
	}

	telemetry::WriteResult write(const Packet& value) noexcept
	{
		++writes;
		if (refuse)
			return telemetry::WriteResult::InvalidValue;
		packet = value;
		packet.u32 = std::min(packet.u32, std::uint32_t{100});
		return telemetry::WriteResult::Applied;
	}

	std::uint64_t wide() noexcept
	{
		++reads;
		return packet.u64;
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

inline Owner owner;
inline constexpr telemetry::Model model{
    telemetry::group("workbench", telemetry::field<&Owner::read, &Owner::write>("Config", owner),
                     telemetry::field<&Owner::wide>("ReadOnly", owner),
                     telemetry::command<&Owner::command>("Command", owner),
                     telemetry::service<&Owner::service>("Service", owner))};
inline const auto view = model.view();
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = telemetry::descriptor::packDescriptor<descriptor>();

struct Metadata {
	std::array<rt::RuntimeType, 64> types{};
	std::array<rt::RuntimeMember, 128> members{};
	std::array<rt::RuntimeEnumEntry, 16> enums{};
	std::array<rt::RuntimeCatalog, 8> catalogs{};
	std::array<rt::RuntimeEndpoint, 32> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	rt::RuntimeSchema schema;

	explicit Metadata(rt::Input descriptor = descriptorBytes) noexcept
	{
		const auto parsed = rt::RuntimeSchema::parse(descriptor, workspace);
		CHECK(parsed);
		schema = parsed.schema;
	}
};

struct FieldData {
	static auto descriptor() noexcept
	{
		return cl::Input{descriptorBytes};
	}

	static const auto& model() noexcept
	{
		return view;
	}

	static void reset() noexcept
	{
		owner = {};
	}
};

template<std::size_t N = 4, std::size_t PayloadBytes = 512, std::size_t SnapshotBytes = 512,
         class Data = FieldData, std::size_t ResponseBytes = 0>
struct Encoded {
	Metadata metadata{Data::descriptor()};
	std::uint32_t now = 0;
	cl::Clock clock{this, +[](void* p) noexcept {
		                return static_cast<Encoded*>(p)->now;
	                }};
	std::array<std::byte, 1024> tx{}, rx{}, port{}, reply{}, scratch{};
	telemetry::Workspace workspace{scratch};
	en::EncodedBackend backend{tx, rx, clock};
	std::array<std::byte, ResponseBytes> responseStorage{};
	wb::WorkbenchModel<N, PayloadBytes, SnapshotBytes, 256> inspector{backend, clock,
	                                                                  responseStorage};

	Encoded() noexcept
	{
		Data::reset();
		CHECK(inspector.admit(metadata.schema, Data::descriptor().first(64), 17) == cl::Status::Ok);
	}

	~Encoded()
	{
		inspector.close();
		inspector.process(now);
		CHECK(inspector.drained());
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

	std::size_t send(cl::Packet packet, bool deliver = true) noexcept
	{
		CHECK(inspector.prepareHandoff(packet.session, packet.requestId));
		CHECK(inspector.onSendAccepted(packet.session, packet.requestId));
		const auto result =
		    en::dispatchEncoded(Data::model(), workspace, packet.session, packet.bytes, reply);
		CHECK(result && result.written <= reply.size());
		if (deliver)
			CHECK(inspector.onReply(cl::Input{reply}.first(result.written), packet.session,
			                        packet.requestId));
		inspector.process(now);
		return result.written;
	}

	void pump() noexcept
	{
		for (unsigned i = 0; i != 32 && inspector.pending(); ++i)
			(void)send(offered());
		CHECK(inspector.pending() == 0);
	}
};

template<class Data = FieldData, std::size_t ResponseBytes = 0>
struct RpcModel {
	Metadata metadata{Data::descriptor()};
	std::uint32_t now = 0;
	cl::Clock clock{this, +[](void* p) noexcept {
		                return static_cast<RpcModel*>(p)->now;
	                }};
	std::array<std::byte, 2048> scratch{}, serverRequest{}, serverResult{}, request{}, result{};
	telemetry::Workspace workspace{scratch};
	rpc::RpcMailbox mailbox{Data::model(), workspace, serverRequest, serverResult};
	const resource::FileSystem<1> files{resource::filesystem(resource::file("/rpc", mailbox))};
	rpc::RpcSessionPool<1> pool{std::array{rpc::RpcSessionBinding{mailbox, files.view()}}};
	rpc::RpcSessionHandle session = pool.acquire({7, 1, 0}).session;
	std::array<std::byte, 40> packet{}, port{};
	std::array<std::byte, 48> reply{};
	cl::rpc::RpcClient native{0, request, result, packet, {3, 40, 1, 1}};
	telemetry::integration::rpc::RpcBackend backend{native, clock};
	std::array<std::byte, ResponseBytes> responseStorage{};
	wb::WorkbenchModel<4, 512, 512, 256> inspector{backend, clock, responseStorage};
	unsigned packets = 0;

	RpcModel() noexcept
	{
		Data::reset();
		CHECK(pool.valid(session));
		CHECK(inspector.admit(metadata.schema, Data::descriptor().first(64), 1) == cl::Status::Ok);
	}

	~RpcModel()
	{
		inspector.close();
		inspector.process(now);
		CHECK(inspector.drained());
	}

	auto bind(std::uint32_t id = 0) noexcept
	{
		const auto binding = inspector.bindField(id);
		CHECK(binding);
		return binding.endpoint;
	}

	void pump() noexcept
	{
		for (unsigned i = 0; i != 400 && inspector.pending(); ++i) {
			auto offered = inspector.nextPacket();
			if (!offered) {
				(void)pool.process(++now);
				inspector.process(now);
				continue;
			}
			++packets;
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
		}
		CHECK(inspector.pending() == 0);
	}
};

using Rpc = RpcModel<>;

template<class Fixture>
auto read(Fixture& f, std::uint32_t id = 0) noexcept
{
	const auto binding = f.bind(id);
	unsigned callbacks = 0;
	CHECK(f.inspector.readField(binding, [&](const im::ReadResult& result) noexcept {
		++callbacks;
		CHECK(result.operation.succeeded() && result.published);
	}));
	f.pump();
	CHECK(callbacks == 1);
	const auto root = f.inspector.root();
	CHECK(root);
	return root.value;
}

template<class T, class Model>
T edited(Model& model, const im::NodeHandle& node) noexcept
{
	const auto value = model.editScalar(node);
	CHECK(value);
	const auto* number = std::get_if<T>(&value.value.value);
	CHECK(number);
	return *number;
}
} // namespace workbench_fixture
#endif
