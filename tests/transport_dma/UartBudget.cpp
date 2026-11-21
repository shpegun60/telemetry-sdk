/* Loss-free 8N1 UART budget with real Encoded dispatch and bounded simulated I/O. MIT. */
#include <client/DeviceClient.hpp>
#include <integration/encoded/RetryingBackend.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <algorithm>
#include <array>
#include <cstdio>

namespace tc = telemetry::client;
namespace enc = telemetry::integration::encoded;
namespace ts = telemetry;
static unsigned checks = 0, failures = 0;

static void check(bool value, unsigned line) noexcept
{
	++checks;
	if (!value) {
		++failures;
		std::fprintf(stderr, "uart_budget line=%u failed\n", line);
	}
}

#define CHECK(value) check(bool(value), __LINE__)

namespace uart_fixture {
constexpr std::uint32_t baud = 115200, bitsPerByte = 10;
constexpr std::uint32_t serverMs = 7, actualJitterMs = 11, jitterBudgetMs = 20;
constexpr std::size_t payloadBytes = 2048, frameCapacity = 32 + payloadBytes;

constexpr std::uint32_t wireMs(std::size_t bytes) noexcept
{
	return static_cast<std::uint32_t>((bytes * bitsPerByte * 1000 + baud - 1) / baud);
}

static_assert(wireMs(frameCapacity) == 181);

struct Large {
	std::array<std::uint8_t, payloadBytes> bytes;
};

struct Small {
	std::uint32_t seed;
};

static_assert(ts::wireSize<Large> == payloadBytes && ts::wireSize<Small> == 4);

static Large pattern(std::uint8_t seed) noexcept
{
	Large value{};
	for (std::size_t i = 0; i < value.bytes.size(); ++i)
		value.bytes[i] = static_cast<std::uint8_t>(i ^ seed);
	return value;
}

struct Device {
	unsigned executions = 0;

	ts::ServiceResult<Large> echo(const Large& request) noexcept
	{
		++executions;
		return ts::ServiceResult<Large>::successFrom([&]() noexcept {
			return request;
		});
	}

	ts::ServiceResult<Large> read(const Small& request) noexcept
	{
		++executions;
		return ts::ServiceResult<Large>::successFrom([&]() noexcept {
			return pattern(static_cast<std::uint8_t>(request.seed));
		});
	}
};

inline Device device;
inline constexpr ts::Model model{ts::group("uart", ts::service<&Device::echo>("Echo", device),
                                           ts::service<&Device::read>("Read", device))};
inline constexpr auto view = model.view();

struct Policy {
	inline static std::array<std::byte, 64> observedHeader{};
	static constexpr std::size_t scratchBytes = 1, scratchAlign = 1;

	static tc::Input descriptorHeader() noexcept
	{
		return observedHeader;
	}
};

using Client = tc::DeviceClient<Policy, 2, payloadBytes, 64>;

// This port models line timing, not HAL. Every frame is eventually delivered.
// Fixed independent TX/RX storage deliberately leaves H753Port's 128B limit alone.
struct Harness {
	struct Flight {
		std::array<std::byte, frameCapacity> request{}, response{};
		std::size_t requestBytes = 0, responseBytes = 0;
		std::uint32_t session = 0, requestId = 0;
		std::uint32_t sentAt = 0, fullTxAt = 0, serverAt = 0, fullRxAt = 0, deliveredAt = 0;
		bool txComplete = false, dispatched = false, delivered = false;
	};

	std::uint32_t now = 0;
	std::array<std::byte, frameCapacity> backendTx{}, portTx{};
	std::array<std::byte, payloadBytes> result{}, requestPayload{};
	std::array<std::byte, 8192> scratch{};
	ts::Workspace workspace{scratch};
	tc::Clock clock{this, +[](void* self) noexcept {
		                return static_cast<Harness*>(self)->now;
	                }};
	enc::RetryingEncodedBackend backend;
	Client client{backend, clock};
	std::array<Flight, 2> flights{};
	unsigned sends = 0, callbacks = 0, blocked = 0, acceptedReplies = 0, ignoredReplies = 0;
	std::uint32_t replyTimeoutMs, jitterMs, callbackAt = 0, rxWireTail = 0;
	std::size_t payloadSize = 0;
	tc::Status status = tc::Status::Busy;
	Large observed{};
	bool txBusy = false, callbackDuringTx = false;

