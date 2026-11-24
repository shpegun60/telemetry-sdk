/*
 * @file LargeResponse.cpp
 * @brief A 4 KiB ServiceResult lives in caller-owned Workspace during dispatch.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */

#include <telemetry/model/Model.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ts = telemetry;

namespace large {

struct Request {
	std::uint8_t seed;
};

struct Response {
	std::array<std::uint8_t, 4096> bytes;
};

inline int calls = 0;

Response raw(const Request& request) noexcept
{
	++calls;
	Response response{};
	response.bytes[0] = request.seed;
	response.bytes.back() = static_cast<std::uint8_t>(request.seed + 1u);
	return response;
}

ts::ServiceResult<Response> wrapped(const Request& request) noexcept
{
	++calls;
	return ts::ServiceResult<Response>::successFrom([&]() -> Response {
		Response response{};
		response.bytes[0] = request.seed;
		response.bytes.back() = static_cast<std::uint8_t>(request.seed + 2u);
		return response;
	});
}

inline constexpr ts::Model model{
    ts::group("large", ts::service<&raw>("Raw"), ts::service<&wrapped>("Wrapped"))};

static_assert(model.maxServiceResponseWireSize() == 4096);
static_assert(model.maxServiceScratch() ==
              ts::scratchBytes<ts::ServiceResult<Response>> +
                  (sizeof(Request) <= ts::maxLocalObjectBytes ? 0 : ts::scratchBytes<Request>));

} // namespace large

int main()
{
	constexpr auto rawId = ts::idOf<large::model, "large/Raw">();
	constexpr auto wrappedId = ts::idOf<large::model, "large/Wrapped">();
	std::array<std::byte, 1> input{std::byte{42}};
	std::array<std::byte, 4096> output{};
	std::array<std::byte, large::model.maxServiceScratch()> scratch{};
	ts::Workspace workspace{scratch};
	auto index = large::model.serviceIndex();

	if (index.callEncoded(rawId, input, std::span{output}.first(4095), workspace).dispatch !=
	        ts::DispatchStatus::BufferTooSmall ||
	    large::calls != 0)
		return 1;
	ts::Workspace shortWorkspace{std::span{scratch}.first(1)};
	if (index.callEncoded(rawId, input, output, shortWorkspace).dispatch !=
	        ts::DispatchStatus::WorkspaceTooSmall ||
	    large::calls != 0)
		return 2;

	auto raw = index.callEncoded(rawId, input, output, workspace);
	if (raw.dispatch != ts::DispatchStatus::Ok || raw.written != 4096 ||
	    output[0] != std::byte{42} || output.back() != std::byte{43} || workspace.used() != 0 ||
	    large::calls != 1)
		return 3;

	auto wrapped = index.callEncoded(wrappedId, input, output, workspace);
	if (wrapped.dispatch != ts::DispatchStatus::Ok || wrapped.written != 4096 ||
	    output[0] != std::byte{42} || output.back() != std::byte{44} || workspace.used() != 0 ||
	    large::calls != 2)
		return 4;
	return 0;
}
