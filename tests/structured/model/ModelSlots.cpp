/*
 * @file ModelSlots.cpp
 * @brief Encoded dispatch through every supported late-bound Service slot.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/model/Model.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ts = telemetry;

namespace slots {

struct Request {
	std::uint8_t value;
};

struct Response {
	std::uint8_t value;
};

inline int calls = 0;

Response first(const Request& request) noexcept
{
	++calls;
	return {static_cast<std::uint8_t>(request.value + 1u)};
}

Response alternate(const Request& request) noexcept
{
	++calls;
	return {static_cast<std::uint8_t>(request.value + 2u)};
}

// Counted Service owner exercises rebind/reset through every standard slot family.
// API: read().
struct Device {
	std::uint8_t offset = 10;

	Response read(const Request& request) const noexcept
	{
		++calls;
		return {static_cast<std::uint8_t>(offset + request.value)};
	}
};

Response contextRead(void* context, const Request& request) noexcept
{
	return static_cast<Device*>(context)->read(request);
}

inline Device device{};
inline telemetry::OwnerSlot<Device> owner;
inline telemetry::FunctionSlot<Response(const Request&) noexcept> function;
inline telemetry::ContextFunctionSlot<Response(const Request&) noexcept> context;
inline telemetry::DelegateRefSlot<Response(const Request&) noexcept> reference;
inline telemetry::DelegateSlot<Response(const Request&) noexcept> value;

// One group of Services, each bound through a different slot form.
inline constexpr ts::Model model{
    ts::group("slots", ts::service<&Device::read>("Owner", owner),
              ts::service("Function", function), ts::service("Context", context),
              ts::service("Reference", reference), ts::service("Value", value))};

} // namespace slots

int main()
{
	std::array<std::byte, 1> request{std::byte{3}};
	std::array<std::byte, 1> response{};
	std::array<std::byte, 64> scratch{};
	ts::Workspace workspace{scratch};
	const auto index = slots::model.serviceIndex();

	for (std::uint16_t i = 0; i < 5; ++i) {
		const auto result =
		    index.callEncoded(ts::detail::makeId(0, i), request, response, workspace);
		if (result.dispatch != ts::DispatchStatus::Unavailable || slots::calls != 0)
			return 1;
	}

	slots::owner.bind(slots::device);
	slots::function.bind(&slots::first);
	slots::context.bind(&slots::contextRead, &slots::device);
	auto borrowed = [](const slots::Request& request) noexcept {
		return slots::device.read(request);
	};
	slots::reference.bind(borrowed);
	slots::value.bind([](const slots::Request& request) noexcept {
		return slots::device.read(request);
	});

	for (std::uint16_t i = 0; i < 5; ++i) {
		const auto result =
		    index.callEncoded(ts::detail::makeId(0, i), request, response, workspace);
		const std::byte expected = i == 1 ? std::byte{4} : std::byte{13};
		if (result.dispatch != ts::DispatchStatus::Ok ||
		    result.endpointStatus != ts::ServiceStatus::Ok || result.written != 1 ||
		    response[0] != expected || slots::calls != static_cast<int>(i + 1))
			return 2;
	}

	slots::function.bind(&slots::alternate);
	if (index.callEncoded(ts::idOf<slots::model, "slots/Function">(), request, response, workspace)
	            .dispatch != ts::DispatchStatus::Ok ||
	    response[0] != std::byte{5} || slots::calls != 6)
		return 3;

	slots::owner.reset();
	slots::function.reset();
	slots::context.reset();
	slots::reference.reset();
	slots::value.reset();
	for (std::uint16_t i = 0; i < 5; ++i) {
		if (index.callEncoded(ts::detail::makeId(0, i), request, response, workspace).dispatch !=
		        ts::DispatchStatus::Unavailable ||
		    slots::calls != 6)
			return 4;
	}
	return 0;
}
