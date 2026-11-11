/* ReadMany runtime integration and deterministic lifecycle checks. MIT. */
#include <integration/read_many/Client.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/RetryingBackend.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <integration/rpc/Backend.hpp>
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include "../../examples/read_comparison/DataModel.hpp"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>

namespace ts = telemetry;
namespace cl = ts::client;
namespace dy = ts::integration::dynamic;
namespace rm = ts::integration::read_many;
namespace en = ts::integration::encoded;
namespace ew = ts::protocol::encoded;
namespace rt = ts::runtime;
static unsigned checks = 0;

static void check(bool condition, unsigned line)
{
	++checks;
	if (!condition) {
		std::fprintf(stderr, "ReadMany client check=%u line=%u failed\n", checks, line);
		std::abort();
	}
}

#define CHECK(...) check(bool(__VA_ARGS__), __LINE__)

enum class Mode : std::int16_t {
	Reverse = -7,
	Forward = 12
};

struct Record {
	std::uint32_t value;
};

struct Mixed {
	unsigned calls = 0;

	bool enabled() noexcept
	{
		++calls;
		return true;
	}

	std::uint64_t wide() noexcept
	{
		++calls;
		return UINT64_MAX;
	}

	std::int64_t signedWide() noexcept
	{
		++calls;
		return INT64_MIN;
	}

	float real() noexcept
	{
		++calls;
		return std::bit_cast<float>(0xff812345u);
	}

	double real64() noexcept
	{
		++calls;
		return std::bit_cast<double>(UINT64_C(0x8000000000000000));
	}

	Mode mode() noexcept
	{
		++calls;
		return static_cast<Mode>(-123);
	}

	Record record() noexcept
	{
		++calls;
		return {17};
	}
};

static Mixed mixed;
static ts::OwnerSlot<Mixed> absent;
static constexpr ts::FieldTable mixedFields{
    ts::field<&Mixed::enabled>("Bool", mixed),   ts::field<&Mixed::wide>("U64", mixed),
    ts::field<&Mixed::signedWide>("S64", mixed), ts::field<&Mixed::real>("F32", mixed),
    ts::field<&Mixed::real64>("F64", mixed),     ts::field<&Mixed::mode>("Enum", mixed),
    ts::field<&Mixed::record>("Struct", mixed),  ts::field<&Mixed::wide>("Unavailable", absent)};
static constexpr ts::FieldCatalogTable mixedCatalogs{ts::group("mixed", mixedFields)};
static constexpr ts::ServiceCatalogTable<> noServices{};
static constexpr ts::Model mixedModel{mixedCatalogs, ts::emptyCommands, noServices};
static constexpr ts::descriptor::Descriptor mixedDescriptor{mixedModel};
static constexpr auto mixedBytes = ts::descriptor::packDescriptor<mixedDescriptor>();
using Data = read_comparison::Sparse8of32;

struct Metadata {
	std::array<rt::RuntimeType, 32> types{};
	std::array<rt::RuntimeMember, 16> members{};
	std::array<rt::RuntimeEnumEntry, 16> enums{};
	std::array<rt::RuntimeCatalog, 8> catalogs{};
	std::array<rt::RuntimeEndpoint, 40> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	rt::RuntimeSchema schema;

	explicit Metadata(rt::Input bytes) noexcept
	{
		const auto parsed = rt::RuntimeSchema::parse(bytes, workspace);
		CHECK(parsed);
		schema = parsed.schema;
	}
};

template<const auto& Model = Data::scalarModel, const auto& Bytes = Data::scalarBytes>
struct Harness {
	Metadata metadata{Bytes};
	std::uint32_t now = 0;
	cl::Clock clock{this, +[](void* p) noexcept {
		                return static_cast<Harness*>(p)->now;
	                }};
	std::array<std::byte, 1024> tx{}, rx{}, port{}, reply{}, scratch{};
	ts::Workspace workspace{scratch};
	en::EncodedBackend backend{tx, rx, clock, 30000, true};
	dy::DynamicClient<4, 1024, 640> client{backend, clock};
	rm::wire::Capability capability{17, metadata.schema.fingerprint()};

	Harness() noexcept
	{
		CHECK(client.admit(metadata.schema, cl::Input{Bytes}.first(64), 17) == cl::Status::Ok);
	}

	auto bind(std::span<const std::uint32_t> ids) noexcept
	{
		auto binding = rm::bindReadMany(client, backend, capability, ids);
		CHECK(binding);
		return binding.binding;
	}

	cl::Packet offered() noexcept
	{
		auto packet = client.nextPacket();
		CHECK(packet && packet.mayExecute);
		std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
		packet.bytes = cl::Input{port}.first(packet.bytes.size());
		return packet;
	}

