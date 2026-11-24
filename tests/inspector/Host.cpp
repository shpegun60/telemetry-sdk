/*
 * @file Host.cpp
 * @brief Public Inspector reads through real Encoded and Resource/RPC models.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include "Fixture.hpp"
#include <bit>
#include <limits>
#include <type_traits>

using namespace inspector_fixture;

namespace alternate {
struct Device {
	unsigned reads = 0;

	std::array<std::uint8_t, 91> read() noexcept
	{
		++reads;
		std::array<std::uint8_t, 91> result{};
		result[0] = 42;
		return result;
	}
};

inline Device device;
inline constexpr telemetry::Model model{
    telemetry::group("inspect", telemetry::field<&Device::read>("Packet", device))};
inline const auto modelView = model.view();
inline constexpr telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto bytes = telemetry::descriptor::packDescriptor<descriptor>();
} // namespace alternate

static void readonlyCounters() noexcept
{
	CHECK(device.writes == 0);
	CHECK(device.commands == 0);
	CHECK(device.services == 0);
}

template<class Model>
static auto checkedRoot(Model& model) noexcept
{
	const auto root = model.root();
	CHECK(root);
	return root.value;
}

template<class Model>
static void decimal(Model& model, auto node, const char* expected) noexcept
{
	std::array<char, 32> text{};
	const auto result = model.decimal(node, text);
	CHECK(result);
	CHECK(result.value == std::strlen(expected));
	CHECK(std::memcmp(text.data(), expected, result.value) == 0);
}

template<class Floating, class Bits, class Model>
static void scalarBits(Model& model, const im::NodeHandle& node, Bits expected) noexcept
{
	static_assert(sizeof(Floating) == sizeof(Bits));
	const auto result = model.scalar(node);
	CHECK(result);
	CHECK(!result.value.enumValue);
	CHECK(result.value.code == (std::is_same_v<Floating, float> ? telemetry::ScalarCode::F32
	                                                            : telemetry::ScalarCode::F64));
	const auto* number = std::get_if<Floating>(&result.value.value);
	CHECK(number);
	// Read the variant's representation directly, without FP comparison or arithmetic.
	CHECK(std::bit_cast<Bits>(*number) == expected);
}

template<class Model>
static void richSnapshot(Model& model) noexcept
{
	const auto root = checkedRoot(model);
	const auto info = model.info(root);
	CHECK(info && info.value.kind == telemetry::TypeKind::Struct);
	CHECK(model.childCount(root).value == 17);
	CHECK(scalar<bool>(model, child(model, root, 0)));
	CHECK(scalar<std::uint8_t>(model, child(model, root, 1)) == UINT8_MAX);
	CHECK(scalar<std::int8_t>(model, child(model, root, 2)) == INT8_MIN);
	CHECK(scalar<std::uint16_t>(model, child(model, root, 3)) == UINT16_MAX);
	CHECK(scalar<std::int16_t>(model, child(model, root, 4)) == INT16_MIN);
	CHECK(scalar<std::uint32_t>(model, child(model, root, 5)) == UINT32_MAX);
	CHECK(scalar<std::int32_t>(model, child(model, root, 6)) == INT32_MIN);
	const auto u64 = child(model, root, 7), i64 = child(model, root, 8);
	CHECK(scalar<std::uint64_t>(model, u64) == UINT64_MAX);
	CHECK(scalar<std::int64_t>(model, i64) == INT64_MIN);
	decimal(model, u64, "18446744073709551615");
	decimal(model, i64, "-9223372036854775808");
	scalarBits<float>(model, child(model, root, 9), UINT32_C(0x80000000));
	scalarBits<double>(model, child(model, root, 10), UINT64_C(0x7ff0000000000000));
	const auto inner = child(model, root, 11);
	CHECK(model.childCount(inner).value == 4);
	CHECK(scalar<std::uint16_t>(model, child(model, inner, 0)) == 0x1234);
	const auto bins = child(model, inner, 1);
	CHECK(model.childCount(bins).value == 3);
	CHECK(scalar<std::int8_t>(model, child(model, bins, 0)) == INT8_MIN);
	CHECK(scalar<std::int8_t>(model, child(model, bins, 2)) == INT8_MAX);
	const auto named = model.scalar(child(model, inner, 2));
	CHECK(named && named.value.enumValue && named.value.enumKnown);
	CHECK(named.value.enumName == "Negative");
	CHECK(std::get<std::int16_t>(named.value.value) == -7);
	const auto rows = child(model, root, 12);
	CHECK(model.childCount(rows).value == 2);
	const auto unknownNode = child(model, child(model, rows, 1), 2);
	const auto unknown = model.scalar(unknownNode);
	CHECK(unknown && unknown.value.enumValue && !unknown.value.enumKnown);
	CHECK(unknown.value.enumName.empty());
	CHECK(std::get<std::int16_t>(unknown.value.value) == -2);
	decimal(model, unknownNode, "-2");
	const auto matrix = child(model, root, 13);
	CHECK(scalar<std::uint32_t>(model, child(model, child(model, matrix, 0), 1)) == 0xaabbccdd);
	CHECK(scalar<std::uint32_t>(model, child(model, child(model, matrix, 1), 0)) == 42);
	const auto empties = child(model, root, 14);
	CHECK(model.childCount(empties).value == 2);
	CHECK(model.childCount(child(model, empties, 0)).value == 0);
	CHECK(model.childCount(child(model, root, 15)).value == 0);
	CHECK(!model.child(root, 17));
	CHECK(!model.child(bins, 3));
	CHECK(!model.child(child(model, root, 0), 0));
	CHECK(!model.scalar(root));
	const auto wide = model.scalar(child(model, root, 16));
	CHECK(wide && wide.value.enumValue && wide.value.enumKnown);
	CHECK(wide.value.enumName == "High");
	CHECK(std::get<std::uint64_t>(wide.value.value) == UINT64_MAX);
	decimal(model, child(model, root, 16), "18446744073709551615");
	std::array<char, 3> shortText{};
	CHECK(!model.decimal(u64, shortText));
	std::array<char, 32> text{};
	CHECK(!model.decimal(child(model, root, 9), text));
}

static void metadataAndEncoded() noexcept
{
	Encoded fixture;
	auto& model = fixture.inspector;
	CHECK(model.fields().size() == 7);
	CHECK(model.commands().size() == 1);
	CHECK(model.services().size() == 1);
	CHECK(model.catalogs(telemetry::descriptor::Category::Field).size() == 1);
	CHECK(model.catalogs(telemetry::descriptor::Category::Command).size() == 1);
	CHECK(model.catalogs(telemetry::descriptor::Category::Service).size() == 1);
	CHECK(model.fields()[0].name == "Packet");
	CHECK(model.commands()[0].name == "Command");
	CHECK(model.services()[0].name == "Service");
	CHECK(device.reads == 0 && model.pending() == 0);
	CHECK(!model.root());
	CHECK(!model.bindField(7));
	const auto field = fixture.bind();
	CHECK(field.operation() == cl::Operation::FieldRead);
	CHECK(field.requestBytes() == 0 && field.responseBytes() == 91);
	unsigned calls = 0;
	CHECK(model.readField(field, [&](const auto& result) noexcept {
		++calls;
		CHECK(result.operation.succeeded());
		CHECK(result.published &&
		      result.valueStatus == telemetry::integration::dynamic::ValueStatus::Ok);
		CHECK(result.modelStatus == im::ModelStatus::Ok);
	}));
	fixture.pump();
	CHECK(calls == 1 && device.reads == 1);
	richSnapshot(model);
	const auto root = checkedRoot(model);
	std::fill(fixture.rx.begin(), fixture.rx.end(), std::byte{0x5a});
	std::fill(fixture.reply.begin(), fixture.reply.end(), std::byte{0xa5});
	std::fill(fixture.scratch.begin(), fixture.scratch.end(), std::byte{0x33});
	device.packet.u64 = 1;
	CHECK(scalar<std::uint64_t>(model, child(model, root, 7)) == UINT64_MAX);
	CHECK(model.readField(field, [&](const auto& result) noexcept {
		++calls;
		CHECK(result.published);
	}));
	CHECK(!model.info(root));
	CHECK(!model.root());
	fixture.pump();
	CHECK(calls == 2 && device.reads == 2);
	CHECK(scalar<std::uint64_t>(model, child(model, checkedRoot(model), 7)) == 1);
	readonlyCounters();
}

static void zeroAndCapacity() noexcept
{
	Encoded<4, 8> fixture;
	auto& model = fixture.inspector;
	unsigned calls = 0;
	const auto wide = fixture.bind(4);
	CHECK(model.readField(wide, [&](const auto& result) noexcept {
		++calls;
		CHECK(result.published);
	}));
	fixture.pump();
	const auto oldRoot = checkedRoot(model);
	const auto large = model.readField(fixture.bind(0), [&](const auto&) noexcept {
		++calls;
	});
	CHECK(!large && large.status == cl::Status::BufferTooSmall);
	CHECK(large.modelStatus == im::ModelStatus::SnapshotTooLarge);
	CHECK(model.pending() == 0 && device.reads == 1 && calls == 1);
	CHECK(model.info(oldRoot));
	CHECK(scalar<std::uint64_t>(model, oldRoot) == UINT64_MAX);
	for (const auto id : {1U, 2U, 3U}) {
		CHECK(model.readField(fixture.bind(id), [&](const auto& result) noexcept {
			++calls;
			if (!result.published)
				std::fprintf(stderr, "zero field %u: operation=%u dispatch=%u value=%u model=%u\n",
				             id, unsigned(result.operation.status),
				             unsigned(result.operation.dispatch), unsigned(result.valueStatus),
				             unsigned(result.modelStatus));
			CHECK(result.published);
		}));
		fixture.pump();
		const auto root = checkedRoot(model);
		CHECK(model.childCount(root).value == (id == 3 ? 65536 : 0));
		if (id == 3) {
			CHECK(model.childCount(child(model, root, 65535)).value == 0);
			CHECK(!model.child(root, 65536));
		}
	}
	CHECK(calls == 4 && device.reads == 4);
	readonlyCounters();
}

static void rpcReads() noexcept
{
	RpcHarness fixture;
	unsigned calls = 0;
	CHECK(fixture.inspector.readField(fixture.bind(), [&](const auto& result) noexcept {
		++calls;
		CHECK(result.operation.succeeded() && result.published);
	}));
	fixture.pump();
	CHECK(calls == 1 && device.reads == 1);
	CHECK(fixture.writePackets == 1 && fixture.readPackets >= 3);
	richSnapshot(fixture.inspector);
	for (const auto id : {4U, 5U}) {
		CHECK(fixture.inspector.readField(fixture.bind(id), [&](const auto& result) noexcept {
			++calls;
			CHECK(result.published);
		}));
		fixture.pump();
		decimal(fixture.inspector, checkedRoot(fixture.inspector),
		        id == 4 ? "18446744073709551615" : "-9223372036854775808");
	}
	CHECK(calls == 3 && device.reads == 3);
	readonlyCounters();
}

template<class Fixture>
static void floatingPointSnapshots() noexcept
{
	Fixture fixture;
	auto& model = fixture.inspector;
	const auto field = fixture.bind();

	struct Pattern {
		std::uint32_t f32;
		std::uint64_t f64;
	};

	// Both signs of quiet/signaling NaNs have nonzero, distinct payloads.
	constexpr std::array patterns{Pattern{UINT32_C(0x7fc1a2b3), UINT64_C(0x7ff8123456789abc)},
	                              Pattern{UINT32_C(0xffd4c3b2), UINT64_C(0xfff9fedcba987654)},
	                              Pattern{UINT32_C(0x7f812345), UINT64_C(0x7ff0123456789abc)},
	                              Pattern{UINT32_C(0xff923456), UINT64_C(0xfff123456789abcd)},
	                              Pattern{UINT32_C(0x00000000), UINT64_C(0x0000000000000000)},
	                              Pattern{UINT32_C(0x80000000), UINT64_C(0x8000000000000000)}};
	unsigned calls = 0;
	for (const auto& pattern : patterns) {
		device.packet.f32 = std::bit_cast<float>(pattern.f32);
		device.packet.f64 = std::bit_cast<double>(pattern.f64);
		CHECK(std::bit_cast<std::uint32_t>(device.packet.f32) == pattern.f32);
		CHECK(std::bit_cast<std::uint64_t>(device.packet.f64) == pattern.f64);
		CHECK(model.readField(field, [&](const auto& result) noexcept {
			++calls;
			CHECK(result.operation.succeeded());
			CHECK(result.published);
		}));
		fixture.pump();
		CHECK(device.reads == calls);
		const auto root = checkedRoot(model);
		const auto f32 = child(model, root, 9), f64 = child(model, root, 10);
		scalarBits<float>(model, f32, pattern.f32);
		scalarBits<double>(model, f64, pattern.f64);
		// Prove the scalar comes from the Inspector's snapshot after transport reuse.
		if constexpr (requires { fixture.rx; })
			std::fill(fixture.rx.begin(), fixture.rx.end(), std::byte{0x5a});
		else
			std::fill(fixture.result.begin(), fixture.result.end(), std::byte{0x5a});
		std::fill(fixture.reply.begin(), fixture.reply.end(), std::byte{0xa5});
		std::fill(fixture.scratch.begin(), fixture.scratch.end(), std::byte{0x33});
		device.packet.f32 = std::bit_cast<float>(UINT32_C(0x3f800000));
		device.packet.f64 = std::bit_cast<double>(UINT64_C(0x3ff0000000000000));
		scalarBits<float>(model, f32, pattern.f32);
		scalarBits<double>(model, f64, pattern.f64);
	}
	CHECK(calls == patterns.size());
	CHECK(model.pending() == 0);
	readonlyCounters();
}

static void boundedQueue() noexcept
{
	Encoded<2> fixture;
	auto& model = fixture.inspector;
	const auto field = fixture.bind();
	unsigned first = 0, second = 0, rejected = 0;
	CHECK(model.readField(field, [&](const auto& result) noexcept {
		++first;
		CHECK(!result.published);
		CHECK(result.operation.succeeded());
	}));
	const auto next = model.readField(field, [&](const auto& result) noexcept {
		++second;
		CHECK(result.published);
	});
	CHECK(next);
	const auto full = model.readField(field, [&](const auto&) noexcept {
		++rejected;
	});
	CHECK(!full && full.status == cl::Status::QueueFull);
	CHECK(model.pending() == 2 && device.reads == 0);
	fixture.pump();
	CHECK(first == 1 && second == 1 && rejected == 0 && device.reads == 2);
	CHECK(model.root());
	unsigned cancelled = 0;
	const auto queued = model.readField(field, [&](const auto& result) noexcept {
		++cancelled;
		CHECK(result.operation.status == cl::Status::Cancelled);
		CHECK(!result.published);
	});
	CHECK(queued && model.cancel(queued.handle));
	model.process(fixture.now);
	CHECK(cancelled == 1 && model.pending() == 0 && device.reads == 2);
	CHECK(!model.root());
	CHECK(!model.nextPacket());
	model.close();
	model.process(fixture.now);
	CHECK(model.drained());
	const auto closed = model.readField(field, [&](const auto&) noexcept {
		++rejected;
	});
	CHECK(!closed && closed.status == cl::Status::Closed);
	CHECK(rejected == 0);
	readonlyCounters();
}

static void deadlineAndMalformed() noexcept
{
	enum class TimeoutPhase {
		Queued,
		Offered,
		Prepared
	};
	for (const auto phase : {TimeoutPhase::Queued, TimeoutPhase::Offered, TimeoutPhase::Prepared}) {
		Encoded fixture;
		unsigned calls = 0;
		const auto field = fixture.bind();
		CHECK(fixture.inspector.readField(
		    field,
		    [&](const auto& result) noexcept {
			    ++calls;
			    CHECK(result.operation.status == cl::Status::Timeout);
			    CHECK(!result.published);
		    },
		    20));
		CHECK(fixture.inspector.pending() == 1 && calls == 0 && device.reads == 0);
		CHECK(!fixture.backend.nextPacket() && !fixture.inspector.recoveryRequired());
		cl::Packet packet{};
		if (phase != TimeoutPhase::Queued) {
			packet = fixture.offered();
			CHECK(fixture.backend.nextPacket() && calls == 0 && device.reads == 0);
		}
		if (phase == TimeoutPhase::Prepared) {
			// send() checks prepareHandoff() before dispatching the real Model.
			fixture.send(packet, false);
			CHECK(!fixture.backend.nextPacket() && device.reads == 1 && calls == 0);
		}
		fixture.now = 20;
		fixture.inspector.process(fixture.now);
		const bool prepared = phase == TimeoutPhase::Prepared;
		const auto* name = phase == TimeoutPhase::Queued    ? "queued"
		                   : phase == TimeoutPhase::Offered ? "offered"
		                                                    : "prepared";
		std::printf("Inspector timeout phase=%s callbacks=%u pending=%zu reads=%u recovery=%u\n",
		            name, calls, fixture.inspector.pending(), device.reads,
		            unsigned(fixture.inspector.recoveryRequired()));
		std::fflush(stdout);
		CHECK(calls == 1 && fixture.inspector.pending() == 0);
		CHECK(fixture.inspector.recoveryRequired() == prepared);
		CHECK(device.reads == unsigned(prepared));
		CHECK(!fixture.inspector.nextPacket());
		CHECK(!fixture.inspector.prepareHandoff(fixture.inspector.session(),
		                                        packet ? packet.requestId : 1));
		if (prepared) {
			const auto bytes = en::encoded_wire::headerBytes + field.responseBytes();
			CHECK(!fixture.inspector.onReply(cl::Input{fixture.reply}.first(bytes), packet.session,
			                                 packet.requestId));
		}
		fixture.now = 100;
		fixture.inspector.process(fixture.now);
		CHECK(calls == 1 && fixture.inspector.pending() == 0);
		CHECK(device.reads == unsigned(prepared));
		CHECK(fixture.inspector.recoveryRequired() == prepared);
		CHECK(!fixture.inspector.root());
		readonlyCounters();
	}
	for (const bool badValue : {false, true}) {
		Encoded fixture;
		unsigned calls = 0;
		CHECK(fixture.inspector.readField(fixture.bind(6), [&](const auto& result) noexcept {
			++calls;
			CHECK(!result.published);
			CHECK(result.operation.status ==
			      (badValue ? cl::Status::Ok : cl::Status::MalformedReply));
			CHECK(result.valueStatus ==
			      (badValue ? telemetry::integration::dynamic::ValueStatus::InvalidPayload
			                : telemetry::integration::dynamic::ValueStatus::NoValue));
		}));
		const auto packet = fixture.offered();
		CHECK(fixture.inspector.prepareHandoff(packet.session, packet.requestId));
		CHECK(fixture.inspector.onSendAccepted(packet.session, packet.requestId));
		const auto reply = en::dispatchEncoded(modelView, fixture.workspace, packet.session,
		                                       packet.bytes, fixture.reply);
		CHECK(reply && reply.written == 33);
		fixture.reply[badValue ? 32 : 0] = std::byte{2};
		CHECK(fixture.inspector.onReply(cl::Input{fixture.reply}.first(reply.written),
		                                packet.session, packet.requestId));
		fixture.inspector.process(fixture.now);
		CHECK(calls == 1 && !fixture.inspector.root());
		CHECK(fixture.inspector.recoveryRequired() == !badValue);
		readonlyCounters();
	}
}

static void changedSchemaSameEndpointAndWidth() noexcept
{
	Encoded fixture;
	auto& model = fixture.inspector;
	const auto oldBinding = fixture.bind();
	CHECK(model.readField(oldBinding, [](const auto& result) noexcept {
		CHECK(result.published);
	}));
	fixture.pump();
	const auto oldNode = checkedRoot(model);
	unsigned cancelled = 0;
	CHECK(model.readField(oldBinding, [&](const auto& result) noexcept {
		++cancelled;
		CHECK(!result.published &&
		      result.valueStatus == telemetry::integration::dynamic::ValueStatus::Stale);
	}));
	// Valid replacement retains endpoint 0 and 91 wire bytes, but changes its
	// root from a nested struct to an array. Size/id checks alone cannot distinguish it.
	const auto parsed = rt::RuntimeSchema::parse(alternate::bytes, fixture.metadata.workspace);
	CHECK(parsed);
	fixture.metadata.schema = parsed.schema;
	CHECK(!model.info(oldNode));
	CHECK(!model.isCurrent(oldBinding));
	CHECK(!model.nextPacket());
	model.process(fixture.now);
	CHECK(cancelled == 1 && model.pending() == 0 && device.reads == 1);
	CHECK(model.fields().empty());
	CHECK(model.confirmQuiescence({17, true, true, true}));
	CHECK(model.admit(parsed.schema, cl::Input{alternate::bytes}.first(64), 18) == cl::Status::Ok);
	const auto newBinding = fixture.bind();
	CHECK(newBinding.id() == oldBinding.id());
	CHECK(newBinding.responseBytes() == oldBinding.responseBytes());
	CHECK(newBinding.responseType() != oldBinding.responseType());
	CHECK(!model.isCurrent(oldBinding));
	unsigned published = 0;
	CHECK(model.readField(newBinding, [&](const auto& result) noexcept {
		++published;
		CHECK(result.published);
	}));
	const auto packet = fixture.offered();
	CHECK(model.prepareHandoff(packet.session, packet.requestId));
	CHECK(model.onSendAccepted(packet.session, packet.requestId));
	const auto result = en::dispatchEncoded(alternate::modelView, fixture.workspace, 18,
	                                        packet.bytes, fixture.reply);
	CHECK(result);
	CHECK(model.onReply(cl::Input{fixture.reply}.first(result.written), packet.session,
	                    packet.requestId));
	model.process(fixture.now);
	CHECK(published == 1 && alternate::device.reads == 1 && device.reads == 1);
	const auto root = checkedRoot(model);
	CHECK(model.info(root).value.kind == telemetry::TypeKind::Array);
	CHECK(model.childCount(root).value == 91);
	CHECK(scalar<std::uint8_t>(model, child(model, root, 0)) == 42);
	CHECK(!model.info(oldNode));
	readonlyCounters();
}

static void nullCallbacks() noexcept
{
	Encoded fixture;
	auto& model = fixture.inspector;
	const auto field = fixture.bind();
	CHECK(model.readField(field, [](const auto& result) noexcept {
		CHECK(result.published);
	}));
	fixture.pump();
	const auto old = checkedRoot(model);
	const auto revision = model.snapshotInfo().revision;
	void (*callback)(const im::ReadResult&) noexcept = nullptr;
	CHECK(model.readField(field, callback).status == cl::Status::InvalidArgument);
	CHECK(model.refresh(callback).status == cl::Status::InvalidArgument);
	CHECK(model.pending() == 0 && device.reads == 1 && model.info(old));
	CHECK(model.snapshotInfo().revision == revision && !fixture.backend.nextPacket());

	struct FalseCallable {
		unsigned* calls;

		explicit operator bool() const noexcept
		{
			return false;
		}

		void operator()(const im::ReadResult& result) noexcept
		{
			CHECK(result.published);
			++*calls;
		}
	};

	unsigned calls = 0;
	CHECK(model.refresh(FalseCallable{&calls}));
	fixture.pump();
	CHECK(calls == 1 && device.reads == 2);
	model.invalidate();
	CHECK(model.readField(field, callback).status == cl::Status::NotAdmitted);
	model.close();
	CHECK(model.readField(field, callback).status == cl::Status::Closed);
	model.process(fixture.now);
	CHECK(model.drained());
}

static unsigned functionCalls = 0;

static void functionCompletion(const im::ReadResult& result) noexcept
{
	CHECK(result.operation.succeeded() && result.published);
	++functionCalls;
}

static void functionReferences() noexcept
{
	Encoded fixture;
	functionCalls = 0;
	CHECK(fixture.inspector.readField(fixture.bind(), functionCompletion));
	fixture.pump();
	CHECK(functionCalls == 1 && device.reads == 1);
	fixture.inspector.close();
	fixture.inspector.process(fixture.now);
	CHECK(fixture.inspector.drained());
}

static void frozenBindingDuringOwnership() noexcept
{
	Encoded fixture;
	auto field = fixture.bind(0);
	const auto replacement = fixture.bind(1);
	bool changed = false;
	unsigned calls = 0;

	struct ReplaceOnMove {
		telemetry::integration::dynamic::EndpointBinding* target;
		const telemetry::integration::dynamic::EndpointBinding* replacement;
		bool* changed;
		unsigned* calls;

		ReplaceOnMove(decltype(target) target, decltype(replacement) replacement, bool& changed,
		              unsigned& calls) noexcept
		    : target(target), replacement(replacement), changed(&changed), calls(&calls)
		{}

		ReplaceOnMove(ReplaceOnMove&& other) noexcept
		    : target(other.target), replacement(other.replacement), changed(other.changed),
		      calls(other.calls)
		{
			if (!*changed) {
				*changed = true;
				*target = *replacement;
			}
		}

		void operator()(const im::ReadResult& result) noexcept
		{
			CHECK(result.published && result.fieldId == 0);
			++*calls;
		}
	};

	CHECK(fixture.inspector.readField(field, ReplaceOnMove{&field, &replacement, changed, calls}));
	CHECK(changed && field.id() == 1);
	const auto packet = fixture.offered();
	CHECK(telemetry::protocol::encoded::parse(packet.bytes).header.endpoint == 0);
	CHECK(fixture.inspector.snapshotInfo().fieldId == 0);
	fixture.send(packet);
	CHECK(calls == 1 && fixture.inspector.root());
	CHECK(fixture.inspector.snapshotInfo().fieldId == 0 && device.reads == 1);
	fixture.inspector.close();
	fixture.inspector.process(fixture.now);
	CHECK(fixture.inspector.drained());
}

struct PublicationState {
	Encoded<>* fixture;
	telemetry::integration::dynamic::EndpointBinding field;
	cl::RequestHandle queued;
	unsigned moves = 0, destructors = 0, calls = 0, nestedCalls = 0;
	bool moved = false, destroyed = false;

	void exercise() noexcept
	{
		auto& model = fixture->inspector;
		const auto before = model.snapshotInfo();
		const auto pending = model.pending();
		CHECK(before.selected && pending >= 1 && pending <= 2 && device.reads == 0);
		CHECK(model.selectField(field) == cl::Status::Busy);
		CHECK(model
		          .readField(field,
		                     [&](const auto&) noexcept {
			                     ++nestedCalls;
		                     })
		          .status == cl::Status::Busy);
		CHECK(model
		          .refresh([&](const auto&) noexcept {
			          ++nestedCalls;
		          })
		          .status == cl::Status::Busy);
		CHECK(model.admit(fixture->metadata.schema, cl::Input{bytes}.first(64), 18) ==
		      cl::Status::Busy);
		model.clearSelection();
		model.process(fixture->now);
		CHECK(!model.nextPacket() && !fixture->backend.nextPacket());
		CHECK(!model.prepareHandoff(17, 1) && !model.onSendAccepted(17, 1));
		CHECK(!model.onReply({}, 17, 1) && !model.cancel(queued));
		CHECK(!model.confirmQuiescence({17, true, true, true}));
		const auto after = model.snapshotInfo();
		CHECK(after.selected && after.fieldId == before.fieldId &&
		      after.revision == before.revision);
		CHECK(model.pending() == pending && device.reads == 0 && nestedCalls == 0);
	}
};

struct PublicationCallback {
	PublicationState* state;
	unsigned depth = 0;
	bool movedFrom = false;

	explicit PublicationCallback(PublicationState& value) noexcept : state(&value)
	{}

	PublicationCallback(PublicationCallback&& other) noexcept
	    : state(other.state), depth(other.depth + 1)
	{
		other.movedFrom = true;
		++state->moves;
		if (!state->moved) {
			state->moved = true;
			state->exercise();
		}
	}

	~PublicationCallback()
	{
		// Skip the original caller-owned object, whose destruction is outside
		// readField(). Moved-from wrapper temporaries retire inside publication.
		if (movedFrom && depth && !state->destroyed) {
			state->destroyed = true;
			++state->destructors;
			state->exercise();
		}
	}

	void operator()(const im::ReadResult& result) noexcept
	{
		CHECK(result.published && result.fieldId == 1);
		++state->calls;
	}
};

static void ownershipReentry() noexcept
{
	Encoded fixture;
	unsigned superseded = 0;
	const auto first =
	    fixture.inspector.readField(fixture.bind(0), [&](const auto& result) noexcept {
		    ++superseded;
		    CHECK(result.operation.succeeded() && !result.published);
	    });
	CHECK(first);
	PublicationState state{&fixture, fixture.bind(1), first.handle};
	PublicationCallback completion{state};
	CHECK(fixture.inspector.readField(state.field, std::move(completion)));
	CHECK(state.moved && state.destroyed && state.moves > 0 && state.destructors == 1);
	CHECK(fixture.inspector.pending() == 2 && state.nestedCalls == 0 &&
	      !fixture.backend.nextPacket());
	fixture.pump();
	CHECK(superseded == 1 && state.calls == 1 && state.nestedCalls == 0 && device.reads == 2);
	CHECK(fixture.inspector.snapshotInfo().fieldId == 1 && fixture.inspector.root());
	fixture.inspector.close();
	fixture.inspector.process(fixture.now);
	CHECK(fixture.inspector.drained());
}

static void ownershipRevocation() noexcept
{
	for (bool close : {false, true}) {
		Encoded fixture;
		const auto field = fixture.bind();
		CHECK(fixture.inspector.readField(field, [](const auto& result) noexcept {
			CHECK(result.published);
		}));
		fixture.pump();
		const auto old = checkedRoot(fixture.inspector);
		unsigned calls = 0;
		bool revoked = false;

		struct RevokeOnMove {
			Encoded<>* fixture;
			bool close;
			bool* revoked;
			unsigned* calls;

			RevokeOnMove(Encoded<>& fixture, bool close, bool& revoked, unsigned& calls) noexcept
			    : fixture(&fixture), close(close), revoked(&revoked), calls(&calls)
			{}

			RevokeOnMove(RevokeOnMove&& other) noexcept
			    : fixture(other.fixture), close(other.close), revoked(other.revoked),
			      calls(other.calls)
			{
				if (!*revoked) {
					*revoked = true;
					if (close)
						fixture->inspector.close();
					else
						fixture->inspector.invalidate();
					CHECK(!fixture->inspector.drained());
					// Revocation wins over the transient publication Busy gate.
					CHECK(fixture->inspector
					          .refresh([](const auto&) noexcept {
					          })
					          .status == (close ? cl::Status::Closed : cl::Status::NotAdmitted));
				}
			}

			void operator()(const im::ReadResult&) noexcept
			{
				++*calls;
			}
		};

		const auto submitted =
		    fixture.inspector.readField(field, RevokeOnMove{fixture, close, revoked, calls});
		CHECK(submitted.status == (close ? cl::Status::Closed : cl::Status::NotAdmitted));
		CHECK(revoked && calls == 0 && fixture.inspector.pending() == 0 && device.reads == 1);
		CHECK(!fixture.inspector.info(old) && !fixture.inspector.root() &&
		      !fixture.inspector.nextPacket());
		fixture.inspector.close();
		fixture.inspector.process(fixture.now);
		CHECK(fixture.inspector.drained());
	}
}

int main()
{
	functionReferences();
	nullCallbacks();
	frozenBindingDuringOwnership();
	ownershipReentry();
	ownershipRevocation();
	metadataAndEncoded();
	zeroAndCapacity();
	rpcReads();
	floatingPointSnapshots<Encoded<>>();
	floatingPointSnapshots<RpcHarness>();
	boundedQueue();
	deadlineAndMalformed();
	changedSchemaSameEndpointAndWidth();
	std::printf("Inspector Host: %u checks, 0 failures\n", checks);
	return 0;
}
