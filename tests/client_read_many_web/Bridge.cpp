/*
 * @file Bridge.cpp
 * @brief Persistent actual Model/Encoded ReadMany server for JS/TS interoperability.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>

namespace ts = telemetry;
namespace rm = ts::protocol::encoded::read_many;
namespace server = ts::integration::encoded;
using Input = std::span<const std::byte>;

enum class Mode : std::int16_t {
	Reverse = -7,
	Forward = 12
};

struct Record {
	std::uint32_t value;
};

struct Device {
	std::uint32_t current = 100;

	template<std::size_t I>
	std::uint32_t read() noexcept
	{
		return current + I;
	}

	ts::WriteResult write(std::uint32_t value) noexcept
	{
		current = value;
		return ts::WriteResult::Applied;
	}

	std::uint64_t wide() noexcept
	{
		return UINT64_MAX;
	}

	std::int64_t signedWide() noexcept
	{
		return INT64_MIN;
	}

	bool enabled() noexcept
	{
		return true;
	}

	Mode mode() noexcept
	{
		return static_cast<Mode>(-123);
	}

	float real() noexcept
	{
		return std::bit_cast<float>(std::uint32_t{0x80000000});
	}

	double doubleReal() noexcept
	{
		return std::bit_cast<double>(UINT64_C(0x7ff8000012345678));
	}

	Record record() noexcept
	{
		return {current};
	}

	ts::CommandResult command(const Record& value) noexcept
	{
		current = value.value;
		return ts::CommandResult::Accepted;
	}

	ts::ServiceResult<Record> echo(const Record& value) noexcept
	{
		return ts::ServiceResult<Record>::success(value);
	}
};

static Device device;
static ts::OwnerSlot<Device> absent;
static constexpr ts::Model model{
    ts::group(
        "scalar", ts::field<&Device::read<0>, &Device::write>("F00", device),
        ts::field<&Device::read<1>>("F01", device), ts::field<&Device::read<2>>("F02", device),
        ts::field<&Device::read<3>>("F03", device), ts::field<&Device::read<4>>("F04", device),
        ts::field<&Device::read<5>>("F05", device), ts::field<&Device::read<6>>("F06", device),
        ts::field<&Device::read<7>>("F07", device), ts::field<&Device::read<8>>("F08", device),
        ts::field<&Device::read<9>>("F09", device), ts::field<&Device::read<10>>("F10", device),
        ts::field<&Device::read<11>>("F11", device), ts::field<&Device::read<12>>("F12", device),
        ts::field<&Device::read<13>>("F13", device), ts::field<&Device::read<14>>("F14", device),
        ts::field<&Device::read<15>>("F15", device), ts::field<&Device::read<16>>("F16", device),
        ts::field<&Device::read<17>>("F17", device), ts::field<&Device::read<18>>("F18", device),
        ts::field<&Device::read<19>>("F19", device), ts::field<&Device::read<20>>("F20", device),
        ts::field<&Device::read<21>>("F21", device), ts::field<&Device::read<22>>("F22", device),
        ts::field<&Device::read<23>>("F23", device), ts::field<&Device::read<24>>("F24", device),
        ts::field<&Device::read<25>>("F25", device), ts::field<&Device::read<26>>("F26", device),
        ts::field<&Device::read<27>>("F27", device), ts::field<&Device::read<28>>("F28", device),
        ts::field<&Device::read<29>>("F29", device), ts::field<&Device::read<30>>("F30", device),
        ts::field<&Device::read<31>>("F31", device)),
    ts::group("extra", ts::field<&Device::wide>("Wide", device),
              ts::field<&Device::signedWide>("Signed", device),
              ts::field<&Device::enabled>("Enabled", device),
              ts::field<&Device::mode>("Mode", device), ts::field<&Device::real>("Real", device),
              ts::field<&Device::doubleReal>("Double", device),
              ts::field<&Device::record>("Aggregate", device),
              ts::field<&Device::read<0>>("Absent", absent)),
    ts::group("control", ts::command<&Device::command>("Configure", device),
              ts::service<&Device::echo>("Echo", device))};
static constexpr ts::descriptor::Descriptor descriptor{model};
static constexpr rm::Capability capability{17, descriptor.fingerprint()};

static int digit(char value) noexcept
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	return -1;
}

int main(int argc, char** argv)
{
	if (argc == 3) {
		auto* output = std::fopen(argv[2], "wb");
		if (!output)
			return 2;
		std::size_t written = 0, needed = 0;
		if (std::strcmp(argv[1], "--descriptor") == 0) {
			static constexpr auto bytes = ts::descriptor::packDescriptor<descriptor>();
			needed = bytes.size();
			written = std::fwrite(bytes.data(), 1, needed, output);
		} else if (std::strcmp(argv[1], "--capability") == 0) {
			std::array<std::byte, rm::capabilityBytes> bytes{};
			const auto result = rm::writeCapability(bytes, capability);
			if (result) {
				needed = result.written;
				written = std::fwrite(bytes.data(), 1, needed, output);
			}
		}
		const auto closed = std::fclose(output);
		return needed && written == needed && closed == 0 ? 0 : 2;
	}
	if (argc != 1)
		return 2;
	std::array<char, 4097> line{};
	std::array<std::byte, 2048> input{}, output{}, scratch{};
	ts::Workspace workspace{scratch};
	while (std::fgets(line.data(), static_cast<int>(line.size()), stdin)) {
		auto size = std::strlen(line.data());
		while (size && (line[size - 1] == '\r' || line[size - 1] == '\n'))
			--size;
		if (!size || size % 2 || size / 2 > input.size())
			return 3;
		for (std::size_t i = 0; i < size / 2; ++i) {
			const auto high = digit(line[2 * i]), low = digit(line[2 * i + 1]);
			if (high < 0 || low < 0)
				return 3;
			input[i] = std::byte((high << 4) | low);
		}
		const auto reply = server::dispatchEncoded(
		    model.view(), workspace, 17, Input{input}.first(size / 2), output, capability);
		if (!reply)
			return 4;
		for (std::size_t i = 0; i < reply.written; ++i)
			std::printf("%02x", std::to_integer<unsigned>(output[i]));
		std::putchar('\n');
		std::fflush(stdout);
	}
	return std::ferror(stdin) ? 5 : 0;
}
