/* Actual generated and handwritten typed mixed batches, one FIFO. MIT. */
#include "Fixture.hpp"
using namespace static_batch_test;

using RepresentativeRequest =
    decltype(bt::items(bt::read(api::control::fields::Config),
                       bt::command(api::control::commands::Configure, std::declval<Data&>())));
using RepresentativeStore = bt::StaticBatchStorage<api::client::CodecPolicy, RepresentativeRequest>;
static_assert(std::is_trivially_destructible_v<RepresentativeStore>);
constinit RepresentativeStore representativeStore;

template<class Codec>
static void ordered() noexcept
{
	Harness<Codec> h;
	Data written = input(), configured = input(37), echoed = input(99);
	const auto request = bt::items(
	    bt::write(api::control::fields::Config, written), bt::read(api::control::fields::Config),
	    bt::command(api::control::commands::Configure, configured),
	    bt::read(api::control::fields::Counter), bt::service(api::control::services::Echo, echoed),
	    bt::command(api::control::commands::Accepted, configured),
	    bt::service(api::control::services::Busy, echoed), bt::read(api::control::fields::Config));
	bt::StaticBatchStorage<Codec, decltype(request)> storage;
	static_assert(std::is_trivially_destructible_v<decltype(storage)>);
	const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
	CHECK(bound && !storage.busy());
	unsigned calls = 0;
	CHECK(bt::batch(h.client, bound.binding, request, storage, [&](const auto& result) noexcept {
		++calls;
		CHECK(storage.busy() && result.operation.succeeded());
		CHECK(result.valueStatus == bt::StaticValueStatus::Ok);
		const auto write = result.template get<0>();
		const auto read = result.template get<1>();
		const auto command = result.template get<2>();
		const auto counter = result.template get<3>();
		const auto service = result.template get<4>();
		const auto accepted = result.template get<5>();
		const auto busy = result.template get<6>();
		const auto final = result.template get<7>();
		CHECK(write && write->operation.succeeded() && !write->hasValue());
		CHECK(read && read->hasValue());
		CHECK(read->valueIf() == std::addressof(read->value()));
		exact(read->value(), 1000);
		CHECK(command && command->operation.executed() && !command->hasValue());
		CHECK(counter && counter->hasValue() && counter->value() == (37u ^ 0x55u));
		CHECK(counter->valueIf() && *counter->valueIf() == (37u ^ 0x55u));
		CHECK(service && service->hasValue());
		CHECK(service->valueIf() == std::addressof(service->value()));
		exact(service->value(), 99);
		CHECK(accepted && accepted->operation.accepted() && !accepted->operation.executed());
		CHECK(busy && !busy->operation.succeeded() && !busy->hasValue());
		CHECK(busy->valueIf() == nullptr);
		CHECK(busy->operation.endpointStatus ==
		      static_cast<unsigned>(telemetry::ServiceStatus::Busy));
		CHECK(final && final->hasValue());
		CHECK(final->valueIf() == std::addressof(final->value()));
		exact(final->value(), 37u ^ 0x55u);
	}));
	CHECK(storage.busy() && h.client.pending() == 1 && calls == 0 && native::traceCount == 0);
	const auto packet = h.offered();
	const auto envelope = ew::parse(packet.bytes);
	CHECK(envelope && envelope.header.operation == ew::Operation::Batch);
	const auto parsed = bw::parseRequest(envelope.payload);
	CHECK(parsed && parsed.count == 8);
	constexpr std::array operations{ew::Operation::FieldWrite, ew::Operation::FieldRead,
	                                ew::Operation::Command,    ew::Operation::FieldRead,
	                                ew::Operation::Service,    ew::Operation::Command,
	                                ew::Operation::Service,    ew::Operation::FieldRead};
	constexpr std::array<unsigned, 8> indices{0, 0, 0, 1, 0, 2, 3, 0};
	for (std::size_t i = 0; i < parsed.count; ++i) {
		CHECK(parsed.items[i].operation == operations[i]);
		CHECK(parsed.items[i].id == (0x10000u | indices[i]));
	}
	// Submission owns encoded values even though the request recipe borrows DTOs.
	written = input(1);
	configured = input(2);
	echoed = input(3);
	h.send(packet);
	CHECK(calls == 1 && !storage.busy() && h.client.pending() == 0);
	const std::array<native::Event, 8> expected{{{2, 1, 0, 1111},
	                                             {1, 1, 0, 1000},
	                                             {3, 1, 0, 37},
	                                             {1, 1, 1, 37u ^ 0x55u},
	                                             {4, 1, 0, 99},
	                                             {3, 1, 2, 37},
	                                             {4, 1, 3, 99},
	                                             {1, 1, 0, 37u ^ 0x55u}}};
	CHECK(native::traceCount == expected.size());
	for (std::size_t i = 0; i < expected.size(); ++i)
		CHECK(native::trace[i] == expected[i]);
	CHECK(native::decoy.reads == 0 && native::decoy.writes == 0 && native::decoy.commands == 0 &&
	      native::decoy.services == 0);
	CHECK(native::device.reads == 3 && native::device.writes == 1 && native::device.commands == 2 &&
	      native::device.services == 2);
	h.stop();
}

