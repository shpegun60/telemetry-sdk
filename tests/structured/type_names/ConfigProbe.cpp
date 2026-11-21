/*
 * @file ConfigProbe.cpp
 * @brief Prints the configuration one translation unit sees through Config.hpp.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * run.py builds it with no project file, with config/telemetry_config.hpp on
 * the include path, and with -D definitions, and compares the printed values
 * and the emitted descriptor minor with what each source must select.
 */

#include <telemetry/Telemetry.hpp>
#include <telemetry/testing/V30Model.hpp>
#include <telemetry/descriptor/Descriptor.hpp>

#include <cstdio>

namespace {
namespace ts = telemetry;

struct Sample {
	std::uint32_t count;
};

Sample read() noexcept
{
	return {};
}

inline constexpr ts::FieldTable fields{ts::field<&read>("Sample")};
inline constexpr ts::testing::FieldCatalogTable catalogs{ts::testing::group("config", fields)};
inline constexpr ts::testing::ServiceCatalogTable<> noServices{};
inline constexpr ts::testing::V30Model model{catalogs, ts::testing::emptyCommands, noServices};
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto bytes = ::telemetry::descriptor::packDescriptor<descriptor>();
} // namespace

int main()
{
	std::printf("TELEMETRY_TYPE_NAMES=%d TELEMETRY_STRUCTURED_LOCAL_BYTES=%d "
	            "maxLocalObjectBytes=%u minor=%u\n",
	            TELEMETRY_TYPE_NAMES, TELEMETRY_STRUCTURED_LOCAL_BYTES,
	            static_cast<unsigned>(ts::maxLocalObjectBytes),
	            std::to_integer<unsigned>(bytes[6]));
	return 0;
}