	std::size_t response(cl::Packet packet) noexcept
	{
		const auto result = en::dispatchEncoded(Model.view(), workspace, packet.session,
		                                        packet.bytes, reply, capability);
		CHECK(result);
		return result.written;
	}

	void send(cl::Packet packet) noexcept
	{
		CHECK(client.prepareHandoff(packet.session, packet.requestId));
		CHECK(client.onSendAccepted(packet.session, packet.requestId));
		const auto bytes = response(packet);
		CHECK(client.onReply(cl::Input{reply}.first(bytes), packet.session, packet.requestId));
	}
};

struct Observed {
	unsigned calls = 0;
	cl::Status status = cl::Status::Busy;
	rm::ValueStatus valueStatus = rm::ValueStatus::NoValue;
	std::uint16_t count = 0;

	void operator()(const rm::ReadManyResult& result) noexcept
	{
		++calls;
		status = result.operation.status;
		valueStatus = result.valueStatus;
		count = result.count;
		if (valueStatus != rm::ValueStatus::Ok)
			for (const auto& item : result.items)
				CHECK(!item.hasValue());
	}
};

static void valuesAndCopiedBindings() noexcept
{
	Harness h;
	Data::owner.reset();
	std::array<std::uint32_t, 32> ids{};
	for (std::size_t i = 0; i < ids.size(); ++i)
		ids[i] = static_cast<std::uint32_t>(31 - i);
	auto binding = h.bind(ids);
	CHECK(binding.entries().size() == 32 && binding.responseBytes() == 386);
	ids.fill(UINT32_MAX);
	unsigned completions = 0;
	CHECK(rm::readMany(h.client, binding, [&](const rm::ReadManyResult& result) noexcept {
		++completions;
		CHECK(result.operation.succeeded() && !result.operation.executed() &&
		      !result.operation.accepted() && result.valueStatus == rm::ValueStatus::Ok);
		CHECK(result.count == 32);
		for (std::size_t i = 0; i < result.count; ++i) {
			CHECK(result.items[i].id == 31 - i && result.items[i].hasValue());
			const auto value = result.items[i].value.get<std::uint32_t>();
			CHECK(value && value.value == read_comparison::data[31 - i]);
		}
	}));
	CHECK(h.client.pending() == 1 && Data::owner.calls() == 0);
	binding = {};
	const auto packet = h.offered();
	const auto parsed = ew::parse(packet.bytes);
	CHECK(parsed && parsed.header.operation == ew::Operation::ReadMany);
	const auto request = rm::wire::parseRequest(parsed.payload);
	CHECK(request && request.count == 32 && request.ids[0] == 31 && request.ids[31] == 0);
	h.send(packet);
	CHECK(completions == 1 && Data::owner.calls() == 32 && h.client.pending() == 0);
	h.client.process(h.now);
	CHECK(completions == 1);
}

static void mixedValues() noexcept
{
	Harness<mixedModel, mixedBytes> h;
	const std::array<std::uint32_t, 7> ids{0, 1, 2, 3, 4, 5, 7};
	const auto binding = h.bind(ids);
	unsigned calls = 0;
	CHECK(rm::readMany(h.client, binding, [&](const rm::ReadManyResult& result) noexcept {
		++calls;
		CHECK(result.operation.succeeded() && result.valueStatus == rm::ValueStatus::Ok &&
		      result.count == ids.size());
		CHECK(result.items[0].value.get<bool>().value);
		CHECK(result.items[1].value.get<std::uint64_t>().value == UINT64_MAX);
		CHECK(result.items[2].value.get<std::int64_t>().value == INT64_MIN);
		CHECK(std::bit_cast<std::uint32_t>(result.items[3].value.get<float>().value) ==
		      0xff812345u);
		CHECK(std::bit_cast<std::uint64_t>(result.items[4].value.get<double>().value) ==
		      UINT64_C(0x8000000000000000));
		const auto number = result.items[5].value.enumUnderlying<std::int16_t>();
		CHECK(number && number.value == -123);
		CHECK(!result.items[6].hasValue() &&
		      result.items[6].status == rm::wire::ItemStatus::Unavailable);
	}));
	h.send(h.offered());
	CHECK(calls == 1);
}

