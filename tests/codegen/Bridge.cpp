/*
 * @file Bridge.cpp
 * @brief Process transport for generated web clients against real native APIs.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include "Device.hpp"
#include <resource/protocol/Protocol.hpp>
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <charconv>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace cf = codegen_fixture;
namespace ts = telemetry;
namespace rpc = resource::telemetry::v3;

static std::vector<std::byte> load(const char* path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	const auto size = file.tellg();
	if (!file || size < 0 || size > 1048576)
		throw std::runtime_error("Invalid input");
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	file.seekg(0);
	if (!bytes.empty())
		file.read(reinterpret_cast<char*>(bytes.data()), size);
	if (!file)
		throw std::runtime_error("Cannot read input");
	return bytes;
}

static void save(const char* path, std::span<const std::byte> bytes)
{
	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char*>(bytes.data()),
	           static_cast<std::streamsize>(bytes.size()));
	if (!file)
		throw std::runtime_error("Cannot write output");
}

static void resourceLoop()
{
	std::array<std::byte, 4096> request{}, result{};
	constexpr auto view = cf::model.view();
	rpc::RpcMailbox mailbox{view, cf::workspace, request, result};
	rpc::DescriptorFile descriptor{cf::descriptorBytes};
	resource::FileSystem files{std::array{resource::file("/rpc", mailbox),
	                                      resource::file("/telemetry/descriptor.bin", descriptor)}};
	std::array<std::byte, 64> reply{};
	std::string text;
	std::uint32_t now = 0;
	while (std::getline(std::cin, text)) {
		if (text.size() > 131102 || text.size() % 2 != 0)
			throw std::runtime_error("Bad packet length");
		std::vector<std::byte> packet(text.size() / 2);
		for (std::size_t i = 0; i < packet.size(); ++i) {
			unsigned value = 0;
			const auto parsed =
			    std::from_chars(text.data() + 2 * i, text.data() + 2 * i + 2, value, 16);
			if (parsed.ec != std::errc{} || parsed.ptr != text.data() + 2 * i + 2)
				throw std::runtime_error("Bad packet hex");
			packet[i] = std::byte(value);
		}
		const auto outcome = resource::protocol::process(files.view(), packet, reply);
		mailbox.process(now++);
		constexpr char hex[] = "0123456789abcdef";
		for (const auto byte : std::span{reply}.first(outcome.written)) {
			const auto value = std::to_integer<unsigned>(byte);
			std::cout << hex[value >> 4] << hex[value & 15];
		}
		std::cout << '\n' << std::flush;
	}
}

int main(int argc, char** argv)
{
	try {
		if (argc == 2 && std::string_view(argv[1]) == "resource") {
			resourceLoop();
			return 0;
		}
		if (argc != 6 || std::string_view(argv[1]) != "encoded")
			throw std::runtime_error("Expected resource or encoded OP ID INPUT OUTPUT");
		std::uint32_t id = 0;
		const std::string_view idText{argv[3]};
		const auto parsed = std::from_chars(idText.data(), idText.data() + idText.size(), id);
		if (parsed.ec != std::errc{} || parsed.ptr != idText.data() + idText.size())
			throw std::runtime_error("Bad endpoint ID");
		const auto input = load(argv[4]);
		std::array<std::byte, 4096> output{};
		unsigned endpoint = 0;
		std::uint32_t written = 0;
		ts::DispatchStatus dispatch = ts::DispatchStatus::InternalError;
		const std::string_view operation{argv[2]};
		if (operation == "read") {
			const auto r = ts::readFieldEncoded(cf::model.view(), id, output, cf::workspace);
			dispatch = r.dispatch;
			written = r.written;
		} else if (operation == "write") {
			const auto r = ts::writeFieldEncoded(cf::model.view(), id, input, cf::workspace);
			dispatch = r.dispatch;
			endpoint = static_cast<unsigned>(r.endpointStatus);
		} else if (operation == "command") {
			const auto r = ts::executeCommandEncoded(cf::model.view(), id, input, cf::workspace);
			dispatch = r.dispatch;
			endpoint = static_cast<unsigned>(r.endpointStatus);
		} else if (operation == "service") {
			const auto r =
			    ts::callServiceEncoded(cf::model.view(), id, input, output, cf::workspace);
			dispatch = r.dispatch;
			endpoint = static_cast<unsigned>(r.endpointStatus);
			written = r.written;
		} else
			throw std::runtime_error("Bad encoded operation");
		save(argv[5], std::span{output}.first(written));
		std::cout << static_cast<unsigned>(dispatch) << ' ' << endpoint << ' ' << written << ' '
		          << client_example::device.calls + cf::edge.calls << '\n';
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