template<class Codec>
static void zeroWire() noexcept
{
	Harness<Codec> h;
	Empty empty;
	Data data = input(23);
	const auto request = bt::items(bt::command(api::control::commands::Reset),
	                               bt::command(api::control::commands::EmptyArgument, empty),
	                               bt::service(api::control::services::Ping),
	                               bt::service(api::control::services::EmptyService),
	                               bt::service(api::control::services::Consume, data),
	                               bt::service(api::control::services::EmptyRequest, empty),
	                               bt::read(api::control::fields::Empty));
	bt::StaticBatchStorage<Codec, decltype(request)> storage;
	const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
	CHECK(bound);
	unsigned calls = 0;
	CHECK(bt::batch(h.client, bound.binding, request, storage, [&](const auto& result) noexcept {
		++calls;
		CHECK(result.operation.succeeded() && result.valueStatus == bt::StaticValueStatus::Ok);
		CHECK(result.template get<0>() && result.template get<0>()->operation.executed());
		CHECK(result.template get<1>() && result.template get<1>()->operation.executed());
		CHECK(result.template get<2>() && !result.template get<2>()->hasValue());
		CHECK(result.template get<3>() && result.template get<3>()->hasValue());
		CHECK(result.template get<3>()->valueIf() != nullptr);
		CHECK(result.template get<3>()->operation.payload.empty());
		CHECK(result.template get<4>() && !result.template get<4>()->hasValue());
		CHECK(result.template get<5>() && result.template get<5>()->hasValue());
		exact(result.template get<5>()->value(), 0);
		CHECK(result.template get<6>() && result.template get<6>()->hasValue());
		CHECK(result.template get<6>()->valueIf() != nullptr);
		CHECK(result.template get<6>()->operation.payload.empty());
	}));
	h.send(h.offered());
	CHECK(calls == 1 && !storage.busy());
	h.stop();
}

struct Observed {
	unsigned calls = 0;
	cl::Status status = cl::Status::Busy;
	bt::StaticValueStatus valueStatus = bt::StaticValueStatus::NoValue;
	bool item = false, value = false;

	template<class T>
	void record(const T& result) noexcept
	{
		++calls;
		status = result.operation.status;
		valueStatus = result.valueStatus;
		const auto first = result.template get<0>();
		item = bool(first);
		value = first && first->hasValue();
	}
};

