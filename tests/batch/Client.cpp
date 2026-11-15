/* Real mixed batches, copied contracts and deterministic lifecycle checks. MIT. */
#include <integration/batch/Client.hpp>
#include <integration/encoded/BatchDispatcher.hpp>
#include "../dynamic_client/Fixture.hpp"

namespace bt = telemetry::integration::batch;
namespace ew = telemetry::protocol::encoded;
using namespace dynamic_fixture;

struct Harness {
	Metadata metadata;
	std::uint32_t now = 0;
	cl::Clock clock{this, +[](void* p) noexcept {
		                return static_cast<Harness*>(p)->now;
	                }};
	std::array<std::byte, 2048> tx{}, rx{}, port{}, reply{}, scratch{}, request{};
	telemetry::Workspace workspace{scratch};
	en::EncodedBackend backend{tx, rx, clock, 30000, false, true};
	dy::DynamicClient<4, 1024, 1536> client{backend, clock};
	bt::wire::Capability capability{17, metadata.schema.fingerprint(), 32, 1024, 1024};

	Harness() noexcept
	{
		device = {};
		CHECK(client.admit(metadata.schema, cl::Input{bytes}.first(64), 17) == cl::Status::Ok);
	}

	auto bind(std::span<const bt::Endpoint> endpoints) noexcept
	{
		const auto binding = bt::bindBatch(client, backend, capability, endpoints);
		CHECK(binding);
		return binding.binding;
	}

	cl::Packet offered() noexcept
	{
		auto packet = client.nextPacket();
		CHECK(packet && packet.mayExecute && packet.bytes.size() <= port.size());
		std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
		packet.bytes = cl::Input{port}.first(packet.bytes.size());
		return packet;
	}

	std::size_t response(cl::Packet packet) noexcept
	{
		const auto result = en::dispatchEncoded(model.view(), workspace, packet.session,
		                                        packet.bytes, reply, capability);
		CHECK(result);
		return result.written;
	}

	void send(cl::Packet packet) noexcept
	{
		CHECK(client.prepareHandoff(packet.session, packet.requestId));
		CHECK(client.onSendAccepted(packet.session, packet.requestId));
		const auto size = response(packet);
		CHECK(client.onReply(cl::Input{reply}.first(size), packet.session, packet.requestId));
	}
};

struct BatchObserved {
	unsigned calls = 0;
	cl::Status status = cl::Status::Busy;
	bt::ValueStatus valueStatus = bt::ValueStatus::NoValue;
	unsigned count = 0;

	void operator()(const bt::BatchResult& result) noexcept
	{
		++calls;
		status = result.operation.status;
		valueStatus = result.valueStatus;
		count = result.count;
		if (valueStatus != bt::ValueStatus::Ok)
			for (const auto& item : result.items)
				CHECK(!item.hasValue());
	}
};

