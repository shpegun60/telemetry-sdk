/* Deterministic static ReadMany with Generated and independent handwritten Codec. MIT. */
#include "Fixture.hpp"

using namespace static_many_test;

template<class Codec>
static void mixedScalars() noexcept
{
	Harness<Codec> h;
	const auto binding = rm::bindReadMany(
	    h.client, h.backend, h.capability,
	    rm::fields(api::mixed::Bool, api::mixed::U8, api::mixed::S8, api::mixed::U16,
	               api::mixed::S16, api::mixed::U32, api::mixed::S32, api::mixed::U64,
	               api::mixed::S64, api::mixed::F32, api::mixed::F64, api::mixed::Enum,
	               api::mixed::Unavailable));
	CHECK(binding);
	Observed observed;
	CHECK(rm::readMany(h.client, binding.binding, [&observed](const auto& result) noexcept {
		observed.record(result);
	}));
	CHECK(observed.calls == 0 && native::owner.calls() == 0);
	const auto packet = h.offered();
	const auto request = ew::parse(packet.bytes);
	CHECK(request && request.header.operation == ew::Operation::ReadMany);
	const auto body = sw::parseRequest(request.payload);
	CHECK(body && body.count == 13);
	for (unsigned i = 0; i < 12; ++i)
		CHECK(body.ids[i] == (0x10000u | i));
	CHECK(body.ids[12] == 0x1000du);
	h.send(packet);
	CHECK(observed.calls == 1 && observed.status == cl::Status::Ok);
	CHECK(observed.valueStatus == rm::StaticValueStatus::Ok && observed.count == 13);
	constexpr std::array<std::uint64_t, 12> expected{1,
	                                                 UINT8_MAX,
	                                                 0x80,
	                                                 UINT16_MAX,
	                                                 0x8000,
	                                                 UINT32_MAX,
	                                                 0x80000000,
	                                                 UINT64_MAX,
	                                                 UINT64_C(0x8000000000000000),
	                                                 0xff812345u,
	                                                 UINT64_C(0x8000000000000000),
	                                                 0xff85};
	for (std::size_t i = 0; i < expected.size(); ++i) {
		CHECK(observed.ids[i] == (0x10000u | i));
		CHECK(observed.statuses[i] == sw::ItemStatus::Ok && observed.present[i]);
		CHECK(observed.values[i] == expected[i]);
		CHECK(native::owner.mixedCalls[i] == 1);
	}
	CHECK(observed.ids[12] == 0x1000du && !observed.present[12]);
	CHECK(observed.statuses[12] == sw::ItemStatus::Unavailable);
	CHECK(native::owner.calls() == 12);
	const auto saved = observed.payload;
	h.reply.fill(std::byte{0xa5});
	h.rx.fill(std::byte{0x5a});
	CHECK(observed.payload == saved); // The application kept copies, not callback views.
	h.stop();
}

template<std::size_t Count, class Codec>
static void arbitrarySelection() noexcept
{
	Harness<Codec> h;
	const auto binding = rm::bindReadMany(h.client, h.backend, h.capability,
	                                      selected(std::make_index_sequence<Count>{}));
	CHECK(binding);
	Observed observed;
	CHECK(rm::readMany(h.client, binding.binding, [&observed](const auto& result) noexcept {
		observed.record(result);
	}));
	const auto packet = h.offered();
	const auto request = ew::parse(packet.bytes);
	CHECK(request && request.header.expectedResponseBytes == 2 + 12 * Count);
	const auto parsed = sw::parseRequest(request.payload);
	CHECK(parsed && parsed.count == Count);
	for (std::size_t i = 0; i < Count; ++i) {
		const auto index = (i * 7 + 5) % 32;
		CHECK(parsed.ids[i] == scalarEndpoints[index].id);
		CHECK((parsed.ids[i] >> 16) == (index < 16 ? 2u : 3u));
	}
	h.send(packet);
	CHECK(observed.calls == 1 && observed.valueStatus == rm::StaticValueStatus::Ok);
	for (std::size_t i = 0; i < Count; ++i) {
		const auto index = (i * 7 + 5) % 32;
		CHECK(observed.ids[i] == scalarEndpoints[index].id);
		CHECK(observed.present[i] && observed.statuses[i] == sw::ItemStatus::Ok);
		CHECK(observed.values[i] == native::word(index));
		CHECK(native::owner.scalarCalls[index] == 1);
	}
	CHECK(native::owner.calls() == Count);
	h.stop();
}