static void lifecycle() noexcept
{
	for (bool cancel : {false, true}) {
		for (unsigned phase = 0; phase != 3; ++phase) {
			Harness<> h;
			Data value = input();
			const auto request = bt::items(bt::write(api::control::fields::Config, value),
			                               bt::read(api::control::fields::Config));
			bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
			const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
			Observed observed;
			const auto submitted = bt::batch(
			    h.client, bound.binding, request, storage,
			    [&observed](const auto& result) noexcept {
				    observed.record(result);
			    },
			    50);
			CHECK(submitted && storage.busy());
			cl::Packet packet;
			if (phase)
				packet = h.offered();
			if (phase == 2)
				CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
			if (cancel)
				CHECK(h.client.cancel(submitted.handle));
			else
				h.now = 50;
			h.client.process(h.now);
			CHECK(observed.calls == 1 && !observed.item && !observed.value);
			CHECK(observed.status == (phase == 2 ? cl::Status::OutcomeUnknown
			                          : cancel   ? cl::Status::Cancelled
			                                     : cl::Status::Timeout));
			CHECK(h.client.recoveryRequired() == (phase == 2));
			CHECK(!storage.busy() && h.client.pending() == 0 && native::traceCount == 0);
			h.client.process(h.now);
			CHECK(observed.calls == 1 && !h.client.cancel(submitted.handle));
			if (phase) {
				CHECK(!h.client.prepareHandoff(packet.session, packet.requestId));
				const auto bytes = h.response(packet);
				CHECK(!h.client.onReply(cl::Input{h.reply}.first(bytes), packet.session,
				                        packet.requestId));
				CHECK(observed.calls == 1);
			}
			h.stop();
		}
	}
	for (unsigned delay : {49u, 50u, 51u}) {
		Harness<> h;
		Data value = input();
		const auto request = bt::items(bt::write(api::control::fields::Config, value));
		bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
		const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
		Observed observed;
		CHECK(bt::batch(
		    h.client, bound.binding, request, storage,
		    [&observed](const auto& result) noexcept {
			    observed.record(result);
		    },
		    50));
		const auto packet = h.offered();
		h.handoff(packet);
		const auto bytes = h.response(packet);
		CHECK(native::device.writes == 1);
		h.now = delay;
		CHECK(h.client.onReply(cl::Input{h.reply}.first(bytes), packet.session, packet.requestId) ==
		      (delay < 50));
		h.client.process(h.now);
		CHECK(observed.calls == 1 && !storage.busy());
		CHECK(observed.status == (delay < 50 ? cl::Status::Ok : cl::Status::OutcomeUnknown));
		CHECK(observed.item == (delay < 50));
		CHECK(native::device.writes == 1); // No implicit replay, even after uncertainty.
		h.stop();
	}
}

struct CountingCodec : api::client::CodecPolicy {
	static inline unsigned encodes = 0;

	template<class T>
	static std::uint8_t encode(const T& value, std::span<std::byte> bytes) noexcept
	{
		++encodes;
		return CodecPolicy::encode(value, bytes);
	}
};

static void refusalAndReuse() noexcept
{
	Harness<CountingCodec, 1> h;
	Data value = input();
	const auto request = bt::items(bt::write(api::control::fields::Config, value),
	                               bt::read(api::control::fields::Config));
	bt::StaticBatchStorage<CountingCodec, decltype(request)> storage;
	const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
	Observed observed;
	unsigned ordinary = 0;
	CHECK(h.client.read(api::control::fields::Bool, [&](const auto& result) noexcept {
		CHECK(result.succeeded() && result.hasValue() && result.value());
		++ordinary;
	}));
	CountingCodec::encodes = 0;
	CHECK(bt::batch(h.client, bound.binding, request, storage,
	                [&observed](const auto& result) noexcept {
		                observed.record(result);
	                })
	          .status == cl::Status::QueueFull);
	CHECK(CountingCodec::encodes == 0 && !storage.busy() && observed.calls == 0);
	CHECK(h.client.pending() == 1 && native::traceCount == 0);
	exact(value, 1111);
	h.send(h.offered());
	CHECK(ordinary == 1);
	CHECK(bt::batch(h.client, bound.binding, request, storage,
	                [&observed](const auto& result) noexcept {
		                observed.record(result);
	                }));
	CHECK(storage.busy());
	CHECK(bt::batch(h.client, bound.binding, request, storage,
	                [&observed](const auto& result) noexcept {
		                observed.record(result);
	                })
	          .status == cl::Status::Busy);
	h.send(h.offered());
	CHECK(observed.calls == 1 && !storage.busy());
	CHECK(bt::batch(h.client, bound.binding, request, storage,
	                [&observed](const auto& result) noexcept {
		                observed.record(result);
	                }));
	h.send(h.offered());
	CHECK(observed.calls == 2 && !storage.busy());
	h.stop();

	Harness<CountingCodec, 2, 58> small;
	const auto tooBig = bt::bindBatch(small.client, small.backend, small.capability, request);
	CountingCodec::encodes = 0;
	CHECK(bt::batch(small.client, tooBig.binding, request, storage,
	                [&observed](const auto& result) noexcept {
		                observed.record(result);
	                })
	          .status == cl::Status::PayloadFull);
	CHECK(CountingCodec::encodes == 0 && !storage.busy() && small.client.pending() == 0);
	exact(value, 1111);
	small.stop();
}

