/* Real Encoded/Retrying/RPC DynamicClient admission and execution checks. MIT. */
#include "Fixture.hpp"
using namespace dynamic_fixture;

namespace alternate {
struct Config {
	float value;
	bool enabled;
};

struct Device {
	unsigned reads = 0;

	Config read() noexcept
	{
		++reads;
		return {3.25f, true};
	}
};

inline Device device;
inline constexpr telemetry::Model model{
    telemetry::group("device", telemetry::field<&Device::read>("Config", device))};
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto bytes = telemetry::descriptor::packDescriptor<descriptor>();
} // namespace alternate

static void validReplacementSameIds() noexcept
{
	Encoded h;
	Observed old;
	const auto binding = h.bind(cl::Operation::FieldRead, 0);
	const auto integer = h.metadata.schema.resolve(binding.responseType(), "value");
	CHECK(integer);
	CHECK(h.metadata.schema.type(integer.value.leafType())->scalarCode ==
	      telemetry::ScalarCode::U32);
	CHECK(h.client.submit(binding, {}, [&](const auto& r) noexcept {
		old(r);
	}));
	CHECK(device.reads == 0 && h.client.pending() == 1);
	const auto replacement = rt::RuntimeSchema::parse(alternate::bytes, h.metadata.workspace);
	CHECK(replacement);
	h.metadata.schema = replacement.schema;
	const auto* endpoint = replacement.schema.findField("device", "Config");
	CHECK(endpoint && endpoint->id == binding.id());
	const auto floating = replacement.schema.resolve(endpoint->typeId, "value");
	CHECK(floating);
	CHECK(replacement.schema.type(floating.value.leafType())->scalarCode ==
	      telemetry::ScalarCode::F32);
	CHECK(endpoint->typeId == binding.responseType());
	CHECK(replacement.schema.type(endpoint->typeId)->wireBytes == binding.responseBytes());
	CHECK(!h.client.isCurrent(binding));
	CHECK(!h.client.nextPacket());
	h.client.process(h.now);
	CHECK(old.calls == 1 && old.status == cl::Status::Cancelled &&
	      old.valueStatus == dy::ValueStatus::Stale);
	CHECK(device.reads == 0 && h.client.pending() == 0);
	CHECK(h.client.admit(replacement.schema, cl::Input{alternate::bytes}.first(64), 18) !=
	      cl::Status::Ok);
	CHECK(h.client.confirmQuiescence({17, true, true, true}));
	CHECK(h.client.admit(replacement.schema, cl::Input{alternate::bytes}.first(64), 18) ==
	      cl::Status::Ok);
	CHECK(!h.client.isCurrent(binding));
	CHECK(h.client
	          .submit(binding, {},
	                  [](const auto&) noexcept {
	                  })
	          .status == cl::Status::NotAdmitted);
	auto fresh = h.client.bind(cl::Operation::FieldRead, 0);
	CHECK(fresh);
	CHECK(fresh.endpoint.id() == binding.id());
	unsigned completions = 0;
	bool correct = false;
	CHECK(h.client.submit(fresh.endpoint, {}, [&](const dy::DynamicResult& r) noexcept {
		++completions;
		CHECK(r.operation.succeeded() && r.hasValue() && r.valueStatus == dy::ValueStatus::Ok);
		const auto member = r.value.member("value");
		CHECK(member);
		const auto value = member.value.get<float>();
		CHECK(value);
		correct = value.value == 3.25f;
	}));
	auto packet = h.offered();
	CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
	CHECK(h.client.onSendAccepted(packet.session, packet.requestId));
	auto reply = en::dispatchEncoded(alternate::model.view(), h.workspace, packet.session,
	                                 packet.bytes, h.reply);
	CHECK(reply.status == en::EncodedDispatchStatus::Ok);
	CHECK(h.client.onReply(cl::Input{h.reply}.first(reply.written), packet.session,
	                       packet.requestId));
	CHECK(completions == 1 && correct && alternate::device.reads == 1 && device.reads == 0 &&
	      old.calls == 1);
}