static void orderedOperations() noexcept
{
	Harness h;
	const std::array<bt::Endpoint, 9> endpoints{{{cl::Operation::FieldWrite, 0},
	                                             {cl::Operation::FieldRead, 0},
	                                             {cl::Operation::Command, 1},
	                                             {cl::Operation::Service, 0},
	                                             {cl::Operation::Command, 2},
	                                             {cl::Operation::Service, 1},
	                                             {cl::Operation::Service, 2},
	                                             {cl::Operation::Service, 3},
	                                             {cl::Operation::FieldRead, 0}}};
	auto binding = h.bind(endpoints);
	const auto first = config(73), echo = config(99);
	const std::array<cl::Input, 9> payloads{first, {}, first, echo, {}, {}, {}, first, {}};
	unsigned calls = 0;
	CHECK(bt::batch(
	    h.client, binding, payloads, h.request, [&](const bt::BatchResult& result) noexcept {
		    ++calls;
		    CHECK(result.operation.succeeded() && result.valueStatus == bt::ValueStatus::Ok &&
		          result.count == 9);
		    CHECK(result.items[0].operation.succeeded() && !result.items[0].hasValue());
		    CHECK(result.items[1].hasValue() &&
		          result.items[1].value.member("value").value.get<std::uint32_t>().value == 73);
		    CHECK(result.items[2].operation.accepted() && !result.items[2].operation.executed());
		    CHECK(result.items[3].hasValue() &&
		          result.items[3].value.member("value").value.get<std::uint32_t>().value == 99);
		    CHECK(result.items[4].operation.executed());
		    CHECK(result.items[5].operation.succeeded() &&
		          result.items[5].valueStatus == bt::ValueStatus::NoValue &&
		          !result.items[5].hasValue());
		    CHECK(result.items[6].operation.succeeded() && result.items[6].hasValue() &&
		          result.items[6].operation.payload.empty());
		    CHECK(!result.items[7].operation.succeeded() &&
		          result.items[7].operation.endpointStatus ==
		              static_cast<unsigned>(telemetry::ServiceStatus::Busy));
		    CHECK(result.items[8].hasValue() &&
		          result.items[8].value.member("value").value.get<std::uint32_t>().value == 73);
	    }));
	CHECK(h.client.pending() == 1 && device.writes == 0 && device.commands == 0);
	binding = {};
	h.request.fill(std::byte{0x5a}); // The sole FIFO owns a complete copy already.
	h.send(h.offered());
	CHECK(calls == 1 && h.client.pending() == 0 && device.writes == 1 && device.reads == 2 &&
	      device.commands == 2 && device.services == 4);
	h.client.process(h.now);
	CHECK(calls == 1);
}

static void refusalAndBudgets() noexcept
{
	Harness h;
	const std::array<bt::Endpoint, 1> endpoints{{{cl::Operation::FieldWrite, 0}}};
	const auto binding = h.bind(endpoints);
	const auto value = config();
	const std::array<cl::Input, 1> payloads{value};
	auto callback = [](const bt::BatchResult&) noexcept {
	};
	CHECK(bt::bindBatch(h.client, h.backend, h.capability, {}).status ==
	      cl::Status::InvalidArgument);
	const std::array<bt::Endpoint, 1> readOnly{{{cl::Operation::FieldWrite, 1}}};
	CHECK(bt::bindBatch(h.client, h.backend, h.capability, readOnly).status ==
	      cl::Status::ReadOnly);
	const std::array<bt::Endpoint, 1> invalid{{{cl::Operation::Batch, 0}}};
	CHECK(bt::bindBatch(h.client, h.backend, h.capability, invalid).status ==
	      cl::Status::InvalidArgument);
	auto capability = h.capability;
	++capability.session;
	CHECK(bt::bindBatch(h.client, h.backend, capability, endpoints).status ==
	      cl::Status::NotAdmitted);
	capability = h.capability;
	++capability.fingerprint;
	CHECK(bt::bindBatch(h.client, h.backend, capability, endpoints).status ==
	      cl::Status::NotAdmitted);
	capability = h.capability;
	capability.requestLimit = 10;
	CHECK(bt::bindBatch(h.client, h.backend, capability, endpoints).status ==
	      cl::Status::BufferTooSmall);
	cl::Backend& erased = h.backend;
	CHECK(bt::bindBatch(h.client, erased, h.capability, endpoints).status ==
	      cl::Status::Unsupported);
	std::array<std::byte, 128> tx{}, rx{};
	en::EncodedBackend disabled{tx, rx, h.clock};
	CHECK(!disabled.supportsBatch());
	CHECK(bt::bindBatch(h.client, disabled, h.capability, endpoints).status ==
	      cl::Status::Unsupported);
	CHECK(bt::batch(h.client, binding, {}, h.request, callback).status ==
	      cl::Status::InvalidArgument);
	CHECK(bt::batch(h.client, binding, payloads,
	                cl::Output{h.request}.first(binding.requestBytes() - 1), callback)
	          .status == cl::Status::BufferTooSmall);
	CHECK(bt::batch(h.client, binding, payloads, h.request, callback, 0).status ==
	      cl::Status::InvalidArgument);
	void (*nullCallback)(const bt::BatchResult&) noexcept = nullptr;
	CHECK(bt::batch(h.client, binding, payloads, h.request, nullCallback).status ==
	      cl::Status::InvalidArgument);
	auto bad = value;
	bad[4] = std::byte{2};
	const std::array<cl::Input, 1> badPayloads{bad};
	CHECK(bt::batch(h.client, binding, badPayloads, h.request, callback).status ==
	      cl::Status::CodecError);
	const std::array<cl::Input, 1> shortPayloads{cl::Input{value}.first(4)};
	CHECK(bt::batch(h.client, binding, shortPayloads, h.request, callback).status ==
	      cl::Status::InvalidArgument);
	std::copy(value.begin(), value.end(), h.request.begin());
	const std::array<cl::Input, 1> aliased{cl::Input{h.request}.first(5)};
	CHECK(bt::batch(h.client, binding, aliased, h.request, callback).status ==
	      cl::Status::InvalidArgument);
	CHECK(h.client.pending() == 0 && !h.client.nextPacket() && device.writes == 0);
}