static void malformedLast() noexcept
{
	for (unsigned mutation = 0; mutation < 8; ++mutation) {
		Harness<> h;
		const auto request = bt::items(bt::read(api::control::fields::Config),
		                               bt::service(api::control::services::Flag));
		bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
		const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
		unsigned calls = 0;
		CHECK(
		    bt::batch(h.client, bound.binding, request, storage, [&](const auto& result) noexcept {
			    ++calls;
			    CHECK(result.operation.succeeded() &&
			          result.valueStatus == bt::StaticValueStatus::InvalidPayload);
			    const auto earlier = result.template get<0>();
			    const auto later = result.template get<1>();
			    CHECK(!earlier || !earlier->hasValue());
			    CHECK(!later || !later->hasValue());
			    CHECK(!earlier || earlier->valueIf() == nullptr);
			    CHECK(!later || later->valueIf() == nullptr);
			    if (earlier) {
				    auto copied = *earlier;
				    copied.valueStatus = bt::StaticValueStatus::Ok;
				    CHECK(!copied.hasValue());
				    CHECK(copied.valueIf() == nullptr);
			    }
			    if (later) {
				    auto copied = *later;
				    copied.valueStatus = bt::StaticValueStatus::Ok;
				    CHECK(!copied.hasValue());
				    CHECK(copied.valueIf() == nullptr);
			    }
			    if (mutation == 0) {
				    CHECK(earlier && later && earlier->operation.succeeded() &&
				          later->operation.succeeded());
			    } else
				    CHECK(!earlier &&
				          !later); // No default Ok items after failed structural validation.
		    }));
		const auto packet = h.offered();
		h.handoff(packet);
		const auto size = h.response(packet);
		const auto envelope = ew::parse(cl::Input{h.reply}.first(size));
		const auto contract = bw::parseRequest(ew::parse(packet.bytes).payload);
		const auto body = bw::parseReply(envelope.payload, {contract.items.data(), contract.count});
		CHECK(body && body.count == 2 && body.items[1].payload.size() == 1);
		std::array<std::byte, 128> changed{};
		std::copy(envelope.payload.begin(), envelope.payload.end(), changed.begin());
		auto length = envelope.payload.size();
		const auto last =
		    static_cast<std::size_t>(body.items[1].payload.data() - envelope.payload.data());
		switch (mutation) {
			case 0:
				changed[last] = std::byte{2};
				break; // Canonical Bool is checked by Codec.
			case 1:
				changed[0] = std::byte{1};
				break;
			case 2:
				changed[2] ^= std::byte{1};
				break; // First ID.
			case 3:
				changed[last - 12] ^= std::byte{1};
				break; // Last item ID.
			case 4:
				changed[last - 8] = std::byte{0};
				break; // Invalid operation.
			case 5:
				changed[last - 5] = std::byte{1};
				break; // Reserved.
			case 6:
				--length;
				break;
			case 7:
				changed[last - 4] = std::byte{0};
				break; // Declared width differs.
		}
		const auto encoded = ew::write(h.reply, envelope.header, cl::Input{changed}.first(length));
		CHECK(encoded);
		h.deliver(packet, encoded.written);
		CHECK(calls == 1 && !storage.busy() && !h.client.recoveryRequired());
		h.stop();
	}
}

template<class Client, class Binding, class Request, class Store>
struct OwnershipState {
	Client* client;
	const Binding* binding;
	const Request* request;
	Store* storage;
	Data* source;
	unsigned moves = 0, destructors = 0, busyAttempts = 0, calls = 0;