static void refusalAndCompatibility() noexcept
{
	static_assert(cl::isReadOnly(cl::Operation::ReadMany));
	static_assert(cl::isReadOnly(cl::Operation::FieldRead));
	static_assert(!cl::isReadOnly(cl::Operation::Command));
	static_assert(!cl::isReadOnly(cl::Operation::Batch));
	const cl::ResultView success{cl::Status::Ok, cl::Operation::ReadMany};
	CHECK(success.succeeded() && !success.accepted() && !success.executed());
	Harness h;
	const std::array<std::uint32_t, 2> ids{0, 1};
	const std::array<std::uint32_t, 2> duplicate{0, 0};
	CHECK(rm::bindReadMany(h.client, h.backend, h.capability, {}).status ==
	      cl::Status::InvalidArgument);
	CHECK(rm::bindReadMany(h.client, h.backend, h.capability, duplicate).status ==
	      cl::Status::InvalidArgument);
	const std::array<std::uint32_t, 1> absentId{999};
	CHECK(rm::bindReadMany(h.client, h.backend, h.capability, absentId).status ==
	      cl::Status::InvalidArgument);
	auto cap = h.capability;
	++cap.session;
	CHECK(rm::bindReadMany(h.client, h.backend, cap, ids).status == cl::Status::NotAdmitted);
	cap = h.capability;
	++cap.fingerprint;
	CHECK(rm::bindReadMany(h.client, h.backend, cap, ids).status == cl::Status::NotAdmitted);
	cap = h.capability;
	cap.itemLimit = 1;
	CHECK(rm::bindReadMany(h.client, h.backend, cap, ids).status == cl::Status::InvalidArgument);
	cap = h.capability;
	cap.requestLimit = 6;
	CHECK(rm::bindReadMany(h.client, h.backend, cap, ids).status == cl::Status::BufferTooSmall);
	cap = h.capability;
	cap.replyLimit = 10;
	CHECK(rm::bindReadMany(h.client, h.backend, cap, ids).status == cl::Status::BufferTooSmall);
	cap.itemLimit = 0;
	CHECK(rm::bindReadMany(h.client, h.backend, cap, ids).status == cl::Status::InvalidArgument);
	cl::Backend& erased = h.backend;
	CHECK(rm::bindReadMany(h.client, erased, h.capability, ids).status == cl::Status::Unsupported);
	std::array<std::byte, 1024> tx{}, rx{};
	en::EncodedBackend disabled{tx, rx, h.clock};
	CHECK(!disabled.supportsReadMany());
	CHECK(rm::bindReadMany(h.client, disabled, h.capability, ids).status ==
	      cl::Status::Unsupported);
	CHECK(h.client.pending() == 0 && !h.backend.nextPacket());
	CHECK(h.client.bind(cl::Operation::ReadMany, 0).status == cl::Status::InvalidArgument);
	CHECK(h.client.bind(static_cast<cl::Operation>(6), 0).status == cl::Status::InvalidArgument);
	CHECK(h.backend.start({cl::Operation::Batch, 0, {}, 14}, h.now) == cl::Status::Unsupported);
	CHECK(h.backend.start({cl::Operation::Resource, 0, {}, 10}, h.now) == cl::Status::Unsupported);
	CHECK(h.backend.start({static_cast<cl::Operation>(8), 0, {}, 10}, h.now) ==
	      cl::Status::InvalidArgument);
	Harness<mixedModel, mixedBytes> mixedHarness;
	const std::array<std::uint32_t, 1> structId{6};
	CHECK(rm::bindReadMany(mixedHarness.client, mixedHarness.backend, mixedHarness.capability,
	                       structId)
	          .status == cl::Status::Unsupported);
	// The ordinary Field API still uses the same queue and its exact DTO reply.
	const auto field = h.client.bind(cl::Operation::FieldRead, 0);
	CHECK(field);
	unsigned calls = 0;
	CHECK(h.client.readField(field.endpoint, [&](const dy::DynamicResult& result) noexcept {
		++calls;
		CHECK(result.operation.succeeded() && result.hasValue());
	}));
	h.send(h.offered());
	CHECK(calls == 1);
}

static void nullCallbacks() noexcept
{
	Harness h;
	Data::owner.reset();
	const std::array<std::uint32_t, 2> ids{0, 1};
	const auto binding = h.bind(ids);
	void (*callback)(const rm::ReadManyResult&) noexcept = nullptr;
	CHECK(rm::readMany(h.client, binding, callback).status == cl::Status::InvalidArgument);
	CHECK(h.client.pending() == 0 && !h.backend.nextPacket() && Data::owner.calls() == 0);

	struct FalseCallable {
		unsigned* calls;

		explicit operator bool() const noexcept
		{
			return false;
		}

		void operator()(const rm::ReadManyResult& result) noexcept
		{
			CHECK(result.operation.succeeded() && result.valueStatus == rm::ValueStatus::Ok);
			++*calls;
		}
	};

	unsigned calls = 0;
	CHECK(rm::readMany(h.client, binding, FalseCallable{&calls}));
	h.send(h.offered());
	CHECK(calls == 1 && h.client.pending() == 0 && Data::owner.calls() == 2);
	unsigned timeouts = 0;
	CHECK(rm::readMany(
	    h.client, binding,
	    [&](const auto& result) noexcept {
		    ++timeouts;
		    CHECK(result.operation.status == cl::Status::Timeout);
	    },
	    5));
	const auto packet = h.offered();
	CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
	CHECK(h.client.onSendAccepted(packet.session, packet.requestId));
	h.now = 5;
	h.client.process(h.now);
	CHECK(timeouts == 1 && h.client.recoveryRequired() && h.client.pending() == 0);
	CHECK(rm::readMany(h.client, binding, callback).status == cl::Status::RecoveryRequired);
	CHECK(h.client.pending() == 0 && Data::owner.calls() == 2);
	h.client.invalidate();
	CHECK(h.client.confirmQuiescence({17, true, true, true}));
	CHECK(rm::readMany(h.client, binding, callback).status == cl::Status::NotAdmitted);
	h.client.close();
	CHECK(rm::readMany(h.client, binding, callback).status == cl::Status::Closed);
	h.client.process(h.now);
	CHECK(h.client.drained());
}