static void lifecycle() noexcept
{
	for (unsigned phase = 0; phase < 3; ++phase) {
		Harness h;
		const std::array<bt::Endpoint, 1> endpoints{{{cl::Operation::Command, 2}}};
		const auto binding = h.bind(endpoints);
		const std::array<cl::Input, 1> payloads{};
		BatchObserved observed;
		CHECK(bt::batch(
		    h.client, binding, payloads, h.request,
		    [&](const auto& r) noexcept {
			    observed(r);
		    },
		    50));
		cl::Packet packet;
		if (phase)
			packet = h.offered();
		if (phase == 2)
			CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
		h.now = 50;
		h.client.process(h.now);
		CHECK(observed.calls == 1 &&
		      observed.status == (phase == 2 ? cl::Status::OutcomeUnknown : cl::Status::Timeout));
		CHECK(h.client.recoveryRequired() == (phase == 2));
		CHECK(h.client.pending() == 0 && device.commands == 0);
		h.client.process(h.now);
		CHECK(observed.calls == 1);
		if (phase == 2) {
			CHECK(bt::batch(h.client, binding, payloads, h.request, [&](const auto& r) noexcept {
				      observed(r);
			      }).status == cl::Status::RecoveryRequired);
			h.client.close();
			CHECK(bt::batch(h.client, binding, payloads, h.request, [&](const auto& r) noexcept {
				      observed(r);
			      }).status == cl::Status::Closed);
		}
	}
	// A correlated acknowledgement retains command outcome after schema reset.
	Harness h;
	const std::array<bt::Endpoint, 2> endpoints{
	    {{cl::Operation::Command, 1}, {cl::Operation::FieldRead, 0}}};
	const auto binding = h.bind(endpoints);
	const auto value = config();
	const std::array<cl::Input, 2> payloads{value, {}};
	unsigned calls = 0;
	CHECK(bt::batch(
	    h.client, binding, payloads, h.request, [&](const bt::BatchResult& result) noexcept {
		    ++calls;
		    CHECK(result.operation.succeeded() && result.count == 2 &&
		          result.valueStatus == bt::ValueStatus::Stale);
		    CHECK(result.items[0].operation.accepted() && result.items[1].operation.succeeded());
		    CHECK(!result.items[0].hasValue() && !result.items[1].hasValue());
	    }));
	const auto packet = h.offered();
	CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
	const auto size = h.response(packet);
	CHECK(h.metadata.workspace.reset() == rt::Status::Ok);
	CHECK(!h.client.nextPacket());
	CHECK(h.client.onReply(cl::Input{h.reply}.first(size), packet.session, packet.requestId));
	CHECK(calls == 1 && device.commands == 1);
}