	void attempt() noexcept
	{
		++busyAttempts;
		CHECK(storage->busy());
		CHECK(bt::batch(*client, *binding, *request, *storage, [](const auto&) noexcept {
		      }).status == cl::Status::Busy);
	}
};

template<class State>
struct ReenterOwnership {
	State* state;

	explicit ReenterOwnership(State& value) noexcept : state(&value)
	{}

	ReenterOwnership(const ReenterOwnership&) = delete;

	ReenterOwnership(ReenterOwnership&& other) noexcept : state(other.state)
	{
		if (++state->moves == 1)
			*state->source = input(5);
		state->attempt();
	}

	~ReenterOwnership() noexcept
	{
		++state->destructors;
		state->attempt();
	}

	template<class T>
	void operator()(const T& result) noexcept
	{
		++state->calls;
		CHECK(result.operation.succeeded() && result.valueStatus == bt::StaticValueStatus::Ok);
		CHECK(result.template get<0>() && result.template get<0>()->operation.succeeded());
		state->attempt();
	}
};

static void ownershipReentry() noexcept
{
	Harness<> h;
	Data value = input();
	const auto request = bt::items(bt::write(api::control::fields::Config, value));
	bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
	const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
	OwnershipState state{&h.client, &bound.binding, &request, &storage, &value};
	CHECK(bt::batch(h.client, bound.binding, request, storage, ReenterOwnership{state}));
	CHECK(state.moves > 0 && state.busyAttempts > 0 && value.number_value == 5);
	h.send(h.offered());
	CHECK(state.calls == 1 && state.destructors > 0 && !storage.busy());
	CHECK(state.busyAttempts == state.moves + state.destructors + state.calls);
	CHECK(native::device.writes == 1 && native::device.current.number == 1000);
	CHECK(native::trace[0].number == 1111); // Callback moves cannot change the owned request.
	h.stop();
}

struct HookCodec : api::client::CodecPolicy {
	static inline void (*encodeHook)() noexcept = nullptr;
	static inline void (*decodeHook)() noexcept = nullptr;

	template<class T>
	static std::uint8_t encode(const T& value, std::span<std::byte> bytes) noexcept
	{
		if (const auto hook = std::exchange(encodeHook, nullptr))
			hook();
		return CodecPolicy::encode(value, bytes);
	}

	template<class T>
	static std::uint8_t decode(std::span<const std::byte> bytes, T& value) noexcept
	{
		if (const auto hook = std::exchange(decodeHook, nullptr))
			hook();
		return CodecPolicy::decode(bytes, value);
	}
};

static Harness<HookCodec>* hookOwner = nullptr;

static void codecReentry() noexcept
{
	for (bool duringDecode : {false, true}) {
		Harness<HookCodec> h;
		Data value = input();
		const auto request = bt::items(bt::write(api::control::fields::Config, value),
		                               bt::read(api::control::fields::Config));
		bt::StaticBatchStorage<HookCodec, decltype(request)> storage;
		const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
		hookOwner = &h;
		auto invalidate = +[]() noexcept {
			hookOwner->client.invalidateAdmission();
		};
		if (duringDecode)
			HookCodec::decodeHook = invalidate;
		else
			HookCodec::encodeHook = invalidate;
		unsigned calls = 0;
		const auto submitted =
		    bt::batch(h.client, bound.binding, request, storage, [&](const auto& result) noexcept {
			    ++calls;
			    CHECK(result.operation.succeeded() &&
			          result.valueStatus == bt::StaticValueStatus::Stale);
			    CHECK(result.template get<0>() && result.template get<0>()->operation.succeeded());
			    CHECK(result.template get<1>() && !result.template get<1>()->hasValue());
			    CHECK(result.template get<1>()->valueIf() == nullptr);
			    auto copied = *result.template get<1>();
			    copied.valueStatus = bt::StaticValueStatus::Ok;
			    CHECK(!copied.hasValue());
			    CHECK(copied.valueIf() == nullptr);
		    });
		if (duringDecode) {
			CHECK(submitted);
			h.send(h.offered());
			CHECK(calls == 1 && native::device.writes == 1);
		} else {
			CHECK(submitted.status == cl::Status::NotAdmitted);
			CHECK(calls == 0 && h.client.pending() == 0 && native::traceCount == 0);
		}
		CHECK(!storage.busy());
		hookOwner = nullptr;
		h.stop();
	}
}