static void ordinary() noexcept
{
	Encoded h;
	Observed read, write, command, service, empty, voidResult, busy, accepted;
	const auto field = h.bind(cl::Operation::FieldRead, 0);
	CHECK(field.requestType() == 0 && field.responseBytes() == 5);
	const auto payload = config(123, true);
	CHECK(
	    h.client.submit(h.bind(cl::Operation::FieldWrite, 0), payload, [&](const auto& r) noexcept {
		    write(r);
	    }));
	CHECK(h.client.submit(field, {}, [&](const auto& r) noexcept {
		read(r);
	}));
	CHECK(h.client.submit(h.bind(cl::Operation::Command, 0), payload, [&](const auto& r) noexcept {
		command(r);
	}));
	CHECK(h.client.submit(h.bind(cl::Operation::Service, 0), payload, [&](const auto& r) noexcept {
		service(r);
	}));
	h.pump();
	CHECK(write.calls == 1 && write.status == cl::Status::Ok &&
	      write.valueStatus == dy::ValueStatus::NoValue);
	CHECK(read.calls == 1 && read.hasValue && read.number == 123 &&
	      read.valueStatus == dy::ValueStatus::Ok);
	CHECK(command.calls == 1 && command.executed && !command.hasValue);
	CHECK(service.hasValue && service.number == 123);
	CHECK(device.writes == 1 && device.reads == 1 && device.commands == 1 && device.services == 1);
	CHECK(h.client.submit(h.bind(cl::Operation::FieldRead, 2), {}, [&](const auto& r) noexcept {
		empty(r);
	}));
	CHECK(h.client.submit(h.bind(cl::Operation::Service, 1), {}, [&](const auto& r) noexcept {
		voidResult(r);
	}));
	CHECK(h.client.submit(h.bind(cl::Operation::Service, 3), payload, [&](const auto& r) noexcept {
		busy(r);
	}));
	CHECK(h.client.submit(h.bind(cl::Operation::Command, 1), payload, [&](const auto& r) noexcept {
		accepted(r);
	}));
	h.pump();
	CHECK(empty.calls == 1 && empty.hasValue && empty.size == 0 &&
	      empty.valueStatus == dy::ValueStatus::Ok);
	CHECK(voidResult.calls == 1 && !voidResult.hasValue &&
	      voidResult.valueStatus == dy::ValueStatus::NoValue);
	CHECK(busy.calls == 1 && !busy.hasValue && busy.valueStatus == dy::ValueStatus::NoValue);
	CHECK(accepted.calls == 1 && accepted.accepted && !accepted.executed && !accepted.hasValue);
	// Copying callback bytes preserves data after the backend response storage is reused.
	CHECK(read.snapshot[0] == std::byte{123});
	CHECK(read.number == 123);
}

static void ownedRequestSnapshot() noexcept
{
	Encoded h;
	Observed result;
	auto payload = config(91, true);
	CHECK(h.client.submit(h.bind(cl::Operation::Command, 0), payload, [&](const auto& r) noexcept {
		result(r);
	}));
	payload = config(999, false);
	h.pump();
	CHECK(result.calls == 1 && result.executed);
	CHECK(device.current.value == 91 && device.current.enabled && device.commands == 1);
}

struct PayloadMoveProbe {
	std::byte* source;
	unsigned* moves;
	unsigned mutateAt;
	Observed* result;

	PayloadMoveProbe(std::byte* bytes, unsigned& count, unsigned when, Observed& observed) noexcept
	    : source(bytes), moves(&count), mutateAt(when), result(&observed)
	{}

	PayloadMoveProbe(const PayloadMoveProbe&) = delete;

	PayloadMoveProbe(PayloadMoveProbe&& other) noexcept
	    : source(other.source), moves(other.moves), mutateAt(other.mutateAt), result(other.result)
	{
		if (++*moves == mutateAt)
			source[4] = std::byte{2};
	}

	void operator()(const dy::DynamicResult& completed) noexcept
	{
		(*result)(completed);
	}
};

