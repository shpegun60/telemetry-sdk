/*
 * @file Parser.cpp
 * @brief Bounded batch parser for differential malformed descriptor checks.
 * SPDX-License-Identifier: MIT
 */
#include <telemetry/runtime/RuntimeSchema.hpp>
#include <array>
#include <cstdio>

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	auto* file = std::fopen(argv[1], "rb");
	if (!file)
		return 2;
	std::array<std::byte, 65536> bytes{};
	std::array<telemetry::runtime::RuntimeType, 128> types{};
	std::array<telemetry::runtime::RuntimeMember, 512> members{};
	std::array<telemetry::runtime::RuntimeEnumEntry, 512> enums{};
	std::array<telemetry::runtime::RuntimeCatalog, 64> catalogs{};
	std::array<telemetry::runtime::RuntimeEndpoint, 128> endpoints{};
	telemetry::runtime::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs,
	                                                     endpoints};
	std::array<unsigned char, 4> header{};
	while (std::fread(header.data(), 1, header.size(), file) == header.size()) {
		const auto size = std::uint32_t{header[0]} | (std::uint32_t{header[1]} << 8) |
		                  (std::uint32_t{header[2]} << 16) | (std::uint32_t{header[3]} << 24);
		if (size > bytes.size() || std::fread(bytes.data(), 1, size, file) != size)
			return 2;
		const auto result =
		    telemetry::runtime::RuntimeSchema::parse({bytes.data(), size}, workspace);
		std::printf("%d\n", bool(result) ? 1 : 0);
	}
	return std::fclose(file) == 0 ? 0 : 2;
}