static void bindingRefusals() noexcept
{
	Harness<> h;
	Data value = input();
	const auto request = bt::items(bt::write(api::control::fields::Config, value));
	auto capability = h.capability;
	++capability.session;
	CHECK(bt::bindBatch(h.client, h.backend, capability, request).status ==
	      cl::Status::NotAdmitted);
	capability = h.capability;
	capability.fingerprint ^= 1;
	CHECK(bt::bindBatch(h.client, h.backend, capability, request).status ==
	      cl::Status::NotAdmitted);
	capability = h.capability;
	capability.requestLimit = 10;
	CHECK(bt::bindBatch(h.client, h.backend, capability, request).status ==
	      cl::Status::BufferTooSmall);
	capability = h.capability;
	capability.replyLimit = 13;
	CHECK(!bt::bindBatch(h.client, h.backend, capability, request));
	auto readonly = api::control::fields::Config;
	readonly.capabilities = 1;
	CHECK(bt::bindBatch(h.client, h.backend, h.capability, bt::items(bt::write(readonly, value)))
	          .status == cl::Status::ReadOnly);
	auto wrong = api::control::fields::Config;
	wrong.valueBytes = 1;
	CHECK(bt::bindBatch(h.client, h.backend, h.capability, bt::items(bt::write(wrong, value)))
	          .status == cl::Status::InvalidArgument);
	cl::Backend& erased = h.backend;
	CHECK(bt::bindBatch(h.client, erased, h.capability, request).status == cl::Status::Unsupported);
	auto null = request;
	std::get<0>(null.entries).request = nullptr;
	CHECK(bt::bindBatch(h.client, h.backend, h.capability, null).status ==
	      cl::Status::InvalidArgument);
	const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
	bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
	auto changed = request;
	std::get<0>(changed.entries).endpoint.id ^= 1;
	CHECK(bt::batch(h.client, bound.binding, changed, storage, [](const auto&) noexcept {
	      }).status == cl::Status::InvalidArgument);
	CHECK(!storage.busy() && native::traceCount == 0 && h.client.pending() == 0);
	h.client.invalidateAdmission();
	CHECK(h.client.admitDescriptor(api::schema::descriptorHeader, 18) == cl::Status::Ok);
	CHECK(bt::batch(h.client, bound.binding, request, storage, [](const auto&) noexcept {
	      }).status == cl::Status::NotAdmitted);
	CHECK(!storage.busy() && h.client.pending() == 0);
	h.stop();
}