static void sameFifo() noexcept
{
	Harness<> h;
	std::array<unsigned, 3> order{};
	unsigned completed = 0;
	const auto binding = rm::bindReadMany(h.client, h.backend, h.capability,
	                                      rm::fields(api::right::F31, api::left::F03));
	CHECK(binding);
	CHECK(h.client.read(api::ordinary::Value, [&](const auto& result) noexcept {
		CHECK(result.succeeded() && result.hasValue() && result.value() == 0xa1b2c3d4u);
		order[completed++] = 1;
	}));
	CHECK(rm::readMany(h.client, binding.binding, [&](const auto& result) noexcept {
		CHECK(result.operation.succeeded());
		CHECK(result.template get<0>().hasValue() &&
		      result.template get<0>().value() == native::word(31));
		CHECK(result.template get<1>().hasValue() &&
		      result.template get<1>().value() == native::word(3));
		order[completed++] = 2;
	}));
	CHECK(h.client.read(api::ordinary::Value, [&](const auto& result) noexcept {
		CHECK(result.succeeded() && result.hasValue() && result.value() == 0xa1b2c3d4u);
		order[completed++] = 3;
	}));
	CHECK(h.client.pending() == 3 && native::owner.calls() == 0);
	for (unsigned i = 0; i < 3; ++i) {
		auto packet = h.offered();
		CHECK(ew::parse(packet.bytes).header.operation ==
		      (i == 1 ? ew::Operation::ReadMany : ew::Operation::FieldRead));
		h.send(packet);
		CHECK(completed == i + 1);
	}
	CHECK((order == std::array<unsigned, 3>{1, 2, 3}));
	CHECK(native::owner.ordinaryCalls == 2 && native::owner.calls() == 4);
	h.stop();
}

static void refusals() noexcept
{
	Harness<> h;
	const auto two = rm::fields(api::left::F00, api::right::F17);
	auto capability = h.capability;
	++capability.session;
	CHECK(rm::bindReadMany(h.client, h.backend, capability, two).status == cl::Status::NotAdmitted);
	capability = h.capability;
	capability.fingerprint ^= 1;
	CHECK(rm::bindReadMany(h.client, h.backend, capability, two).status == cl::Status::NotAdmitted);
	capability = h.capability;
	capability.itemLimit = 1;
	CHECK(rm::bindReadMany(h.client, h.backend, capability, two).status ==
	      cl::Status::InvalidArgument);
	capability = h.capability;
	capability.requestLimit = 6;
	CHECK(rm::bindReadMany(h.client, h.backend, capability, two).status ==
	      cl::Status::BufferTooSmall);
	capability = h.capability;
	capability.replyLimit = 10;
	CHECK(rm::bindReadMany(h.client, h.backend, capability, two).status ==
	      cl::Status::BufferTooSmall);
	capability = h.capability;
	capability.session = 0;
	CHECK(rm::bindReadMany(h.client, h.backend, capability, two).status ==
	      cl::Status::InvalidArgument);
	CHECK(rm::bindReadMany(h.client, h.backend, h.capability,
	                       rm::fields(api::left::F00, api::left::F00))
	          .status == cl::Status::InvalidArgument);
	auto metadata = api::left::F00;
	metadata.capabilities = 0;
	CHECK(rm::bindReadMany(h.client, h.backend, h.capability, rm::fields(metadata)).status ==
	      cl::Status::InvalidArgument);
	metadata = api::left::F00;
	metadata.valueBytes = 8;
	CHECK(rm::bindReadMany(h.client, h.backend, h.capability, rm::fields(metadata)).status ==
	      cl::Status::InvalidArgument);
	metadata = api::left::F00;
	metadata.fingerprint ^= 1;
	CHECK(rm::bindReadMany(h.client, h.backend, h.capability, rm::fields(metadata)).status ==
	      cl::Status::NotAdmitted);
	cl::Backend& erased = h.backend;
	CHECK(rm::bindReadMany(h.client, erased, h.capability, two).status == cl::Status::Unsupported);
	std::array<std::byte, 256> tx{}, rx{};
	en::EncodedBackend other{tx, rx, h.clock, 30000, true};
	CHECK(rm::bindReadMany(h.client, other, h.capability, two).status == cl::Status::Unsupported);
	en::EncodedBackend disabled{tx, rx, h.clock};
	cl::DeviceClient<api::client::CodecPolicy> disabledClient{disabled, h.clock};
	CHECK(disabledClient.admitDescriptor(api::schema::descriptorHeader, 17) == cl::Status::Ok);
	CHECK(rm::bindReadMany(disabledClient, disabled, h.capability, two).status ==
	      cl::Status::Unsupported);
	CHECK(h.client.pending() == 0 && !h.client.nextPacket() && native::owner.calls() == 0);
	disabledClient.close();
	disabledClient.process(0);
	h.stop();
}