static void requestMutationDuringOwnership() noexcept
{
	for (const auto operation :
	     {cl::Operation::FieldWrite, cl::Operation::Command, cl::Operation::Service}) {
		for (const unsigned mutateAt : {1U, 2U}) {
			Encoded h;
			Observed result;
			auto payload = config(111, true);
			const auto endpoint = h.bind(operation, 0);
			CHECK(h.metadata.schema.view(endpoint.requestType(), payload));
			unsigned moves = 0;
			const auto submitted = h.client.submit(
			    endpoint, payload, PayloadMoveProbe{payload.data(), moves, mutateAt, result});
			CHECK(moves >= mutateAt && payload[4] == std::byte{2});
			if (mutateAt == 1) {
				// The wrapper's first ownership move precedes the FIFO copy.
				CHECK(submitted.status == cl::Status::CodecError);
				CHECK(h.client.pending() == 0 && !h.client.nextPacket() && result.calls == 0);
				CHECK(device.writes == 0 && device.commands == 0 && device.services == 0);
			} else {
				// Later moves run only after the core has copied the canonical bytes.
				CHECK(submitted);
				auto packet = h.offered();
				const auto parsed = telemetry::protocol::encoded::parse(packet.bytes);
				CHECK(parsed && parsed.payload.size() == 5 && parsed.payload[4] == std::byte{1});
				CHECK(h.metadata.schema.view(endpoint.requestType(), parsed.payload));
				h.send(packet);
				CHECK(result.calls == 1 && result.status == cl::Status::Ok);
				if (operation == cl::Operation::Service)
					CHECK(result.hasValue && result.number == 111 &&
					      result.snapshot[4] == std::byte{1});
				else
					CHECK(device.current.value == 111 && device.current.enabled);
				CHECK(device.writes + device.commands + device.services == 1);
			}
			h.client.close();
			h.client.process(h.now);
			CHECK(h.client.drained());
		}
	}
}

static void refusals() noexcept
{
	Encoded h;
	Observed result;
	const auto cb = [&](const auto& r) noexcept {
		result(r);
	};
	CHECK(h.client.bind(cl::Operation::FieldWrite, 1).status == cl::Status::ReadOnly);
	CHECK(!h.client.bind(cl::Operation::Service, 999));
	CHECK(!h.client.bind(static_cast<cl::Operation>(0), 0));
	auto data = config();
	data[4] = std::byte{2};
	const auto command = h.bind(cl::Operation::Command, 0);
	CHECK(!h.client.submit(command, data, cb));
	CHECK(result.calls == 0 && h.client.pending() == 0);
	CHECK(!h.client.submit(command, cl::Input{data}.first(4), cb));
	CHECK(!h.client.submit(h.bind(cl::Operation::Command, 2), config(), cb));
	CHECK(!h.client.submit(dy::EndpointBinding{}, {}, cb));
	Encoded other;
	CHECK(!other.client.submit(command, config(), cb));
	CHECK(result.calls == 0);
	// Exact observed header admission; fingerprint equality is not a 64-byte check.
	auto header = std::array<std::byte, 64>{};
	std::copy_n(bytes.begin(), 64, header.begin());
	header[24] ^= std::byte{1};
	CHECK(h.client.admit(h.metadata.schema, header, 18) != cl::Status::Ok);
	h.client.close();
	h.client.process(h.now);
	CHECK(h.client.submit(command, config(), cb).status == cl::Status::Closed);
}