static void sameFifoAndFailures() noexcept
{
	Harness<> h;
	Data value = input();
	const auto request = bt::items(bt::write(api::control::fields::Config, value),
	                               bt::read(api::control::fields::Counter));
	bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
	const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
	unsigned order = 0;
	CHECK(h.client.read(api::control::fields::Bool, [&](const auto& result) noexcept {
		CHECK(order++ == 0 && result.succeeded() && result.hasValue() && result.value());
	}));
	CHECK(bt::batch(h.client, bound.binding, request, storage, [&](const auto& result) noexcept {
		CHECK(order++ == 1 && result.operation.succeeded());
		const auto readback = result.template get<1>();
		CHECK(readback && readback->hasValue() && readback->value() == 1000);
	}));
	CHECK(h.client.read(api::control::fields::Counter, [&](const auto& result) noexcept {
		CHECK(order++ == 2 && result.succeeded() && result.hasValue() && result.value() == 1000);
	}));
	CHECK(h.client.pending() == 3 && native::traceCount == 0);
	for (unsigned i = 0; i < 3; ++i) {
		h.send(h.offered());
		CHECK(order == i + 1);
	}
	CHECK(!storage.busy() && h.client.pending() == 0);
	h.stop();

	Harness<> errors;
	const auto failed = bt::items(bt::command(api::control::commands::Reject, value),
	                              bt::service(api::control::services::Busy, value),
	                              bt::read(api::control::fields::Unavailable),
	                              bt::read(api::control::fields::Config));
	bt::StaticBatchStorage<api::client::CodecPolicy, decltype(failed)> errorStore;
	const auto errorBinding =
	    bt::bindBatch(errors.client, errors.backend, errors.capability, failed);
	unsigned calls = 0;
	CHECK(bt::batch(
	    errors.client, errorBinding.binding, failed, errorStore, [&](const auto& result) noexcept {
		    ++calls;
		    CHECK(result.operation.succeeded() && result.valueStatus == bt::StaticValueStatus::Ok);
		    const auto command = result.template get<0>();
		    const auto service = result.template get<1>();
		    const auto unavailable = result.template get<2>();
		    const auto read = result.template get<3>();
		    CHECK(command && !command->operation.succeeded() && !command->hasValue());
		    CHECK(command->operation.endpointStatus ==
		          static_cast<unsigned>(telemetry::CommandResult::InvalidValue));
		    CHECK(service && !service->operation.succeeded() && !service->hasValue());
		    CHECK(service->valueIf() == nullptr);
		    CHECK(service->operation.endpointStatus ==
		          static_cast<unsigned>(telemetry::ServiceStatus::Busy));
		    CHECK(unavailable && !unavailable->operation.succeeded() && !unavailable->hasValue());
		    CHECK(unavailable->valueIf() == nullptr);
		    CHECK(unavailable->operation.dispatch != 0);
		    CHECK(read && read->hasValue());
		    exact(read->value(), 42);
	    }));
	errors.send(errors.offered());
	CHECK(calls == 1 && !errorStore.busy() && native::traceCount == 3);
	errors.stop();
}

template<std::size_t... I>
auto repeatedReads(std::index_sequence<I...>) noexcept
{
	return bt::items(((void)I, bt::read(api::control::fields::Counter))...);
}

template<std::size_t... I>
auto repeatedEcho(Data& value, std::index_sequence<I...>) noexcept
{
	return bt::items(((void)I, bt::service(api::control::services::Echo, value))...);
}

template<class Request>
static void sizeProfile(const char* name, const Request&) noexcept
{
	using Store = bt::StaticBatchStorage<api::client::CodecPolicy, Request>;
	using Client = api::client::DeviceClient<2, Store::requestBytes>;
	static_assert(std::is_trivially_destructible_v<Store>);
	std::printf(
	    "Static Batch profile=%s clientSlots=2 callbackBytes=64 payloadBytes=%zu clientBytes=%zu storeBytes=%zu encodedBytes=%zu requestBytes=%zu responseBytes=%zu\n",
	    name, Store::requestBytes, sizeof(Client), sizeof(Store), Store::encodedBytes,
	    Store::requestBytes, Store::responseBytes);
}

template<std::size_t Count, bool Large>
static void repeated() noexcept
{
	Harness<> h;
	Data value = input(71);
	const auto request = [&]() noexcept {
		if constexpr (Large)
			return repeatedEcho(value, std::make_index_sequence<Count>{});
		else
			return repeatedReads(std::make_index_sequence<Count>{});
	}();
	sizeProfile(Large ? "32dto" : Count == 32 ? "32read" : "8read", request);
	bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
	const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
	unsigned calls = 0;
	CHECK(bound &&
	      bt::batch(h.client, bound.binding, request, storage, [&](const auto& result) noexcept {
		      ++calls;
		      CHECK(result.operation.succeeded() &&
		            result.valueStatus == bt::StaticValueStatus::Ok);
		      [&]<std::size_t... I>(std::index_sequence<I...>) noexcept {
			      (
			          [&] {
				          const auto item = result.template get<I>();
				          CHECK(item && item->hasValue());
				          if constexpr (Large)
					          exact(item->value(), 71);
				          else
					          CHECK(item->value() == 42);
			          }(),
			          ...);
		      }(std::make_index_sequence<Count>{});
	      }));
	h.send(h.offered());
	CHECK(calls == 1 && native::traceCount == Count && !storage.busy());
	h.stop();
}

using ReadItem = decltype(bt::read(api::control::fields::Counter));
using ReadResult = bt::StaticBatchResult<api::client::CodecPolicy, ReadItem>;
static unsigned freeCalls = 0;