	explicit Harness(std::uint32_t timeout, std::uint32_t jitter = actualJitterMs)
	    : backend(backendTx, result, clock, {.replyTimeoutMs = timeout, .maxRetries = 1}),
	      replyTimeoutMs(timeout), jitterMs(jitter)
	{
		device = {};
		const ts::descriptor::Descriptor descriptor{model};
		CHECK(descriptor.read(0, Policy::observedHeader).written == 64);
		CHECK(client.admitDescriptor(Policy::observedHeader, 1) == tc::Status::Ok);
	}

	~Harness()
	{
		client.close();
		client.process(now);
		CHECK(client.drained());
		CHECK(!txBusy);
	}

	void submit(bool largeRequest)
	{
		payloadSize = largeRequest ? payloadBytes : ts::wireSize<Small>;
		const auto request = tc::Output{requestPayload}.first(payloadSize);
		if (largeRequest)
			CHECK(ts::encode(pattern(42), request) == ts::CodecStatus::Ok);
		else
			CHECK(ts::encode(Small{42}, request) == ts::CodecStatus::Ok);
		CHECK(client.submitEncoded(
		    {tc::Operation::Service, largeRequest ? 0u : 1u, request, payloadBytes},
		    [this](const tc::ResultView& value) noexcept {
			    ++callbacks;
			    callbackAt = now;
			    callbackDuringTx = txBusy;
			    status = value.status;
			    if (value.status == tc::Status::Ok) {
				    CHECK(value.dispatch == 0 && value.endpointStatus == 0);
				    CHECK(ts::decode(value.payload, observed) == ts::CodecStatus::Ok);
			    }
		    },
		    1000));
	}

	void sendOffered()
	{
		const auto packet = client.nextPacket();
		if (!packet)
			return;
		if (txBusy) {
			++blocked; // Permission is not requested while a DMA-style borrow is live.
			return;
		}
		CHECK(sends < flights.size());
		if (sends >= flights.size())
			return;
		CHECK(packet.bytes.size() <= portTx.size());
		std::copy(packet.bytes.begin(), packet.bytes.end(), portTx.begin());
		CHECK(client.prepareHandoff(packet.session, packet.requestId));
		auto& flight = flights[sends];
		flight.requestBytes = packet.bytes.size();
		flight.session = packet.session;
		flight.requestId = packet.requestId;
		flight.sentAt = now;
		flight.fullTxAt = now + wireMs(packet.bytes.size());
		txBusy = true;
		++sends;
		// Acceptance is at DMA-style start. Full byte delivery is a later event.
		CHECK(client.onSendAccepted(packet.session, packet.requestId));
	}

	void step(std::uint32_t tick)
	{
		now = tick;
		if (txBusy) {
			auto& flight = flights[sends - 1];
			if (now == flight.fullTxAt) {
				// The peer cannot inspect a partial request. Port storage is retained
				// until this complete-frame arrival and then copied into peer storage.
				std::copy_n(portTx.begin(), flight.requestBytes, flight.request.begin());
				flight.txComplete = true;
				flight.serverAt = now + serverMs;
				txBusy = false;
			}
		}
		for (unsigned i = 0; i < sends; ++i) {
			auto& flight = flights[i];
			if (flight.txComplete && !flight.dispatched && now == flight.serverAt) {
				const auto reply = enc::dispatchEncoded(
				    view, workspace, 1, tc::Input{flight.request}.first(flight.requestBytes),
				    flight.response);
				CHECK(reply.status == enc::EncodedDispatchStatus::Ok);
				CHECK(reply.written == frameCapacity);
				flight.responseBytes = reply.written;
				flight.dispatched = true;
				const auto rxStart = std::max(now, rxWireTail);
				flight.fullRxAt = rxStart + wireMs(reply.written);
				rxWireTail = flight.fullRxAt; // Responses share one non-overlapping RX wire.
				flight.deliveredAt = flight.fullRxAt + jitterMs;
			}
			if (flight.dispatched && !flight.delivered && now == flight.deliveredAt) {
				flight.delivered = true;
				if (client.onReply(tc::Input{flight.response}.first(flight.responseBytes),
				                   flight.session, flight.requestId))
					++acceptedReplies;
				else
					++ignoredReplies;
			}
		}
		client.process(now);
		sendOffered();
	}

	void run()
	{
		for (std::uint32_t tick = 0; tick <= 800; ++tick)
			step(tick);
		CHECK(!txBusy && callbacks == 1);
		for (unsigned i = 0; i < sends; ++i)
			CHECK(flights[i].txComplete && flights[i].dispatched && flights[i].delivered);
		CHECK(device.executions == sends); // No loss and no server duplicate suppression.
		if (sends == 2) {
			CHECK(flights[0].session == flights[1].session);
			CHECK(flights[0].requestId == flights[1].requestId);
			CHECK(flights[0].requestBytes == flights[1].requestBytes);
			CHECK(std::equal(flights[0].request.begin(),
			                 flights[0].request.begin() + flights[0].requestBytes,
			                 flights[1].request.begin()));
		}
	}