// A Batch that only reads changes nothing: lost after its handoff it times
// out as a FieldRead does, never OutcomeUnknown, and the session still needs
// its recovery because the request was handed off.
static void readOnlyLifecycle() noexcept
{
	for (unsigned phase = 0; phase < 3; ++phase) {
		Harness h;
		const std::array<bt::Endpoint, 2> endpoints{
		    {{cl::Operation::FieldRead, 0}, {cl::Operation::FieldRead, 0}}};
		const auto binding = h.bind(endpoints);
		const std::array<cl::Input, 2> payloads{};
		BatchObserved observed;
		CHECK(bt::batch(
		    h.client, binding, payloads, h.request,
		    [&](const auto& r) noexcept {
			    observed(r);
		    },
		    50));
		cl::Packet packet;
		if (phase)
			packet = h.offered();
		if (phase == 2)
			CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
		h.now = 50;
		h.client.process(h.now);
		CHECK(observed.calls == 1 && observed.status == cl::Status::Timeout);
		CHECK(h.client.recoveryRequired() == (phase == 2));
		CHECK(h.client.pending() == 0);
	}
}

static void malformedLaterValue() noexcept
{
	for (unsigned mutation = 0; mutation < 4; ++mutation) {
		Harness h;
		const std::array<bt::Endpoint, 2> endpoints{
		    {{cl::Operation::FieldRead, 0}, {cl::Operation::Service, 0}}};
		const auto binding = h.bind(endpoints);
		const auto value = config();
		const std::array<cl::Input, 2> payloads{{{}, value}};
		BatchObserved observed;
		CHECK(bt::batch(h.client, binding, payloads, h.request, [&](const auto& r) noexcept {
			observed(r);
		}));
		const auto packet = h.offered();
		CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
		const auto size = h.response(packet);
		auto body = cl::Output{h.reply}.subspan(ew::headerBytes, size - ew::headerBytes);
		if (mutation == 0)
			body[0] = std::byte{1};
		if (mutation == 1)
			body[19] = std::byte{7};
		if (mutation == 2)
			body[26] = std::byte{1};
		if (mutation == 3)
			body.back() = std::byte{2}; // A valid first DTO must not escape.
		CHECK(h.client.onReply(cl::Input{h.reply}.first(size), packet.session, packet.requestId));
		CHECK(observed.calls == 1 && observed.status == cl::Status::Ok &&
		      observed.valueStatus == bt::ValueStatus::InvalidPayload);
		CHECK(observed.count == 0);
	}
}

static void fifoAndIdentity() noexcept
{
	Harness h;
	const std::array<bt::Endpoint, 2> endpoints{
	    {{cl::Operation::Command, 2}, {cl::Operation::Command, 2}}};
	const auto binding = h.bind(endpoints);
	const std::array<cl::Input, 2> payloads{};
	unsigned calls = 0;
	std::array<cl::Submission, 4> submitted{};
	for (auto& item : submitted)
		CHECK(item = bt::batch(h.client, binding, payloads, h.request,
		                       [&](const auto& result) noexcept {
			                       ++calls;
			                       CHECK(result.operation.succeeded() ||
			                             result.operation.status == cl::Status::Cancelled);
		                       }));
	CHECK(bt::batch(h.client, binding, payloads, h.request, [](const auto&) noexcept {
	      }).status == cl::Status::QueueFull);
	CHECK(h.client.cancel(submitted[1].handle));
	for (unsigned i = 0; i < 16 && h.client.pending(); ++i) {
		const auto packet = h.client.nextPacket();
		if (packet)
			h.send(h.offered());
		else
			h.client.process(h.now);
	}
	CHECK(calls == 4 && device.commands == 6 && h.client.pending() == 0);
	Harness other;
	CHECK(bt::batch(other.client, binding, payloads, other.request, [](const auto&) noexcept {
	      }).status == cl::Status::NotAdmitted);
}