static void beforeAndAfterAuthorization() noexcept
{
	for (unsigned phase = 0; phase < 3; ++phase) {
		Encoded h;
		Observed active, queued;
		const auto b = h.bind(cl::Operation::Command, 0);
		CHECK(h.client.submit(
		    b, config(),
		    [&](const auto& r) noexcept {
			    active(r);
		    },
		    20));
		CHECK(h.client.submit(
		    b, config(),
		    [&](const auto& r) noexcept {
			    queued(r);
		    },
		    20));
		auto packet = h.offered();
		if (phase)
			CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
		CHECK(h.metadata.workspace.reset() == rt::Status::Ok);
		CHECK(!h.client.prepareHandoff(packet.session, packet.requestId));
		CHECK(!h.client.nextPacket());
		CHECK(h.client
		          .submit(b, config(),
		                  [](const auto&) noexcept {
		                  })
		          .status == cl::Status::NotAdmitted);
		h.client.process(h.now);
		CHECK(queued.calls == 1 && queued.status == cl::Status::Cancelled);
		if (!phase) {
			CHECK(active.calls == 1 && active.status == cl::Status::Cancelled);
			CHECK(device.commands == 0);
		} else if (phase == 1) {
			// Authorization is evidence even before send acceptance is reported.
			h.now = 21;
			h.client.process(h.now);
			CHECK(active.calls == 1 && active.status == cl::Status::OutcomeUnknown);
		} else {
			CHECK(h.client.onSendAccepted(packet.session, packet.requestId));
			const auto reply = en::dispatchEncoded(model.view(), h.workspace, packet.session,
			                                       packet.bytes, h.reply);
			CHECK(reply.status == en::EncodedDispatchStatus::Ok);
			CHECK(h.client.onReply(cl::Input{h.reply}.first(reply.written), packet.session,
			                       packet.requestId));
			CHECK(active.calls == 1 && active.status == cl::Status::Ok &&
			      active.valueStatus == dy::ValueStatus::Stale);
			CHECK(device.commands == 1);
		}
		CHECK(active.calls == 1 && queued.calls == 1);
		h.client.process(100);
		CHECK(active.calls == 1 && queued.calls == 1);
	}
}

static void staleReadAndDeadline() noexcept
{
	for (bool late : {false, true}) {
		Encoded h;
		Observed value;
		CHECK(h.client.submit(
		    h.bind(cl::Operation::FieldRead, 0), {},
		    [&](const auto& r) noexcept {
			    value(r);
		    },
		    20));
		auto p = h.offered();
		CHECK(h.client.prepareHandoff(p.session, p.requestId));
		CHECK(h.client.onSendAccepted(p.session, p.requestId));
		auto reply = en::dispatchEncoded(model.view(), h.workspace, p.session, p.bytes, h.reply);
		CHECK(reply.status == en::EncodedDispatchStatus::Ok);
		CHECK(h.metadata.workspace.reset() == rt::Status::Ok);
		if (late)
			h.now = 20;
		(void)h.client.onReply(cl::Input{h.reply}.first(reply.written), p.session, p.requestId);
		h.client.process(h.now);
		CHECK(value.calls == 1 && value.valueStatus == dy::ValueStatus::Stale && !value.hasValue);
		CHECK(value.status == (late ? cl::Status::Timeout : cl::Status::Ok));
	}
}

static void boundedQueueAndClose() noexcept
{
	Encoded h;
	Observed callbacks;
	const auto binding = h.bind(cl::Operation::Command, 0);
	std::array<cl::RequestHandle, 8> handles{};
	for (auto& handle : handles) {
		auto result = h.client.submit(binding, config(), [&](const auto& r) noexcept {
			callbacks(r);
		});
		CHECK(result);
		handle = result.handle;
	}
	CHECK(h.client.pending() == 8);
	CHECK(h.client
	          .submit(binding, config(),
	                  [](const auto&) noexcept {
	                  })
	          .status == cl::Status::QueueFull);
	CHECK(h.client.cancel(handles[7]));
	h.client.close();
	for (unsigned i = 0; i < 16 && h.client.pending(); ++i)
		h.client.process(h.now);
	CHECK(callbacks.calls == 8 && callbacks.status == cl::Status::Cancelled);
	CHECK(h.client.pending() == 0 && h.client.drained());
	CHECK(device.commands == 0);
	h.client.process(100);
	CHECK(callbacks.calls == 8);
	CHECK(h.client
	          .submit(binding, config(),
	                  [](const auto&) noexcept {
	                  })
	          .status == cl::Status::Closed);
}