	void print(const char* name) const
	{
		std::printf("uart_budget_timeline {\"scenario\":\"%s\",\"baud\":%u,\"bits_per_byte\":%u,"
		            "\"reply_timeout_ms\":%u,\"request_bytes\":%zu,\"response_bytes\":%zu,"
		            "\"tx_ms\":%u,\"rx_ms\":%u,\"server_ms\":%u,\"actual_jitter_ms\":%u,"
		            "\"send_at_ms\":[",
		            name, baud, bitsPerByte, replyTimeoutMs, payloadSize + 32, frameCapacity,
		            wireMs(payloadSize + 32), wireMs(frameCapacity), serverMs, jitterMs);
		for (unsigned i = 0; i < sends; ++i)
			std::printf("%s%u", i ? "," : "", flights[i].sentAt);
		std::printf(
		    "],\"first_tx_complete_ms\":%u,\"first_server_ms\":%u,\"first_rx_complete_ms\":%u,"
		    "\"first_reply_delivery_ms\":%u,\"callback_ms\":%u,\"status\":%u,\"executions\":%u,"
		    "\"blocked_while_tx\":%u,\"accepted_replies\":%u,\"ignored_replies\":%u}\n",
		    flights[0].fullTxAt, flights[0].serverAt, flights[0].fullRxAt, flights[0].deliveredAt,
		    callbackAt, static_cast<unsigned>(status), device.executions, blocked, acceptedReplies,
		    ignoredReplies);
	}
};

static void scenario(const char* name, bool large, std::uint32_t timeout, bool fits,
                     std::uint32_t jitter = actualJitterMs)
{
	Harness h{timeout, jitter};
	h.submit(large);
	h.run();
	const auto tx = wireMs((large ? payloadBytes : ts::wireSize<Small>)+32);
	CHECK(h.flights[0].sentAt == 0 && h.flights[0].fullTxAt == tx);
	CHECK(h.flights[0].serverAt == tx + serverMs);
	CHECK(h.flights[0].fullRxAt == tx + serverMs + wireMs(frameCapacity));
	CHECK(h.flights[0].deliveredAt == tx + serverMs + wireMs(frameCapacity) + jitter);
	if (fits) {
		CHECK(h.sends == 1 && h.blocked == 0);
		CHECK(h.status == tc::Status::Ok && !h.client.recoveryRequired());
		CHECK(h.callbackAt == h.flights[0].deliveredAt && !h.callbackDuringTx);
		CHECK(h.acceptedReplies == 1 && h.ignoredReplies == 0);
		CHECK(h.observed.bytes == pattern(42).bytes);
	} else {
		CHECK(h.sends == 2);
		CHECK(h.flights[1].sentAt == std::max(tx, timeout));
		CHECK(h.status == tc::Status::OutcomeUnknown && h.client.recoveryRequired());
		CHECK(h.callbackAt == h.flights[1].sentAt + timeout);
		CHECK(h.callbackAt < h.flights[0].deliveredAt);
		CHECK(h.acceptedReplies == 0 && h.ignoredReplies == 2);
		if (large)
			CHECK(h.blocked > 0 && h.callbackDuringTx);
		CHECK(!h.client.nextPacket());
	}
	h.print(name);
}
} // namespace uart_fixture

int main()
{
	using namespace uart_fixture;
	constexpr auto fullBudget =
	    wireMs(frameCapacity) + serverMs + wireMs(frameCapacity) + jitterBudgetMs;
	static_assert(fullBudget == 389);
	scenario("long-default-100", true, 100, false);
	scenario("long-budget-389", true, fullBudget, true);
	scenario("long-profile-500", true, 500, true);
	scenario("long-profile-500-jitter-20", true, 500, true, jitterBudgetMs);
	constexpr auto smallTx = wireMs(32 + ts::wireSize<Small>);
	static_assert(smallTx == 4);
	constexpr auto txOnlyBudget = smallTx + serverMs + jitterBudgetMs;
	constexpr auto slowRxBudget = txOnlyBudget + wireMs(frameCapacity);
	static_assert(txOnlyBudget == 31 && slowRxBudget == 212);
	scenario("slow-rx-tx-only-31", false, txOnlyBudget, false);
	scenario("slow-rx-default-100", false, 100, false);
	scenario("slow-rx-budget-212", false, slowRxBudget, true);
	std::printf("transport_dma_uart checks=%u failures=%u\n", checks, failures);
	return failures ? 1 : 0;
}