static void freeCompletion(const ReadResult& result) noexcept
{
	const auto value = result.get<0>();
	CHECK(result.operation.succeeded() && value && value->hasValue() && value->value() == 42);
	++freeCalls;
}

struct FalseBool {
	Observed* observed;

	explicit operator bool() const noexcept
	{
		return false;
	}

	template<class T>
	void operator()(const T& result) noexcept
	{
		observed->record(result);
	}
};

struct MoveOnly {
	Observed* observed;

	explicit MoveOnly(Observed& value) noexcept : observed(&value)
	{}

	MoveOnly(const MoveOnly&) = delete;

	MoveOnly(MoveOnly&& other) noexcept : observed(std::exchange(other.observed, nullptr))
	{}

	template<class T>
	void operator()(const T& result) noexcept
	{
		CHECK(observed);
		observed->record(result);
	}
};

static void callableKindsAndClosedRecovery() noexcept
{
	Harness<> h;
	const auto request = bt::items(bt::read(api::control::fields::Counter));
	bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
	const auto bound = bt::bindBatch(h.client, h.backend, h.capability, request);
	void (*nullFunction)(const ReadResult&) noexcept = nullptr;
	CHECK(bt::batch(h.client, bound.binding, request, storage, nullFunction).status ==
	      cl::Status::InvalidArgument);
	CHECK(bt::batch(h.client, bound.binding, request, storage, freeCompletion, 0).status ==
	      cl::Status::InvalidArgument);
	CHECK(bt::batch(h.client, bound.binding, request, storage, freeCompletion, cl::maxTimeoutMs + 1)
	          .status == cl::Status::InvalidArgument);
	freeCalls = 0;
	CHECK(bt::batch(h.client, bound.binding, request, storage, freeCompletion));
	h.send(h.offered());
	CHECK(freeCalls == 1);
	Observed observed;
	CHECK(bt::batch(h.client, bound.binding, request, storage, FalseBool{&observed}));
	h.send(h.offered());
	CHECK(observed.calls == 1 && observed.value);
	CHECK(bt::batch(h.client, bound.binding, request, storage, MoveOnly{observed}));
	h.send(h.offered());
	CHECK(observed.calls == 2 && observed.value && !storage.busy());
	h.stop();

	Harness<> recovering;
	const auto stale =
	    bt::bindBatch(recovering.client, recovering.backend, recovering.capability, request);
	CHECK(bt::batch(
	    recovering.client, stale.binding, request, storage,
	    [&observed](const auto& result) noexcept {
		    observed.record(result);
	    },
	    50));
	const auto packet = recovering.offered();
	CHECK(recovering.client.prepareHandoff(packet.session, packet.requestId));
	recovering.client.invalidateAdmission();
	recovering.now = 50;
	recovering.backend.process(50);
	CHECK(recovering.backend.recoveryRequired() && storage.busy());
	CHECK(
	    bt::batch(recovering.client, stale.binding, request, storage, MoveOnly{observed}).status ==
	    cl::Status::RecoveryRequired);
	CHECK(observed.calls == 2 && recovering.client.pending() == 1);
	recovering.client.close();
	CHECK(
	    bt::batch(recovering.client, stale.binding, request, storage, MoveOnly{observed}).status ==
	    cl::Status::Closed);
	CHECK(observed.calls == 2 && recovering.client.pending() == 1);
	recovering.client.process(50);
	CHECK(observed.calls == 3 && observed.status == cl::Status::Timeout && !observed.item);
	CHECK(!storage.busy());
	recovering.stop();
}

int main()
{
	ordered<api::client::CodecPolicy>();
	ordered<static_batch_contracts::Codec>();
	zeroWire<api::client::CodecPolicy>();
	zeroWire<static_batch_contracts::Codec>();
	lifecycle();
	refusalAndReuse();
	malformedLast();
	ownershipReentry();
	codecReentry();
	bindingRefusals();
	sameFifoAndFailures();
	repeated<8, false>();
	repeated<32, false>();
	repeated<32, true>();
	callableKindsAndClosedRecovery();
	std::printf("Static Batch checks=%u failures=0\n", checks);
	return 0;
}
