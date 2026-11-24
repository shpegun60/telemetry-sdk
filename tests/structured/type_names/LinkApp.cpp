/*
 * @file LinkApp.cpp
 * @brief Caller of two compiled SDK boundaries for the type-names link test.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * run.py links this object with StructuredAbi.cpp and Adapter.cpp built with
 * the same and with the opposite TELEMETRY_TYPE_NAMES. The switch is part of
 * the ABI tag, so a mixed build names other symbols and must fail to link.
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <telemetry/abi/StructuredAbi.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {
namespace ts = telemetry;

// A named struct, so the switch would change this program's descriptor.
struct Sample {
	std::uint32_t count;
};

Sample read() noexcept
{
	return {7};
}

inline constexpr ts::FieldTable fields{ts::field<&read>("Sample")};
inline constexpr ts::testing::FieldCatalogTable catalogs{ts::testing::group("link", fields)};
inline constexpr ts::testing::ServiceCatalogTable<> noServices{};
inline constexpr ts::testing::V30Model model{catalogs, ts::testing::emptyCommands, noServices};
} // namespace

int main()
{
	ts::requireStructuredAbi();
	std::array<std::byte, 64> storage{};
	ts::Workspace workspace{storage};
	std::array<std::byte, 8> output{};
	const auto result = ts::readFieldEncoded(model.view(), 0, output, workspace);
	return result.dispatch == ts::DispatchStatus::Ok && result.written == 4 ? 0 : 1;
}