static void retryAndRpc() noexcept
{
	Harness h;
	std::array<std::byte, 2048> tx{}, rx{};
	en::RetryingEncodedBackend backend{tx, rx, h.clock, {10, 1}, false, true};
	dy::DynamicClient<4, 1024, 1536> client{backend, h.clock};
	CHECK(client.admit(h.metadata.schema, cl::Input{bytes}.first(64), 17) == cl::Status::Ok);
	const std::array<bt::Endpoint, 1> endpoints{{{cl::Operation::Command, 2}}};
	const auto binding = bt::bindBatch(client, backend, h.capability, endpoints);
	CHECK(binding && backend.supportsBatch());
	const std::array<cl::Input, 1> payloads{};
	unsigned calls = 0;
	CHECK(bt::batch(client, binding.binding, payloads, h.request, [&](const auto& result) noexcept {
		++calls;
		CHECK(result.operation.succeeded() && result.items[0].operation.executed());
	}));
	const auto first = client.nextPacket();
	CHECK(first && client.prepareHandoff(first.session, first.requestId));
	CHECK(client.onSendAccepted(first.session, first.requestId));
	const auto firstSize = h.response(first);
	CHECK(firstSize && device.commands == 1 && calls == 0);
	h.now = 10;
	client.process(h.now);
	const auto second = client.nextPacket();
	CHECK(second && second.session == first.session && second.requestId == first.requestId &&
	      std::equal(first.bytes.begin(), first.bytes.end(), second.bytes.begin(),
	                 second.bytes.end()));
	CHECK(client.prepareHandoff(second.session, second.requestId));
	CHECK(client.onSendAccepted(second.session, second.requestId));
	const auto secondSize = h.response(second);
	CHECK(client.onReply(cl::Input{h.reply}.first(secondSize), second.session, second.requestId));
	CHECK(calls == 1 && device.commands == 2);
	// RPC mapping is intentionally unavailable and cannot reach its old codec.
	std::array<std::byte, 1024> request{}, result{}, packet{};
	cl::rpc::RpcClient native{0, request, result, packet};
	telemetry::integration::rpc::RpcBackend rpcBackend{native, h.clock};
	dy::DynamicClient<4, 1024, 1536> rpcClient{rpcBackend, h.clock};
	CHECK(rpcClient.admit(h.metadata.schema, cl::Input{bytes}.first(64), 17) == cl::Status::Ok);
	CHECK(bt::bindBatch(rpcClient, rpcBackend, h.capability, endpoints).status ==
	      cl::Status::Unsupported);
	CHECK(rpcBackend.start({cl::Operation::Batch, 0, {}, 14}, h.now) == cl::Status::Unsupported);
	CHECK(!native.active() && !rpcBackend.nextPacket());
}