static unsigned functionCalls = 0;

static void functionCompletion(const rm::ReadManyResult& result) noexcept
{
	CHECK(result.operation.succeeded() && result.valueStatus == rm::ValueStatus::Ok &&
	      result.count == 2);
	++functionCalls;
}

static void functionReferences() noexcept
{
	Harness h;
	const std::array<std::uint32_t, 2> ids{0, 1};
	const auto binding = h.bind(ids);
	functionCalls = 0;
	CHECK(rm::readMany(h.client, binding, functionCompletion));
	h.send(h.offered());
	CHECK(functionCalls == 1 && h.client.pending() == 0);
}

static void lifecycle() noexcept
{
	const std::array<std::uint32_t, 2> ids{0, 1};
	for (unsigned phase = 0; phase < 3; ++phase) {
		Harness h;
		Observed observed;
		const auto binding = h.bind(ids);
		CHECK(rm::readMany(
		    h.client, binding,
		    [&](const auto& result) noexcept {
			    observed(result);
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
		CHECK(!h.client.nextPacket() && h.client.pending() == 0);
		if (phase)
			CHECK(!h.client.prepareHandoff(packet.session, packet.requestId));
		h.client.process(h.now);
		CHECK(observed.calls == 1);
	}
	for (unsigned phase = 0; phase < 3; ++phase) {
		Harness h;
		Observed observed;
		const auto binding = h.bind(ids);
		CHECK(rm::readMany(
		    h.client, binding,
		    [&](const auto& result) noexcept {
			    observed(result);
		    },
		    50));
		cl::Packet packet;
		if (phase)
			packet = h.offered();
		if (phase == 2)
			CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
		h.client.invalidate();
		CHECK(!h.client.nextPacket());
		if (phase)
			CHECK(!h.client.prepareHandoff(packet.session, packet.requestId));
		if (phase == 2) {
			CHECK(observed.calls == 0);
			const auto size = h.response(packet);
			CHECK(
			    h.client.onReply(cl::Input{h.reply}.first(size), packet.session, packet.requestId));
			CHECK(observed.status == cl::Status::Ok &&
			      observed.valueStatus == rm::ValueStatus::Stale);
		} else {
			h.client.process(h.now);
			CHECK(observed.status == cl::Status::Cancelled &&
			      observed.valueStatus == rm::ValueStatus::Stale);
		}
		CHECK(observed.calls == 1);
		h.client.process(h.now);
		CHECK(observed.calls == 1 && h.client.pending() == 0);
		CHECK(h.client.confirmQuiescence({17, true, true, true}));
		CHECK(h.client.admit(h.metadata.schema, cl::Input{Data::scalarBytes}.first(64), 18) ==
		      cl::Status::Ok);
		CHECK(rm::readMany(h.client, binding, [&](const auto& result) noexcept {
			      observed(result);
		      }).status == cl::Status::NotAdmitted);
	}
	// Deadline wins even if valid bytes are already available in a port buffer.
	Harness h;
	Observed observed;
	const auto binding = h.bind(ids);
	CHECK(rm::readMany(
	    h.client, binding,
	    [&](const auto& result) noexcept {
		    observed(result);
	    },
	    50));
	const auto packet = h.offered();
	CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
	const auto size = h.response(packet);
	h.now = 50;
	CHECK(!h.client.onReply(cl::Input{h.reply}.first(size), packet.session, packet.requestId));
	CHECK(observed.calls == 1 && observed.status == cl::Status::Timeout &&
	      h.client.recoveryRequired());
	CHECK(rm::readMany(h.client, binding, [&](const auto& result) noexcept {
		      observed(result);
	      }).status == cl::Status::RecoveryRequired);
	h.client.close();
	CHECK(rm::readMany(h.client, binding, [&](const auto& result) noexcept {
		      observed(result);
	      }).status == cl::Status::Closed);
}

static void malformedBodies() noexcept
{
	const std::array<std::uint32_t, 2> ids{1, 0};
	for (unsigned mutation = 0; mutation < 7; ++mutation) {
		Harness<mixedModel, mixedBytes> h;
		Observed observed;
		const auto binding = h.bind(ids);
		CHECK(rm::readMany(h.client, binding, [&](const auto& result) noexcept {
			observed(result);
		}));
		const auto packet = h.offered();
		CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
		auto size = h.response(packet);
		auto body = cl::Output{h.reply}.subspan(ew::headerBytes, size - ew::headerBytes);
		switch (mutation) {
			case 0:
				body[0] = std::byte{1};
				break; // Wrong count.
			case 1:
				body[2] = std::byte{0};
				break; // Wrong order/ID.
			case 2:
				body[7] = std::byte{1};
				break; // Reserved.
			case 3:
				body[6] = std::byte{5};
				break; // Unknown status.
			case 4:
				body[8] = std::byte{4};
				break; // Wrong captured U64 length.
			case 5:
				body.back() = std::byte{2};
				break; // Invalid bool after valid first item.
			case 6:
				--size;
				ew::detail::store32(h.reply, 20, static_cast<std::uint32_t>(body.size() - 1));
				break;
		}
		CHECK(h.client.onReply(cl::Input{h.reply}.first(size), packet.session, packet.requestId));
		CHECK(observed.calls == 1 && observed.status == cl::Status::Ok &&
		      observed.valueStatus == rm::ValueStatus::InvalidPayload && observed.count == 0);
	}
}

static void rpcAndRetry() noexcept
{
	Harness h;
	std::array<std::byte, 1024> request{}, result{}, packet{};
	cl::rpc::RpcClient native{0, request, result, packet};
	ts::integration::rpc::RpcBackend backend{native, h.clock};
	dy::DynamicClient<4, 1024, 640> dynamic{backend, h.clock};
	CHECK(dynamic.admit(h.metadata.schema, cl::Input{Data::scalarBytes}.first(64), 17) ==
	      cl::Status::Ok);
	const std::array<std::uint32_t, 1> ids{0};
	CHECK(rm::bindReadMany(dynamic, backend, h.capability, ids).status == cl::Status::Unsupported);
	CHECK(dynamic.pending() == 0 && !native.nextPacket() && !backend.nextPacket());
	CHECK(backend.start({cl::Operation::ReadMany, 0, {}, 14}, 0) == cl::Status::Unsupported);
	CHECK(!native.active() && !native.nextPacket());
	en::RetryingEncodedBackend retry{request, result, h.clock, {10, 1}, true};
	CHECK(retry.supportsReadMany() && retry.openSession(17));
	std::array<std::byte, 6> payload{};
	CHECK(rm::wire::writeRequest(payload, ids));
	CHECK(retry.start({cl::Operation::ReadMany, 0, payload, 14, 100}, 0) == cl::Status::Ok);
	const auto first = retry.nextPacket();
	CHECK(first && first.mayExecute && retry.prepareHandoff(first.session, first.requestId));
	CHECK(retry.onSendAccepted(first.session, first.requestId));
	h.now = 10;
	retry.process(h.now);
	const auto second = retry.nextPacket();
	CHECK(second && second.session == first.session && second.requestId == first.requestId &&
	      std::equal(first.bytes.begin(), first.bytes.end(), second.bytes.begin(),
	                 second.bytes.end()));
	CHECK(retry.prepareHandoff(second.session, second.requestId));
	CHECK(retry.onSendAccepted(second.session, second.requestId));
	h.now = 20;
	retry.process(h.now);
	CHECK(retry.completed() && retry.result().status == cl::Status::Timeout &&
	      retry.recoveryRequired());
}

static void fifoCancellationAndReentrancy() noexcept
{
	const std::array<std::uint32_t, 2> ids{0, 1};
	Harness h;
	const auto binding = h.bind(ids);
	unsigned calls = 0;
	std::array<cl::Submission, 4> submissions{};
	for (auto& submitted : submissions)
		submitted = rm::readMany(h.client, binding, [&](const auto& value) noexcept {
			++calls;
			CHECK(value.operation.succeeded() && value.valueStatus == rm::ValueStatus::Ok);
		});
	CHECK(h.client.pending() == 4 && calls == 0);
	for (auto submitted : submissions)
		CHECK(submitted);
	CHECK(rm::readMany(h.client, binding, [&](const auto&) noexcept {
		      ++calls;
	      }).status == cl::Status::QueueFull);
	for (unsigned i = 0; i < 4; ++i)
		h.send(h.offered());
	CHECK(calls == 4 && h.client.pending() == 0);
	for (unsigned phase = 0; phase < 3; ++phase) {
		Harness cancel;
		Observed observed;
		const auto captured = cancel.bind(ids);
		const auto submitted =
		    rm::readMany(cancel.client, captured, [&](const auto& value) noexcept {
			    observed(value);
		    });
		CHECK(submitted);
		cl::Packet packet;
		if (phase)
			packet = cancel.offered();
		if (phase == 2)
			CHECK(cancel.client.prepareHandoff(packet.session, packet.requestId));
		CHECK(cancel.client.cancel(submitted.handle));
		cancel.client.process(cancel.now);
		CHECK(observed.calls == 1 && observed.status == cl::Status::Cancelled);
		CHECK(cancel.client.recoveryRequired() == (phase == 2));
		CHECK(!cancel.client.nextPacket());
		cancel.client.process(cancel.now);
		CHECK(observed.calls == 1);
	}
	unsigned reentered = 0;
	CHECK(rm::readMany(h.client, binding, [&](const auto& value) noexcept {
		CHECK(value.operation.succeeded());
		++reentered;
		CHECK(!h.client.confirmQuiescence({17, true, true, true}));
		CHECK(rm::readMany(h.client, binding, [&](const auto& second) noexcept {
			CHECK(second.operation.succeeded());
			++reentered;
		}));
	}));
	h.send(h.offered());
	CHECK(reentered == 1 && h.client.pending() == 1);
	h.send(h.offered());
	CHECK(reentered == 2 && h.client.pending() == 0);
}

static void identityAndBackendFailures() noexcept
{
	const std::array<std::uint32_t, 2> ids{0, 1};
	Harness h;
	Observed observed;
	const auto binding = h.bind(ids);
	Harness other;
	CHECK(rm::readMany(other.client, binding, [&](const auto& result) noexcept {
		      observed(result);
	      }).status == cl::Status::NotAdmitted);
	CHECK(rm::bindReadMany(h.client, other.backend, h.capability, ids).status ==
	      cl::Status::Unsupported);
	CHECK(rm::readMany(h.client, binding, [&](const auto& result) noexcept {
		observed(result);
	}));
	const auto packet = h.offered();
	CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
	const auto size = h.response(packet);
	// Workspace invalidation must be observed before completion touches metadata.
	CHECK(h.metadata.workspace.reset() == rt::Status::Ok);
	CHECK(h.client.onReply(cl::Input{h.reply}.first(size), packet.session, packet.requestId));
	CHECK(observed.calls == 1 && observed.status == cl::Status::Ok &&
	      observed.valueStatus == rm::ValueStatus::Stale && observed.count == 0);
	CHECK(rm::readMany(h.client, binding, [&](const auto& result) noexcept {
		      observed(result);
	      }).status == cl::Status::NotAdmitted);
	for (unsigned mode = 0; mode < 2; ++mode) {
		Harness broken;
		Observed outcome;
		const auto captured = broken.bind(ids);
		CHECK(rm::readMany(broken.client, captured, [&](const auto& value) noexcept {
			outcome(value);
		}));
		const auto offered = broken.offered();
		CHECK(broken.client.prepareHandoff(offered.session, offered.requestId));
		auto length = broken.response(offered);
		if (mode == 0) {
			broken.reply[4] = std::byte{99};
		} else {
			auto header = ew::parse(cl::Input{broken.reply}.first(length)).header;
			header.dispatch = static_cast<std::uint8_t>(ts::DispatchStatus::NotFound);
			header.payloadBytes = 0;
			CHECK(ew::writeHeader(broken.reply, header) == ew::Status::Ok);
			length = ew::headerBytes;
		}
		CHECK(broken.client.onReply(cl::Input{broken.reply}.first(length), offered.session,
		                            offered.requestId));
		CHECK(outcome.calls == 1 && outcome.count == 0);
		if (mode == 0) {
			CHECK(outcome.status == cl::Status::MalformedReply && broken.client.recoveryRequired());
		} else {
			CHECK(outcome.status == cl::Status::Ok &&
			      outcome.valueStatus == rm::ValueStatus::NoValue &&
			      !broken.client.recoveryRequired());
		}
	}
	// A synchronous support gate must never process a recovery state or invoke
	// callbacks to discover which status has precedence over NotAdmitted.
	Harness recovery;
	Observed timeout;
	const auto captured = recovery.bind(ids);
	CHECK(rm::readMany(
	    recovery.client, captured,
	    [&](const auto& value) noexcept {
		    timeout(value);
	    },
	    1));
	const auto offered = recovery.offered();
	CHECK(recovery.client.prepareHandoff(offered.session, offered.requestId));
	recovery.now = 1;
	recovery.backend.process(recovery.now);
	CHECK(recovery.backend.recoveryRequired() && timeout.calls == 0 &&
	      recovery.client.pending() == 1);
	CHECK(rm::bindReadMany(recovery.client, recovery.backend, recovery.capability, ids).status ==
	      cl::Status::RecoveryRequired);
	CHECK(rm::readMany(recovery.client, captured, [&](const auto& value) noexcept {
		      timeout(value);
	      }).status == cl::Status::RecoveryRequired);
	CHECK(timeout.calls == 0 && recovery.client.pending() == 1);
	recovery.client.process(recovery.now);
	CHECK(timeout.calls == 1 && timeout.status == cl::Status::Timeout);
}

struct PublicationProbe {
	Harness<>* harness;
	unsigned moves = 0, attempts = 0, oldCallbacks = 0;
	bool processBeforeInvalidate = false, onDestroy = false;

	void attempt() noexcept
	{
		if (attempts++)
			return;
		if (processBeforeInvalidate)
			harness->client.process(harness->now);
		harness->client.invalidate();
		CHECK(!harness->client.confirmQuiescence({17, true, true, true}));
		CHECK(harness->client.admit(harness->metadata.schema,
		                            cl::Input{Data::scalarBytes}.first(64),
		                            18) == cl::Status::Busy);
	}
};

struct RebindOnOwnership {
	PublicationProbe* probe;

	explicit RebindOnOwnership(PublicationProbe& value) noexcept : probe(&value)
	{}

	RebindOnOwnership(const RebindOnOwnership&) = delete;

	RebindOnOwnership(RebindOnOwnership&& source) noexcept : probe(source.probe)
	{
		++probe->moves;
		if (!probe->onDestroy && probe->attempts == 0)
			probe->attempt();
	}

	~RebindOnOwnership()
	{
		if (probe->onDestroy && probe->attempts == 0)
			probe->attempt();
	}

	void operator()(const rm::ReadManyResult& value) noexcept
	{
		CHECK(value.operation.status == cl::Status::Cancelled);
	}

	void operator()(const dy::DynamicResult& value) noexcept
	{
		CHECK(value.operation.status == cl::Status::Cancelled);
	}
};

static void ownershipPublication() noexcept
{
	const std::array<std::uint32_t, 2> ids{0, 1};
	for (unsigned phase = 0; phase < 3; ++phase) {
		Harness h;
		PublicationProbe probe{&h};
		probe.onDestroy = phase == 1;
		probe.processBeforeInvalidate = phase == 2;
		const auto binding = h.bind(ids);
		if (phase == 2) {
			const auto old = h.client.bind(cl::Operation::FieldRead, 0);
			CHECK(old);
			CHECK(h.client.readField(old.endpoint, [&](const auto& value) noexcept {
				CHECK(value.operation.succeeded());
				++probe.oldCallbacks;
			}));
			const auto offered = h.offered();
			CHECK(h.client.prepareHandoff(offered.session, offered.requestId));
			const auto length = h.response(offered);
			CHECK(h.backend.onReply(cl::Input{h.reply}.first(length), offered.session,
			                        offered.requestId));
			CHECK(h.backend.completed() && probe.oldCallbacks == 0);
		}
		const auto submitted = rm::readMany(h.client, binding, RebindOnOwnership{probe});
		CHECK(probe.moves != 0 && probe.attempts == 1 && h.client.session() == 17);
		CHECK(probe.oldCallbacks == (phase == 2 ? 1u : 0u));
		CHECK(submitted.status == cl::Status::NotAdmitted);
		CHECK(!h.client.nextPacket());
		h.client.process(h.now);
		CHECK(h.client.pending() == 0);
		CHECK(h.client.confirmQuiescence({17, true, true, true}));
		CHECK(h.client.admit(h.metadata.schema, cl::Input{Data::scalarBytes}.first(64), 18) ==
		      cl::Status::Ok);
	}
	// Changing the caller's binding during callable capture cannot alter the
	// frozen request IDs, response budget or completion's captured types.
	Harness h;
	auto binding = h.bind(ids);
	const std::array<std::uint32_t, 1> oneId{3};
	const auto other = h.bind(oneId);

	// Keep this adversarial callable small enough for the same explicit bound.
	// Its replacement binding is borrowed by the callable, not captured again.
	struct SmallReplace {
		rm::ReadManyBinding* destination;
		const rm::ReadManyBinding* replacement;
		unsigned* calls;

		SmallReplace(rm::ReadManyBinding& target, const rm::ReadManyBinding& other,
		             unsigned& count) noexcept
		    : destination(&target), replacement(&other), calls(&count)
		{}

		SmallReplace(const SmallReplace&) = delete;

		SmallReplace(SmallReplace&& source) noexcept
		    : destination(source.destination), replacement(source.replacement), calls(source.calls)
		{
			*destination = *replacement;
		}

		void operator()(const rm::ReadManyResult& value) noexcept
		{
			++*calls;
			CHECK(value.valueStatus == rm::ValueStatus::Ok && value.count == 2 &&
			      value.items[0].id == 0 && value.items[1].id == 1);
		}
	};

	unsigned completed = 0;
	CHECK(rm::readMany(h.client, binding, SmallReplace{binding, other, completed}));
	CHECK(binding.entries().size() == 1);
	const auto packet = h.offered();
	CHECK(ew::parse(packet.bytes).header.expectedResponseBytes == 26);
	h.send(packet);
	CHECK(completed == 1);
}

static void legacyOwnershipPublication() noexcept
{
	for (unsigned phase = 0; phase < 3; ++phase) {
		Harness h;
		PublicationProbe probe{&h};
		probe.onDestroy = phase == 1;
		probe.processBeforeInvalidate = phase == 2;
		const auto field = h.client.bind(cl::Operation::FieldRead, 0);
		CHECK(field);
		if (phase == 2) {
			CHECK(h.client.readField(field.endpoint, [&](const auto& value) noexcept {
				CHECK(value.operation.succeeded());
				++probe.oldCallbacks;
			}));
			const auto offered = h.offered();
			CHECK(h.client.prepareHandoff(offered.session, offered.requestId));
			const auto length = h.response(offered);
			CHECK(h.backend.onReply(cl::Input{h.reply}.first(length), offered.session,
			                        offered.requestId));
		}
		const auto submitted = h.client.readField(field.endpoint, RebindOnOwnership{probe});
		CHECK(probe.moves != 0 && probe.attempts == 1 && h.client.session() == 17);
		CHECK(probe.oldCallbacks == (phase == 2 ? 1u : 0u));
		CHECK(submitted.status == cl::Status::NotAdmitted && !h.client.nextPacket());
		h.client.process(h.now);
		CHECK(h.client.pending() == 0);
		CHECK(h.client.confirmQuiescence({17, true, true, true}));
	}
	Harness h;
	auto endpoint = h.client.bind(cl::Operation::FieldRead, 0).endpoint;
	const auto replacement = h.client.bind(cl::Operation::FieldRead, 1).endpoint;

	struct ReplaceEndpoint {
		dy::EndpointBinding* endpoint;
		const dy::EndpointBinding* replacement;
		unsigned* calls;

		ReplaceEndpoint(dy::EndpointBinding& target, const dy::EndpointBinding& other,
		                unsigned& count) noexcept
		    : endpoint(&target), replacement(&other), calls(&count)
		{}

		ReplaceEndpoint(const ReplaceEndpoint&) = delete;

		ReplaceEndpoint(ReplaceEndpoint&& source) noexcept
		    : endpoint(source.endpoint), replacement(source.replacement), calls(source.calls)
		{
			*endpoint = *replacement;
		}

		void operator()(const dy::DynamicResult& value) noexcept
		{
			++*calls;
			CHECK(value.hasValue() && value.value.get<std::uint32_t>().value == 0);
		}
	};

	unsigned calls = 0;
	CHECK(h.client.readField(endpoint, ReplaceEndpoint{endpoint, replacement, calls}));
	CHECK(endpoint.id() == 1);
	const auto packet = h.offered();
	CHECK(ew::parse(packet.bytes).header.endpoint == 0);
	h.send(packet);
	CHECK(calls == 1);
}

int main()
{
	functionReferences();
#ifdef READ_MANY_CALLBACK_TOO_SMALL
	Harness probe;
	dy::DynamicClient<> small{probe.backend, probe.clock};
	(void)rm::readMany(small, rm::ReadManyBinding{}, [](const auto&) noexcept {
	});
#endif
#ifdef READ_MANY_FORGE
	rm::ReadManyBinding forged;
	forged.client_ = nullptr;
#endif
	valuesAndCopiedBindings();
	mixedValues();
	refusalAndCompatibility();
	nullCallbacks();
	lifecycle();
	malformedBodies();
	rpcAndRetry();
	fifoCancellationAndReentrancy();
	identityAndBackendFailures();
	ownershipPublication();
	legacyOwnershipPublication();
	std::printf("ReadMany sizes: dynamic192=%zu dynamic640=%zu binding=%zu result=%zu "
	            "backend=%zu alignment=%zu\n",
	            sizeof(dy::DynamicClient<4, 130>), sizeof(dy::DynamicClient<4, 130, 640>),
	            sizeof(rm::ReadManyBinding), sizeof(rm::ReadManyResult), sizeof(en::EncodedBackend),
	            alignof(dy::DynamicClient<4, 130, 640>));
	std::printf("ReadMany client: %u checks, 0 failures\n", checks);
	return 0;
}