static void responseClassification() noexcept
{
	for (bool malformed : {false, true}) {
		Encoded h;
		Observed result;
		const auto binding = h.bind(cl::Operation::FieldRead, 0);
		CHECK(h.client.submit(binding, {}, [&](const auto& r) noexcept {
			result(r);
		}));
		auto p = h.offered();
		CHECK(h.client.prepareHandoff(p.session, p.requestId));
		CHECK(h.client.onSendAccepted(p.session, p.requestId));
		auto reply = en::dispatchEncoded(modelView, h.workspace, p.session, p.bytes, h.reply);
		CHECK(reply.status == en::EncodedDispatchStatus::Ok);
		if (malformed)
			h.reply[4] = std::byte{2};
		else
			h.reply[36] = std::byte{2};
		(void)h.client.onReply(cl::Input{h.reply}.first(reply.written), p.session, p.requestId);
		h.client.process(h.now);
		CHECK(result.calls == 1 && !result.hasValue);
		CHECK(result.status == (malformed ? cl::Status::MalformedReply : cl::Status::Ok));
		CHECK(result.valueStatus ==
		      (malformed ? dy::ValueStatus::NoValue : dy::ValueStatus::InvalidPayload));
		if (malformed) {
			CHECK(h.client.recoveryRequired());
			CHECK(h.client.isCurrent(binding));
			CHECK(h.client
			          .submit(binding, {},
			                  [](const auto&) noexcept {
			                  })
			          .status == cl::Status::RecoveryRequired);
		}
	}
}

static void realRetries() noexcept
{
	for (bool lostReply : {false, true}) {
		Encoded<en::RetryingEncodedBackend> h;
		Observed result;
		CHECK(h.client.submit(
		    h.bind(cl::Operation::Command, 0), config(),
		    [&](const auto& r) noexcept {
			    result(r);
		    },
		    500));
		auto first = h.offered();
		if (lostReply)
			h.send(first, false);
		else {
			CHECK(h.client.prepareHandoff(first.session, first.requestId));
			CHECK(h.client.onSendAccepted(first.session, first.requestId));
		}
		CHECK(result.calls == 0);
		h.now = 100;
		auto retry = h.offered();
		CHECK(retry.session == first.session && retry.requestId == first.requestId);
		h.send(retry);
		CHECK(result.calls == 1 && result.executed);
		CHECK(device.commands == (lostReply ? 2u : 1u));
	}
}

static void retryInvalidation() noexcept
{
	Encoded<en::RetryingEncodedBackend> h;
	Observed result;
	CHECK(h.client.submit(
	    h.bind(cl::Operation::Command, 0), config(),
	    [&](const auto& r) noexcept {
		    result(r);
	    },
	    500));
	const auto first = h.offered();
	h.send(first, false);
	CHECK(device.commands == 1 && result.calls == 0);
	h.now = 100;
	auto retry = h.offered();
	CHECK(retry.requestId == first.requestId && retry.session == first.session);
	CHECK(h.metadata.workspace.reset() == rt::Status::Ok);
	CHECK(!h.client.prepareHandoff(retry.session, retry.requestId));
	CHECK(!h.client.nextPacket());
	CHECK(device.commands == 1);
	h.now = 500;
	h.client.process(h.now);
	CHECK(result.calls == 1 && result.status == cl::Status::OutcomeUnknown);
	CHECK(h.client.recoveryRequired());
	CHECK(h.client
	          .submit({}, {},
	                  [](const auto&) noexcept {
	                  })
	          .status == cl::Status::RecoveryRequired);
}

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
	std::size_t lastReplySize = 0;
	cl::rpc::RpcClient native{0, request, result, packet, {3, 40, 1, 1}};
	telemetry::integration::rpc::RpcBackend backend{native, clock};
	dy::DynamicClient<8, 4096> client{backend, clock};

	RpcHarness() noexcept
	{
		device = {};
		CHECK(pool.valid(session));
		CHECK(client.admit(metadata.schema, cl::Input{bytes}.first(64), 1) == cl::Status::Ok);
	}

	auto bind(cl::Operation op, std::uint32_t id) noexcept
	{
		auto b = client.bind(op, id);
		CHECK(b);
		return b.endpoint;
	}

	bool exchange() noexcept
	{
		auto p = client.nextPacket();
		if (!p) {
			(void)pool.process(++now);
			client.process(now);
			return false;
		}
		CHECK(p.bytes.size() <= port.size());
		std::copy(p.bytes.begin(), p.bytes.end(), port.begin());
		auto wire = cl::Input{port}.first(p.bytes.size());
		CHECK(client.prepareHandoff(p.session, p.requestId));
		CHECK(client.onSendAccepted(p.session, p.requestId));
		auto r = pool.handle(session, wire, reply);
		lastReplySize = r.reply.written;
		CHECK(r.status == rpc::RpcSessionStatus::Ok && r.reply.status == resource::Status::Ok);
		if (r.ticket.serial)
			CHECK(pool.replyAccepted(session, r.ticket) == rpc::RpcSessionStatus::Ok);
		CHECK(client.onReply(cl::Input{reply}.first(r.reply.written), p.session, p.requestId));
		(void)pool.process(now);
		++now;
		return true;
	}

	void pump() noexcept
	{
		for (unsigned i = 0; i < 200 && client.pending(); ++i)
			(void)exchange();
		CHECK(client.pending() == 0);
	}
};