static void publicationAndPassiveGate() noexcept
{
	Harness h;
	const std::array<bt::Endpoint, 2> endpoints{
	    {{cl::Operation::Command, 2}, {cl::Operation::Command, 2}}};
	auto binding = h.bind(endpoints);
	const std::array<bt::Endpoint, 1> otherEndpoints{{{cl::Operation::FieldRead, 0}}};
	const auto replacement = h.bind(otherEndpoints);
	const std::array<cl::Input, 2> payloads{};
	unsigned calls = 0;

	struct ReplaceBinding {
		bt::BatchBinding* target;
		const bt::BatchBinding* replacement;
		unsigned* calls;

		ReplaceBinding(bt::BatchBinding& a, const bt::BatchBinding& b, unsigned& c) noexcept
		    : target(&a), replacement(&b), calls(&c)
		{}

		ReplaceBinding(const ReplaceBinding&) = delete;

		ReplaceBinding(ReplaceBinding&& other) noexcept
		    : target(other.target), replacement(other.replacement), calls(other.calls)
		{
			*target = *replacement;
		}

		void operator()(const bt::BatchResult& result) noexcept
		{
			++*calls;
			CHECK(result.count == 2 && result.items[0].operation.executed() &&
			      result.items[1].operation.executed());
		}
	};

	CHECK(bt::batch(h.client, binding, payloads, h.request,
	                ReplaceBinding{binding, replacement, calls}));
	CHECK(binding.entries().size() == 1);
	h.send(h.offered());
	CHECK(calls == 1 && device.commands == 2);
	for (unsigned close = 0; close < 2; ++close) {
		Harness owner;
		const auto frozen = owner.bind(endpoints);

		struct RevokeOnMove {
			decltype(owner.client)* client;
			bool close;

			RevokeOnMove(decltype(owner.client)& value, bool c) noexcept : client(&value), close(c)
			{}

			RevokeOnMove(const RevokeOnMove&) = delete;

			RevokeOnMove(RevokeOnMove&& source) noexcept
			    : client(source.client), close(source.close)
			{
				if (close)
					client->close();
				else
					client->invalidate();
				CHECK(!client->confirmQuiescence({17, true, true, true}));
			}

			void operator()(const bt::BatchResult&) noexcept
			{
				CHECK(false);
			}
		};

		const auto submitted = bt::batch(owner.client, frozen, payloads, owner.request,
		                                 RevokeOnMove{owner.client, bool(close)});
		CHECK(submitted.status == (close ? cl::Status::Closed : cl::Status::NotAdmitted));
		CHECK(owner.client.pending() == 0 && !owner.client.nextPacket());
	}
	// Make backend recovery observable without advancing the owner's core.
	Harness stale;
	const auto old = stale.bind(endpoints);
	stale.client.invalidate();
	CHECK(stale.backend.start({cl::Operation::Command, 2, {}, 0, 50}, 0) == cl::Status::Ok);
	const auto packet = stale.backend.nextPacket();
	CHECK(packet && stale.backend.prepareHandoff(packet.session, packet.requestId));
	stale.now = 50;
	stale.backend.process(stale.now);
	CHECK(stale.backend.completed() && stale.backend.recoveryRequired());
	unsigned rejectedCallbacks = 0;
	auto rejected = [&](const bt::BatchResult&) noexcept {
		++rejectedCallbacks;
	};
	CHECK(bt::batch(stale.client, old, payloads, stale.request, rejected).status ==
	      cl::Status::RecoveryRequired);
	CHECK(stale.backend.completed() && stale.client.pending() == 0 && rejectedCallbacks == 0);
	stale.client.close();
	CHECK(bt::batch(stale.client, old, payloads, stale.request, rejected).status ==
	      cl::Status::Closed);
	CHECK(rejectedCallbacks == 0);
}

static unsigned functionCalls = 0;

static void functionCompletion(const bt::BatchResult& result) noexcept
{
	CHECK(result.operation.succeeded() && result.valueStatus == bt::ValueStatus::Ok &&
	      result.count == 1);
	++functionCalls;
}

static void functionReferences() noexcept
{
	Harness h;
	const std::array<bt::Endpoint, 1> endpoints{{{cl::Operation::FieldRead, 0}}};
	const auto binding = h.bind(endpoints);
	const std::array<cl::Input, 1> payloads{};
	functionCalls = 0;
	CHECK(bt::batch(h.client, binding, payloads, h.request, functionCompletion));
	h.send(h.offered());
	CHECK(functionCalls == 1 && h.client.pending() == 0);
}

int main()
{
	functionReferences();
#ifdef BATCH_CALLBACK_TOO_SMALL
	Harness probe;
	dy::DynamicClient<> small{probe.backend, probe.clock};
	(void)bt::batch(small, bt::BatchBinding{}, {}, probe.request,
	                [](const bt::BatchResult&) noexcept {
	                });
#endif
#ifdef BATCH_FORGE
	bt::BatchBinding forged;
	forged.client_ = nullptr;
#endif
	orderedOperations();
	refusalAndBudgets();
	lifecycle();
	readOnlyLifecycle();
	malformedLaterValue();
	fifoAndIdentity();
	retryAndRpc();
	publicationAndPassiveGate();
	std::printf("Batch sizes: dynamic192=%zu dynamic1536=%zu binding=%zu result=%zu backend=%zu\n",
	            sizeof(dy::DynamicClient<4, 1024>), sizeof(dy::DynamicClient<4, 1024, 1536>),
	            sizeof(bt::BatchBinding), sizeof(bt::BatchResult), sizeof(en::EncodedBackend));
	std::printf("Batch client: %u checks, 0 failures\n", checks);
	return 0;
}
