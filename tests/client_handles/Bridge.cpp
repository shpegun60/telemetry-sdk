/*
 * @file Bridge.cpp
 * @brief The group-tree Model as a native Encoded peer for the JS handle parity.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * `Bridge --descriptor FILE` writes the Model's descriptor. Without arguments it
 * reads one hex Encoded request per line and answers with the hex reply, or with
 * the device's counters for the line "state".
 */

#include "../structured/group_tree/AdcBranch.hpp"
#include "../structured/group_tree/MetrologyBranch.hpp"

#include <integration/encoded/Dispatcher.hpp>
#include <telemetry/descriptor/Descriptor.hpp>

#include <array>
#include <cstdio>
#include <cstring>

namespace {
namespace ts = ::telemetry;
namespace en = ::telemetry::integration::encoded;
inline constexpr ts::Model model{adc::tlm::branch, metrology::tlm::branch};
inline constexpr ts::descriptor::Descriptor descriptor{model};
inline constexpr auto bytes = ts::descriptor::packDescriptor<descriptor>();

int digit(char value) noexcept
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	return -1;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc == 3 && std::strcmp(argv[1], "--descriptor") == 0) {
		std::FILE* file = std::fopen(argv[2], "wb");
		if (!file)
			return 2;
		const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
		return std::fclose(file) == 0 && written ? 0 : 2;
	}
	if (argc != 1)
		return 2;
	std::array<char, 4096> line{};
	std::array<std::byte, 1024> input{}, output{}, scratch{};
	ts::Workspace workspace{scratch};
	while (std::fgets(line.data(), static_cast<int>(line.size()), stdin)) {
		auto size = std::strlen(line.data());
		while (size && (line[size - 1] == '\r' || line[size - 1] == '\n'))
			line[--size] = '\0';
		if (std::strcmp(line.data(), "state") == 0) {
			std::printf("{\"resets\":%d,\"calibrations\":%d,\"writes\":%d}\n",
			            adc::tlm::instance.resets, adc::tlm::instance.calibrations,
			            adc::tlm::instance.writes);
		} else {
			if (!size || size % 2 || size / 2 > input.size())
				return 3;
			for (std::size_t i = 0; i < size / 2; ++i) {
				const auto high = digit(line[2 * i]), low = digit(line[2 * i + 1]);
				if (high < 0 || low < 0)
					return 3;
				input[i] = std::byte((high << 4) | low);
			}
			const auto reply =
			    en::dispatchEncoded(model.view(), workspace, 17,
			                        std::span<const std::byte>{input}.first(size / 2), output);
			if (reply.status != en::EncodedDispatchStatus::Ok)
				return 4;
			for (std::size_t i = 0; i < reply.written; ++i)
				std::printf("%02x", std::to_integer<unsigned>(output[i]));
			std::putchar('\n');
		}
		std::fflush(stdout);
	}
	return std::ferror(stdin) ? 5 : 0;
}