static void rpcOperationsAndPartialCleanup() noexcept
{
	{
		RpcHarness h;
		Observed read, write, command, service;
		auto data = config(81, true);
		CHECK(h.client.submit(h.bind(cl::Operation::FieldWrite, 0), data,
		                      [&](const auto& r) noexcept {
			                      write(r);
		                      }));
		CHECK(h.client.submit(h.bind(cl::Operation::FieldRead, 0), {}, [&](const auto& r) noexcept {
			read(r);
		}));
		CHECK(h.client.submit(h.bind(cl::Operation::Command, 0), data, [&](const auto& r) noexcept {
			command(r);
		}));
		CHECK(h.client.submit(h.bind(cl::Operation::Service, 0), data, [&](const auto& r) noexcept {
			service(r);
		}));
		h.pump();
		CHECK(write.calls == 1 && read.calls == 1 && command.calls == 1 && service.calls == 1);
		CHECK(read.hasValue && read.number == 81);
		CHECK(service.hasValue && service.number == 81);
	}
	RpcHarness h;
	Observed old, fresh;
	std::array<std::byte, 128> big{};
	CHECK(h.client.submit(
	    h.bind(cl::Operation::Command, 3), big,
	    [&](const auto& r) noexcept {
		    old(r);
	    },
	    500));
	const auto offered = h.client.nextPacket();
	CHECK(offered && !offered.mayExecute);
	CHECK(h.exchange());
	CHECK(h.mailbox.state() == telemetry::protocol::rpc::RpcState::Receiving);
	CHECK(device.commands == 0);
	const auto oldReply = h.reply;
	const auto oldReplySize = h.lastReplySize;
	const auto oldId = offered.requestId;
	CHECK(h.metadata.workspace.reset() == rt::Status::Ok);
	CHECK(!h.client.nextPacket());
	h.client.process(h.now);
	CHECK(old.calls == 1 && old.status == cl::Status::Cancelled);
	CHECK(device.commands == 0);
	CHECK(h.mailbox.state() == telemetry::protocol::rpc::RpcState::Receiving);
	CHECK(!h.client.confirmQuiescence({1, false, true, true}));
	CHECK(h.pool.release(h.session) == rpc::RpcSessionStatus::Ok);
	(void)h.pool.process(h.now);
	CHECK(h.mailbox.state() == telemetry::protocol::rpc::RpcState::Idle);
	CHECK(h.client.confirmQuiescence({1, true, true, true}));
	auto parsed = rt::RuntimeSchema::parse(bytes, h.metadata.workspace);
	CHECK(parsed);
	h.metadata.schema = parsed.schema;
	h.session = h.pool.acquire({7, 2, 0}).session;
	CHECK(h.pool.valid(h.session));
	CHECK(h.client.admit(h.metadata.schema, cl::Input{bytes}.first(64), 2) == cl::Status::Ok);
	CHECK(h.client.submit(h.bind(cl::Operation::Command, 2), {}, [&](const auto& r) noexcept {
		fresh(r);
	}));
	CHECK(!h.client.onReply(cl::Input{oldReply}.first(oldReplySize), 1, oldId));
	CHECK(fresh.calls == 0);
	h.pump();
	CHECK(fresh.calls == 1 && fresh.executed && device.commands == 1 && old.calls == 1);
}