static void lifecycle() noexcept
{
	for (bool cancel : {false, true}) {
		for (unsigned phase = 0; phase < 3; ++phase) {
			Harness<> h;
			const auto binding = rm::bindReadMany(h.client, h.backend, h.capability,
			                                      rm::fields(api::left::F00, api::right::F17));
			CHECK(binding);
			Observed observed;
			const auto submitted = rm::readMany(
			    h.client, binding.binding,
			    [&observed](const auto& result) noexcept {
				    observed.record(result);
			    },
			    50);
			CHECK(submitted);
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
			CHECK(observed.calls == 1);
			CHECK(observed.status == (cancel ? cl::Status::Cancelled : cl::Status::Timeout));
			CHECK(observed.status != cl::Status::OutcomeUnknown);
			CHECK(h.client.recoveryRequired() == (phase == 2));
			CHECK(h.client.pending() == 0 && !h.client.nextPacket());
			CHECK(!observed.present[0] && !observed.present[1]);
			CHECK(native::owner.calls() == 0);
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
	for (unsigned phase = 0; phase < 3; ++phase) {
		Harness<> h;
		const auto binding =
		    rm::bindReadMany(h.client, h.backend, h.capability, rm::fields(api::left::F00));
		Observed observed;
		CHECK(rm::readMany(
		    h.client, binding.binding,
		    [&observed](const auto& result) noexcept {
			    observed.record(result);
		    },
		    50));
		const auto packet = h.offered();
		h.handoff(packet);
		const auto size = h.response(packet);
		h.now = phase == 0 ? 49 : phase == 1 ? 50 : 51;
		CHECK(h.client.onReply(cl::Input{h.reply}.first(size), packet.session, packet.requestId) ==
		      (phase == 0));
		h.client.process(h.now);
		CHECK(observed.calls == 1);
		CHECK(observed.status == (phase == 0 ? cl::Status::Ok : cl::Status::Timeout));
		CHECK(observed.present[0] == (phase == 0));
		h.stop();
	}
}

// Every changed body remains a valid TEQ1 envelope; refusal is at the typed
// ReadMany boundary. A valid earlier U32 must never leak before a bad later Bool.
static void malformed() noexcept
{
	for (unsigned mutation = 0; mutation < 10; ++mutation) {
		Harness<> h;
		const auto binding = rm::bindReadMany(h.client, h.backend, h.capability,
		                                      rm::fields(api::left::F00, api::mixed::Bool));
		Observed observed;
		CHECK(rm::readMany(h.client, binding.binding, [&observed](const auto& result) noexcept {
			observed.record(result);
		}));
		const auto packet = h.offered();
		h.handoff(packet);
		const auto bytes = h.response(packet);
		auto parsed = ew::parse(cl::Input{h.reply}.first(bytes));
		CHECK(parsed && parsed.payload.size() == 23);
		std::array<std::byte, 64> body{};
		std::copy(parsed.payload.begin(), parsed.payload.end(), body.begin());
		std::size_t count = parsed.payload.size();
		switch (mutation) {
			case 0:
				body[22] = std::byte{2};
				break; // Noncanonical later Bool.
			case 1:
				body[0] = std::byte{1};
				break; // Count.
			case 2:
				body[2] ^= std::byte{1};
				break; // First ID.
			case 3:
				body[14] ^= std::byte{1};
				break; // Later ID/order.
			case 4:
				body[18] = std::byte{5};
				break; // Unknown item status.
			case 5:
				body[19] = std::byte{1};
				break; // Reserved byte.
			case 6:
				body[20] = std::byte{2};
				break; // Wrong later width.
			case 7:
				--count;
				break; // Missing later value.
			case 8:    // A no-value error followed by an extra byte, within the envelope budget.
				body[18] = std::byte{1};
				body[20] = std::byte{0};
				body[22] = std::byte{0};
				break;
			case 9:
				body[18] = std::byte{1};
				break; // Error with value bytes.
		}
		auto header = parsed.header;
		const auto written = ew::write(h.reply, header, cl::Input{body}.first(count));
		CHECK(written);
		h.deliver(packet, written.written);
		CHECK(observed.calls == 1 && observed.status == cl::Status::Ok);
		CHECK(observed.valueStatus == rm::StaticValueStatus::InvalidPayload);
		CHECK(!observed.present[0] && !observed.present[1]);
		CHECK(h.client.pending() == 0 && !h.client.recoveryRequired());
		h.stop();
	}
}

static void perItemStatuses() noexcept
{
	for (unsigned status = 1; status <= 4; ++status) {
		Harness<> h;
		const auto binding = rm::bindReadMany(h.client, h.backend, h.capability,
		                                      rm::fields(api::left::F00, api::mixed::Bool));
		Observed observed;
		CHECK(rm::readMany(h.client, binding.binding, [&observed](const auto& result) noexcept {
			observed.record(result);
		}));
		const auto packet = h.offered();
		h.handoff(packet);
		const auto bytes = h.response(packet);
		const auto parsed = ew::parse(cl::Input{h.reply}.first(bytes));
		std::array<std::byte, 22> body{};
		std::copy_n(parsed.payload.begin(), body.size(), body.begin());
		body[18] = std::byte(status);
		body[20] = std::byte{0};
		const auto written = ew::write(h.reply, parsed.header, body);
		CHECK(written);
		h.deliver(packet, written.written);
		CHECK(observed.calls == 1 && observed.status == cl::Status::Ok);
		CHECK(observed.valueStatus == rm::StaticValueStatus::Ok);
		CHECK(observed.present[0] && observed.values[0] == native::word(0));
		CHECK(!observed.present[1] && observed.statuses[1] == static_cast<sw::ItemStatus>(status));
		h.stop();
	}
}

static void boundedSubmission() noexcept
{
	Harness<api::client::CodecPolicy, 1> h;
	const auto binding =
	    rm::bindReadMany(h.client, h.backend, h.capability, rm::fields(api::left::F00));
	Observed accepted, rejected;
	CHECK(rm::readMany(h.client, binding.binding, [&accepted](const auto& result) noexcept {
		accepted.record(result);
	}));
	CHECK(rm::readMany(h.client, binding.binding, [&rejected](const auto& result) noexcept {
		      rejected.record(result);
	      }).status == cl::Status::QueueFull);
	CHECK(h.client.pending() == 1 && accepted.calls == 0 && rejected.calls == 0);
	CHECK(native::owner.calls() == 0);
	h.send(h.offered());
	CHECK(accepted.calls == 1 && rejected.calls == 0);
	h.stop();

	Harness<api::client::CodecPolicy, 2, 5> small;
	const auto smallBinding =
	    rm::bindReadMany(small.client, small.backend, small.capability, rm::fields(api::left::F00));
	CHECK(smallBinding);
	CHECK(
	    rm::readMany(small.client, smallBinding.binding, [&rejected](const auto& result) noexcept {
		    rejected.record(result);
	    }).status == cl::Status::PayloadFull);
	CHECK(small.client.pending() == 0 && !small.client.nextPacket() && rejected.calls == 0);
	CHECK(native::owner.calls() == 0);
	small.stop();
}

static void bindingOwnershipAndSessions() noexcept
{
	Harness<> h;
	auto source = rm::fields(api::left::F00, api::right::F31);
	auto binding = rm::bindReadMany(h.client, h.backend, h.capability, source);
	CHECK(binding);
	std::get<0>(source.endpoints).id = api::left::F09.id;
	Observed observed;
	CHECK(rm::readMany(h.client, binding.binding, [&observed](const auto& result) noexcept {
		observed.record(result);
	}));
	binding = {}; // Accepted completion owns its normalized binding.
	h.send(h.offered());
	CHECK(observed.ids[0] == api::left::F00.id && observed.values[0] == native::word(0));
	CHECK(observed.ids[1] == api::right::F31.id && observed.values[1] == native::word(31));

	const auto old =
	    rm::bindReadMany(h.client, h.backend, h.capability, rm::fields(api::left::F00));
	Harness<> foreign;
	CHECK(rm::readMany(foreign.client, old.binding, [&observed](const auto& result) noexcept {
		      observed.record(result);
	      }).status == cl::Status::NotAdmitted);
	CHECK(observed.calls == 1 && foreign.client.pending() == 0);
	foreign.stop();
	h.client.invalidateAdmission();
	CHECK(h.client.submissionStatus() == cl::Status::NotAdmitted);
	CHECK(rm::readMany(h.client, old.binding, [&observed](const auto& result) noexcept {
		      observed.record(result);
	      }).status == cl::Status::NotAdmitted);
	CHECK(h.client.admitDescriptor(api::schema::descriptorHeader, 18) == cl::Status::Ok);
	CHECK(rm::readMany(h.client, old.binding, [&observed](const auto& result) noexcept {
		      observed.record(result);
	      }).status == cl::Status::NotAdmitted);
	h.capability.session = 18;
	const auto current =
	    rm::bindReadMany(h.client, h.backend, h.capability, rm::fields(api::left::F00));
	CHECK(current);
	CHECK(rm::readMany(h.client, current.binding, [&observed](const auto& result) noexcept {
		observed.record(result);
	}));
	h.send(h.offered());
	CHECK(observed.calls == 2 && observed.valueStatus == rm::StaticValueStatus::Ok);
	h.stop();
	CHECK(rm::readMany(h.client, current.binding, [&observed](const auto& result) noexcept {
		      observed.record(result);
	      }).status == cl::Status::Closed);
}

struct HookCodec : static_many_contracts::Codec {
	static inline void (*hook)() noexcept = nullptr;

	template<class T>
	static std::uint8_t decode(std::span<const std::byte> input, T& value) noexcept
	{
		if (const auto selectedHook = std::exchange(hook, nullptr))
			selectedHook();
		return Codec::decode(input, value);
	}
};

static Harness<HookCodec>* hookOwner = nullptr;

static void handwrittenAndDecodeReentry() noexcept
{
	Harness<HookCodec> h;
	const static_many_contracts::Field<std::uint32_t> a{api::left::F00.id};
	const static_many_contracts::Field<bool> b{api::mixed::Bool.id};
	const auto binding = rm::bindReadMany(h.client, h.backend, h.capability, rm::fields(a, b));
	CHECK(binding);
	for (bool revoke : {false, true}) {
		Observed observed;
		CHECK(rm::readMany(h.client, binding.binding, [&observed](const auto& result) noexcept {
			observed.record(result);
		}));
		hookOwner = &h;
		if (revoke)
			HookCodec::hook = +[]() noexcept {
				hookOwner->client.invalidateAdmission();
			};
		h.send(h.offered());
		CHECK(observed.calls == 1 && observed.status == cl::Status::Ok);
		CHECK(observed.valueStatus ==
		      (revoke ? rm::StaticValueStatus::Stale : rm::StaticValueStatus::Ok));
		CHECK(observed.present[0] == !revoke && observed.present[1] == !revoke);
		if (!revoke)
			CHECK(observed.values[0] == native::word(0) && observed.values[1] == 1);
	}
	hookOwner = nullptr;
	h.stop();
}

struct FalseBool {
	Observed* observed;

	explicit operator bool() const noexcept
	{
		return false;
	}

	template<class T>
	void operator()(const T& value) noexcept
	{
		observed->record(value);
	}
};

struct MoveOnly {
	Observed* observed;

	explicit MoveOnly(Observed& target) noexcept : observed(&target)
	{}

	MoveOnly(const MoveOnly&) = delete;

	MoveOnly(MoveOnly&& other) noexcept : observed(std::exchange(other.observed, nullptr))
	{}

	template<class T>
	void operator()(const T& value) noexcept
	{
		CHECK(observed);
		observed->record(value);
	}
};

struct OwnershipProbe {
	Harness<>* owner;
	Observed* observed;
	unsigned moves = 0, destructors = 0, triggerMove = 1;
};

struct InvalidateOnMove {
	OwnershipProbe* probe;

	explicit InvalidateOnMove(OwnershipProbe& value) noexcept : probe(&value)
	{}

	InvalidateOnMove(const InvalidateOnMove&) = delete;

	InvalidateOnMove(InvalidateOnMove&& other) noexcept : probe(other.probe)
	{
		if (++probe->moves == probe->triggerMove)
			probe->owner->client.invalidateAdmission();
	}

	~InvalidateOnMove() noexcept
	{
		++probe->destructors;
	}

	template<class T>
	void operator()(const T& value) noexcept
	{
		probe->observed->record(value);
	}
};

static void callbacksAndReentry() noexcept
{
	Harness<> h;
	const auto binding =
	    rm::bindReadMany(h.client, h.backend, h.capability, rm::fields(api::left::F00));
	using Result = rm::StaticReadManyResult<std::uint32_t>;
	void (*nullFunction)(const Result&) noexcept = nullptr;
	CHECK(rm::readMany(h.client, binding.binding, nullFunction).status ==
	      cl::Status::InvalidArgument);
	CHECK(rm::readMany(
	          h.client, binding.binding,
	          [](const auto&) noexcept {
	          },
	          0)
	          .status == cl::Status::InvalidArgument);
	CHECK(rm::readMany(
	          h.client, binding.binding,
	          [](const auto&) noexcept {
	          },
	          cl::maxTimeoutMs + 1)
	          .status == cl::Status::InvalidArgument);
	Observed observed;
	CHECK(rm::readMany(h.client, binding.binding, FalseBool{&observed}));
	h.send(h.offered());
	CHECK(observed.calls == 1);
	CHECK(rm::readMany(h.client, binding.binding, MoveOnly{observed}));
	h.send(h.offered());
	CHECK(observed.calls == 2);
	CHECK(rm::readMany(h.client, binding.binding, [&](const Result& result) noexcept {
		observed.record(result);
		CHECK(!h.client.nextPacket()); // No hidden progress inside a completion.
		CHECK(rm::readMany(h.client, binding.binding, MoveOnly{observed}));
		CHECK(h.client.pending() == 2); // Completing record plus one queued request.
	}));
	h.send(h.offered());
	CHECK(observed.calls == 3 && h.client.pending() == 1);
	h.send(h.offered());
	CHECK(observed.calls == 4 && h.client.pending() == 0);
	h.stop();

	for (unsigned trigger = 1; trigger <= 3; ++trigger) {
		Harness<> changing;
		const auto selectedBinding = rm::bindReadMany(
		    changing.client, changing.backend, changing.capability, rm::fields(api::left::F00));
		Observed refused;
		OwnershipProbe probe{&changing, &refused, 0, 0, trigger};
		{
			InvalidateOnMove completion{probe};
			CHECK(rm::readMany(changing.client, selectedBinding.binding, std::move(completion))
			          .status == cl::Status::NotAdmitted);
		}
		CHECK(probe.moves >= trigger && probe.destructors > 0);
		CHECK(refused.calls == 0 && changing.client.pending() == 0);
		CHECK(!changing.client.nextPacket() && native::owner.calls() == 0);
		changing.stop();
	}
}

template<class Codec>
static void floatingBits() noexcept
{
	Harness<Codec> h;
	const auto binding = rm::bindReadMany(h.client, h.backend, h.capability,
	                                      rm::fields(api::mixed::F32, api::mixed::F64));
	CHECK(binding);
	constexpr std::array<std::uint32_t, 6> f32{0x7fc12345u, 0xffc23456u, 0x7f812345u,
	                                           0xff823456u, 0u,          0x80000000u};
	constexpr std::array<std::uint64_t, 6> f64{UINT64_C(0x7ff8123456789abc),
	                                           UINT64_C(0xfff823456789abcd),
	                                           UINT64_C(0x7ff0123456789abc),
	                                           UINT64_C(0xfff023456789abcd),
	                                           0,
	                                           UINT64_C(0x8000000000000000)};
	for (std::size_t i = 0; i < f32.size(); ++i) {
		native::owner.f32Bits = f32[i];
		native::owner.f64Bits = f64[i];
		Observed observed;
		CHECK(rm::readMany(h.client, binding.binding, [&observed](const auto& result) noexcept {
			observed.record(result);
		}));
		h.send(h.offered());
		CHECK(observed.calls == 1 && observed.valueStatus == rm::StaticValueStatus::Ok);
		CHECK(observed.present[0] && observed.values[0] == f32[i]);
		CHECK(observed.present[1] && observed.values[1] == f64[i]);
	}
	h.stop();
}

struct DestructorProbe {
	Harness<>* owner;
	rm::StaticReadManyBinding<std::uint32_t> binding;
	Observed* observed;
	unsigned attempts = 0;
};

struct SubmitOnDestroy {
	DestructorProbe* probe;

	explicit SubmitOnDestroy(DestructorProbe& value) noexcept : probe(&value)
	{}

	SubmitOnDestroy(const SubmitOnDestroy&) = delete;

	SubmitOnDestroy(SubmitOnDestroy&& other) noexcept : probe(std::exchange(other.probe, nullptr))
	{}

	~SubmitOnDestroy() noexcept
	{
		if (!probe)
			return;
		++probe->attempts;
		CHECK(rm::readMany(probe->owner->client, probe->binding, MoveOnly{*probe->observed}));
	}

	template<class T>
	void operator()(const T& value) noexcept
	{
		probe->observed->record(value);
	}
};

static void destructorReentry() noexcept
{
	Harness<> h;
	const auto binding =
	    rm::bindReadMany(h.client, h.backend, h.capability, rm::fields(api::left::F00));
	Observed observed;
	DestructorProbe probe{&h, binding.binding, &observed};
	CHECK(rm::readMany(h.client, binding.binding, SubmitOnDestroy{probe}));
	CHECK(probe.attempts == 0);
	h.send(h.offered());
	CHECK(probe.attempts == 1 && observed.calls == 1 && h.client.pending() == 1);
	h.send(h.offered());
	CHECK(observed.calls == 2 && h.client.pending() == 0);
	h.stop();
}

struct ReentrantField {
	static constexpr unsigned category = 1;
	using Value = std::uint32_t;
	std::uint32_t id = api::left::F00.id;
	Harness<>* owner;
	bool close;

	bool readable() const noexcept
	{
		if (close)
			owner->client.close();
		else
			owner->client.invalidateAdmission();
		return true;
	}
};

static void bindingReentry() noexcept
{
	for (bool close : {false, true}) {
		Harness<> h;
		const auto bound =
		    rm::bindReadMany(h.client, h.backend, h.capability,
		                     rm::fields(ReentrantField{api::left::F00.id, &h, close}));
		CHECK(!bound);
		CHECK(bound.status == (close ? cl::Status::Closed : cl::Status::NotAdmitted));
		CHECK(h.client.pending() == 0 && native::owner.calls() == 0);
		CHECK(!h.client.nextPacket());
		h.stop();
	}
}

static void pureRecoveryGate() noexcept
{
	Harness<> h;
	const auto selection = rm::fields(api::left::F00);
	const auto binding = rm::bindReadMany(h.client, h.backend, h.capability, selection);
	Observed observed;
	CHECK(rm::readMany(
	    h.client, binding.binding,
	    [&observed](const auto& result) noexcept {
		    observed.record(result);
	    },
	    50));
	const auto packet = h.offered();
	CHECK(h.client.prepareHandoff(packet.session, packet.requestId));
	h.client.invalidateAdmission(); // FIFO's stored admission state is NotAdmitted.
	CHECK(h.client.submissionStatus() == cl::Status::NotAdmitted);
	h.now = 50;
	h.backend.process(h.now); // Native backend timeout; core process has not synchronized it.
	CHECK(h.backend.recoveryRequired() && h.backend.completed());
	CHECK(h.client.submissionStatus() == cl::Status::RecoveryRequired);
	CHECK(rm::bindReadMany(h.client, h.backend, h.capability, selection).status ==
	      cl::Status::RecoveryRequired);
	CHECK(rm::readMany(h.client, binding.binding, MoveOnly{observed}).status ==
	      cl::Status::RecoveryRequired);
	CHECK(h.client.pending() == 1 && observed.calls == 0 && native::owner.calls() == 0);
	h.client.close();
	CHECK(h.backend.recoveryRequired() && h.client.submissionStatus() == cl::Status::Closed);
	CHECK(rm::bindReadMany(h.client, h.backend, h.capability, selection).status ==
	      cl::Status::Closed);
	CHECK(rm::readMany(h.client, binding.binding, MoveOnly{observed}).status == cl::Status::Closed);
	CHECK(h.client.pending() == 1 && observed.calls == 0 && native::owner.calls() == 0);
	h.client.process(h.now);
	CHECK(observed.calls == 1 && observed.status == cl::Status::Timeout);
	CHECK(!observed.present[0] && h.client.pending() == 0);
	h.stop();
}

int main()
{
	mixedScalars<api::client::CodecPolicy>();
	mixedScalars<static_many_contracts::Codec>();
	arbitrarySelection<1, api::client::CodecPolicy>();
	arbitrarySelection<8, api::client::CodecPolicy>();
	arbitrarySelection<12, api::client::CodecPolicy>();
	arbitrarySelection<16, api::client::CodecPolicy>();
	arbitrarySelection<32, api::client::CodecPolicy>();
	arbitrarySelection<32, static_many_contracts::Codec>();
	sameFifo();
	refusals();
	lifecycle();
	malformed();
	perItemStatuses();
	boundedSubmission();
	bindingOwnershipAndSessions();
	handwrittenAndDecodeReentry();
	callbacksAndReentry();
	floatingBits<api::client::CodecPolicy>();
	floatingBits<static_many_contracts::Codec>();
	destructorReentry();
	bindingReentry();
	pureRecoveryGate();
	std::printf("Static ReadMany checks=%u failures=0\n", checks);
	return 0;
}
