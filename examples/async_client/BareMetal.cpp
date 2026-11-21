/* Generated SDK client in a cooperative board loop. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
#include <device_api/ClientBindings.hpp>
#if defined(TELEMETRY_EXAMPLE_RETRY_ENCODED)
#include <integration/encoded/RetryingBackend.hpp>
#else
#include <integration/encoded/Backend.hpp>
#endif
#include <algorithm>

namespace cl = telemetry::client;
namespace api = device_api;
// Implement these thin functions in the board port. They run in the main loop.
// RX IRQs publish events; callbacks and client methods never run in an IRQ.
extern void board_init() noexcept;
extern std::uint32_t board_millis() noexcept;
extern cl::Input board_verifiedDescriptorHeader() noexcept;
extern bool board_running() noexcept;
extern void board_poll() noexcept;
extern bool board_txAvailable(std::size_t bytes) noexcept;
extern bool board_beginPacket(cl::Input bytes, std::uint32_t session,
                              std::uint32_t request) noexcept;
extern bool board_pollReply(cl::Output storage, std::size_t& bytes, std::uint32_t& session,
                            std::uint32_t& request) noexcept;
extern void board_closeSession() noexcept;

static std::array<std::byte, 1024> tx{}, response{}, portTx{}, portRx{};
static cl::Clock clientClock{nullptr, +[](void*) noexcept {
	                             return board_millis();
                             }};
#if defined(TELEMETRY_EXAMPLE_RETRY_ENCODED)
// Retries may execute an endpoint again. Use only where that is acceptable.
static ::telemetry::integration::encoded::RetryingEncodedBackend backend{
    tx, response, clientClock, {.replyTimeoutMs = 500, .maxRetries = 3}};
#else
static ::telemetry::integration::encoded::EncodedBackend backend{tx, response, clientClock};
#endif
static api::client::DeviceClient<4, 256> client{backend, clientClock};
// The device's endpoints by path: device.read<"device/Config">(...), or a handle.
static api::client::Device<api::client::DeviceClient<4, 256>> device{client};
// Explicit cross-platform example profile; includes FIFO wait and every attempt.
static constexpr std::uint32_t requestTimeoutMs = 30000;
static unsigned completed = 0, failed = 0;

static void step() noexcept
{
	board_poll();
	std::size_t count = 0;
	std::uint32_t session = 0, request = 0;
	if (board_pollReply(portRx, count, session, request))
		(void)client.onReply(cl::Input{portRx}.first(count), session, request);
	client.process(board_millis());
	const auto packet = client.nextPacket();
	if (packet && packet.bytes.size() <= portTx.size() && board_txAvailable(packet.bytes.size())) {
		// Port-owned bytes stay stable until board_txAvailable() reports release.
		std::copy(packet.bytes.begin(), packet.bytes.end(), portTx.begin());
		if (client.prepareHandoff(packet.session, packet.requestId)) {
			if (board_beginPacket(cl::Input{portTx}.first(packet.bytes.size()), packet.session,
			                      packet.requestId))
				(void)client.onSendAccepted(packet.session, packet.requestId);
			else
				backend.cancel(cl::Status::TransportError);
		}
	}
}

int main()
{
	board_init();
	if (client.admitDescriptor(board_verifiedDescriptorHeader(), 1) != cl::Status::Ok)
		return 1;
	// Three logical requests share one active Encoded exchange and one byte pool.
	// The path is checked at compile time against the generated table.
	auto one = device.read<"device/Config">(
	    [](const auto& result) noexcept {
		    if (!result.hasValue())
			    ++failed;
		    ++completed;
	    },
	    requestTimeoutMs);
	// A handle keeps the endpoint for later calls.
	const auto deviceConfig = api::client::endpoint<"device/Config">(device);
	api::types::client_example::Config config{};
	config.gain = 2.0f;
	auto two = deviceConfig.write(
	    config,
	    [](const auto& result) noexcept {
		    if (!result)
			    ++failed;
		    ++completed;
	    },
	    requestTimeoutMs);
	// The per-family constant api::device::fields::Config names the same
	// endpoint; that form is being removed.
	auto three = client.read(
	    api::device::fields::Config,
	    [](const auto& result) noexcept {
		    if (!result.hasValue() || result.value().gain != 2.0f)
			    ++failed;
		    ++completed;
	    },
	    requestTimeoutMs);
	if (!one || !two || !three)
		return 2;
	while (board_running() && completed != 3)
		step();
	client.close();
	while (!client.drained())
		step();
	board_closeSession();
	return failed || completed != 3 ? 3 : 0;
}
