/* All metadata factories accept constexpr array-element pointer names.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT. */
#include <telemetry/Telemetry.hpp>
namespace ts = telemetry;
inline constexpr std::array<std::array<char, 6>, 4> names{{{'F', 'i', 'e', 'l', 'd', '\0'},
                                                           {'C', 'm', 'd', '\0'},
                                                           {'S', 'v', 'c', '\0'},
                                                           {'G', 'r', 'o', 'u', 'p', '\0'}}};

std::uint32_t readValue() noexcept
{
	return 1;
}

telemetry::CommandResult commandCall() noexcept
{
	return telemetry::CommandResult::Executed;
}

void serviceCall() noexcept
{}

inline constexpr ts::FieldTable fields{ts::field<&readValue>(names[0].data())};
inline constexpr ts::CommandTable commands{ts::command<&commandCall>(names[1].data())};
inline constexpr ts::ServiceTable services{ts::service<&serviceCall>(names[2].data())};
inline constexpr ts::Model model{ts::group(names[3].data(), ts::field<&readValue>(names[0].data()),
                                           ts::command<&commandCall>(names[1].data()),
                                           ts::service<&serviceCall>(names[2].data()))};
static_assert(model.groupName(0)[0] == 'G');
static_assert(fields.get<0>().name()[0] == 'F' && commands.get<0>().name()[0] == 'C' &&
              services.get<0>().name()[0] == 'S');