static cl::Packet rpcPacket(RpcHarness& h) noexcept
{
	for (unsigned i = 0; i < 20; ++i) {
		auto packet = h.client.nextPacket();
		if (packet)
			return packet;
		++h.now;
		(void)h.pool.process(h.now);
		h.client.process(h.now);
	}
	CHECK(false);
	return {};
}

static void rpcFinalAuthorization() noexcept
{
	RpcHarness h;
	Observed result;
	std::array<std::byte, 128> large{};
	CHECK(h.client.submit(
	    h.bind(cl::Operation::Command, 3), large,
	    [&](const auto& r) noexcept {
		    result(r);
	    },
	    100));
	cl::Packet final{};
	for (unsigned i = 0; i < 30; ++i) {
		auto packet = rpcPacket(h);
		if (packet.mayExecute) {
			final = packet;
			break;
		}
		CHECK(h.exchange());
	}
	CHECK(final && final.mayExecute);
	CHECK(device.commands == 0);
	CHECK(h.client.prepareHandoff(final.session, final.requestId));
	// No onSendAccepted: authorization itself conservatively permits execution.
	CHECK(h.metadata.workspace.reset() == rt::Status::Ok);
	CHECK(!h.client.nextPacket());
	h.now = 100;
	h.client.process(h.now);
	CHECK(result.calls == 1 && result.status == cl::Status::OutcomeUnknown &&
	      result.valueStatus == dy::ValueStatus::Stale);
	CHECK(h.client.recoveryRequired() && device.commands == 0);
}

static void rpcPassiveResultOrBlockedRead() noexcept
{
	for (bool authorizeRead : {false, true}) {
		RpcHarness h;
		Observed result;
		CHECK(h.client.submit(
		    h.bind(cl::Operation::Command, 2), {},
		    [&](const auto& r) noexcept {
			    result(r);
		    },
		    100));
		// Final WRITE ACK is only receipt: the callback result still needs READ.
		CHECK(h.exchange());
		CHECK(device.commands == 1 && result.calls == 0);
		auto read = rpcPacket(h);
		CHECK(!read.mayExecute);
		CHECK(read.bytes[0] == std::byte(resource::protocol::Op::Read));
		std::copy(read.bytes.begin(), read.bytes.end(), h.port.begin());
		const auto size = read.bytes.size();
		if (authorizeRead) {
			CHECK(h.client.prepareHandoff(read.session, read.requestId));
			CHECK(h.client.onSendAccepted(read.session, read.requestId));
		}
		CHECK(h.metadata.workspace.reset() == rt::Status::Ok);
		if (authorizeRead) {
			const auto reply = h.pool.handle(h.session, cl::Input{h.port}.first(size), h.reply);
			CHECK(reply.status == rpc::RpcSessionStatus::Ok &&
			      reply.reply.status == resource::Status::Ok);
			CHECK(h.client.onReply(cl::Input{h.reply}.first(reply.reply.written), read.session,
			                       read.requestId));
			CHECK(result.calls == 1 && result.status == cl::Status::Ok && result.executed &&
			      result.valueStatus == dy::ValueStatus::Stale);
		} else {
			CHECK(!h.client.prepareHandoff(read.session, read.requestId));
			CHECK(!h.client.nextPacket());
			CHECK(result.calls == 0);
			h.now = 100;
			h.client.process(h.now);
			CHECK(result.calls == 1 && result.status == cl::Status::OutcomeUnknown &&
			      result.valueStatus == dy::ValueStatus::Stale);
			CHECK(h.client.recoveryRequired());
		}
		CHECK(device.commands == 1);
		h.client.process(1000);
		CHECK(result.calls == 1);
	}
}

int main()
{
	validReplacementSameIds();
	ordinary();
	ownedRequestSnapshot();
	requestMutationDuringOwnership();
	refusals();
	beforeAndAfterAuthorization();
	staleReadAndDeadline();
	boundedQueueAndClose();
	responseClassification();
	realRetries();
	retryInvalidation();
	rpcOperationsAndPartialCleanup();
	rpcFinalAuthorization();
	rpcPassiveResultOrBlockedRead();
	std::printf("dynamic client: %u checks, 0 failures\n", checks);
	return 0;
}
