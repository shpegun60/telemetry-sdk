/*
 * @file WeakEndpoints.cpp
 * @brief Unresolved ELF weak targets never become calls through address zero.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/model/Model.hpp>
namespace ts = telemetry;

struct Request {
	std::uint32_t number;
};

std::uint32_t missingGet() noexcept __attribute__((weak));
telemetry::WriteResult missingSet(std::uint32_t) noexcept __attribute__((weak));
telemetry::CommandResult missingCall(const Request&) noexcept __attribute__((weak));
inline constexpr ts::FieldTable fields{ts::field<&missingGet, &missingSet>("missing")};
inline constexpr ts::CommandTable commands{ts::command<&missingCall>("missing")};
inline constexpr ts::Model model{ts::group("test", ts::field<&missingGet, &missingSet>("missing"),
                                           ts::command<&missingCall>("call"))};

int main()
{
	if (fields.read<0>() ||
	    fields.write<0>(std::uint32_t{1}) != telemetry::WriteResult::Unavailable ||
	    commands.call<0>(Request{}) != telemetry::CommandResult::Unavailable)
		return 1;
	std::array<std::byte, 64> scratch{};
	std::array<std::byte, 4> bytes{};
	ts::Workspace workspace{scratch};
	if (model.fieldIndex().readEncoded(0, bytes, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable ||
	    model.fieldIndex().writeEncoded(0, bytes, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable ||
	    model.commandIndex().executeEncoded(0, bytes, workspace).dispatch !=
	        ts::DispatchStatus::Unavailable ||
	    ts::read<model, "test/missing">() ||
	    ts::call<model, "test/call">(Request{}) != telemetry::CommandResult::Unavailable)
		return 2;
	return 0;
}
