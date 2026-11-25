/* One actual Encoded dispatcher and the same FIFO for static and typed reads. MIT. */
#ifndef TELEMETRY_TESTS_READ_MANY_STATIC_FIXTURE_HPP
#define TELEMETRY_TESTS_READ_MANY_STATIC_FIXTURE_HPP
#pragma once
#include "Contracts.hpp"
#include "Model.hpp"
#include <integration/read_many/StaticClient.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace static_many_test {
namespace api = static_many_api;
namespace native = static_many_fixture;
namespace cl = telemetry::client;
namespace rm = telemetry::integration::read_many;
namespace en = telemetry::integration::encoded;
namespace ew = telemetry::protocol::encoded;
namespace sw = ew::read_many;
inline unsigned checks = 0;

inline void check(bool value, unsigned line) noexcept
{
	++checks;
	if (!value) {
		std::fprintf(stderr, "Static ReadMany check=%u line=%u failed\n", checks, line);
		std::abort();
	}
}

#define CHECK(...) ::static_many_test::check(bool(__VA_ARGS__), __LINE__)

template<class C = api::client::CodecPolicy, std::size_t N = 4, std::size_t Payload = 1024,
         std::size_t Callback = 256>
struct Harness {
	using Client = cl::DeviceClient<C, N, Payload, Callback>;
	std::uint32_t now = 0;
	cl::Clock clock{this, +[](void* pointer) noexcept {
		                return static_cast<Harness*>(pointer)->now;
	                }};
	std::array<std::byte, 1024> tx{}, rx{}, port{}, reply{}, scratch{};
	telemetry::Workspace workspace{scratch};
	en::EncodedBackend backend{tx, rx, clock, 30000, true};
	Client client{backend, clock};
	sw::Capability capability{17, C::fingerprint};

	Harness() noexcept
	{
		native::owner.reset();
		CHECK(client.admitDescriptor(C::descriptorHeader(), 17) == cl::Status::Ok);
	}

	cl::Packet offered() noexcept
	{
		client.process(now);
		auto packet = client.nextPacket();
		CHECK(packet);
		CHECK(packet.bytes.size() <= port.size());
		std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
		packet.bytes = cl::Input{port}.first(packet.bytes.size());
		return packet;
	}

	std::size_t response(cl::Packet packet) noexcept
	{
		const auto done = en::dispatchEncoded(native::model.view(), workspace, packet.session,
		                                      packet.bytes, reply, capability);
		CHECK(done);
		return done.written;
	}

	void handoff(cl::Packet packet) noexcept
	{
		CHECK(client.prepareHandoff(packet.session, packet.requestId));
		CHECK(client.onSendAccepted(packet.session, packet.requestId));
	}

	void deliver(cl::Packet packet, std::size_t bytes) noexcept
	{
		CHECK(client.onReply(cl::Input{reply}.first(bytes), packet.session, packet.requestId));
		client.process(now);
	}

	void send(cl::Packet packet) noexcept
	{
		handoff(packet);
		deliver(packet, response(packet));
	}

	void stop() noexcept
	{
		client.close();
		for (unsigned i = 0; i < 8 && !client.drained(); ++i)
			client.process(now);
		CHECK(client.drained());
	}
};

template<class T>
std::uint64_t bits(const T& value) noexcept
{
	if constexpr (std::is_same_v<T, bool>)
		return value ? 1 : 0;
	else if constexpr (std::is_same_v<T, float>)
		return std::bit_cast<std::uint32_t>(value);
	else if constexpr (std::is_same_v<T, double>)
		return std::bit_cast<std::uint64_t>(value);
	else {
		using R = typename static_many_contracts::Representation<T>::Type;
		using U = std::make_unsigned_t<R>;
		return static_cast<U>(static_cast<R>(value));
	}
}

struct Observed {
	unsigned calls = 0;
	cl::Status status = cl::Status::Busy;
	rm::StaticValueStatus valueStatus = rm::StaticValueStatus::NoValue;
	std::array<std::uint32_t, 32> ids{};
	std::array<sw::ItemStatus, 32> statuses{};
	std::array<bool, 32> present{};
	std::array<std::uint64_t, 32> values{};
	std::array<std::byte, 514> payload{};
	std::size_t payloadBytes = 0, count = 0;

	template<class... T>
	void record(const rm::StaticReadManyResult<T...>& result) noexcept
	{
		++calls;
		status = result.operation.status;
		valueStatus = result.valueStatus;
		payloadBytes = result.operation.payload.size();
		CHECK(payloadBytes <= payload.size());
		std::copy(result.operation.payload.begin(), result.operation.payload.end(),
		          payload.begin());
		count = sizeof...(T);
		[&]<std::size_t... I>(std::index_sequence<I...>) noexcept {
			(
			    [&] {
				    const auto& item = result.template get<I>();
				    ids[I] = item.id;
				    statuses[I] = item.status;
				    present[I] = item.hasValue();
				    if (present[I])
					    values[I] = bits(item.value());
			    }(),
			    ...);
		}(std::index_sequence_for<T...>{});
	}
};

inline constexpr std::array scalarEndpoints{
    api::left::F00,  api::left::F01,  api::left::F02,  api::left::F03,  api::left::F04,
    api::left::F05,  api::left::F06,  api::left::F07,  api::left::F08,  api::left::F09,
    api::left::F10,  api::left::F11,  api::left::F12,  api::left::F13,  api::left::F14,
    api::left::F15,  api::right::F16, api::right::F17, api::right::F18, api::right::F19,
    api::right::F20, api::right::F21, api::right::F22, api::right::F23, api::right::F24,
    api::right::F25, api::right::F26, api::right::F27, api::right::F28, api::right::F29,
    api::right::F30, api::right::F31};

template<std::size_t... I>
constexpr auto selected(std::index_sequence<I...>) noexcept
{
	return rm::fields(scalarEndpoints[(I * 7 + 5) % 32]...);
}
} // namespace static_many_test
#endif
