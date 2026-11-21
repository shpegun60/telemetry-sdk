/*
 * @file Device.cpp
 * @brief Native producer and encoded endpoint bridge for JS interoperability.
 * @author Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT
 */
#include "../../../examples/structured_client/Device.hpp"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace ex = client_example;
namespace ts = telemetry;

static void save(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char*>(bytes.data()),
	           static_cast<std::streamsize>(bytes.size()));
	if (!file)
		throw std::runtime_error("Cannot write fixture");
}

static std::vector<std::byte> load(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	const auto size = file.tellg();
	if (!file || size < 0 || size > 1048576)
		throw std::runtime_error("Invalid input file");
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	file.seekg(0);
	if (!bytes.empty())
		file.read(reinterpret_cast<char*>(bytes.data()), size);
	if (!file)
		throw std::runtime_error("Cannot read input");
	return bytes;
}

static void generate(const std::filesystem::path& directory)
{
	std::filesystem::create_directories(directory);
	save(directory / "descriptor.bin", ex::descriptorBytes);
	std::vector<std::byte> values(ex::values.size());
	const auto read = ex::values.read(0, values);
	if (read.status != resource::Status::Ok || !read.eof || read.written != values.size())
		throw std::runtime_error("Values fixture failed");
	save(directory / "values.bin", values);
	std::array<std::byte, ts::wireSize<ex::Request>> request;
	if (ts::encode(ex::request, request) != ts::CodecStatus::Ok)
		throw std::runtime_error("Request fixture failed");
	save(directory / "request.bin", request);
	std::array<std::byte, ts::wireSize<ex::Response>> response;
	const auto call = ex::model.serviceIndex().callEncoded(0u, request, response, ex::workspace);
	if (call.dispatch != ts::DispatchStatus::Ok || call.endpointStatus != ts::ServiceStatus::Ok)
		throw std::runtime_error("Response fixture failed");
	save(directory / "response.bin", response);
}

int main(int argc, char** argv)
{
	try {
		if (argc == 2) {
			generate(argv[1]);
			return 0;
		}
		if (argc != 5)
			throw std::runtime_error("Expected operation id input output");
		std::uint32_t id;
		const std::string_view idText{argv[2]};
		const auto parsed = std::from_chars(idText.data(), idText.data() + idText.size(), id);
		if (parsed.ec != std::errc{} || parsed.ptr != idText.data() + idText.size())
			throw std::runtime_error("Invalid packed u32 ID");
		const auto input = load(argv[3]);
		std::array<std::byte, 2048> output;
		ts::DispatchStatus dispatch;
		unsigned endpoint = 0;
		std::uint32_t written = 0;
		const std::string_view operation{argv[1]};
		if (operation == "service") {
			const auto result =
			    ex::model.serviceIndex().callEncoded(id, input, output, ex::workspace);
			dispatch = result.dispatch;
			endpoint = static_cast<unsigned>(result.endpointStatus);
			written = result.written;
		} else if (operation == "write") {
			const auto result = ex::model.fieldIndex().writeEncoded(id, input, ex::workspace);
			dispatch = result.dispatch;
			endpoint = static_cast<unsigned>(result.endpointStatus);
			if (dispatch == ts::DispatchStatus::Ok &&
			    result.endpointStatus == telemetry::WriteResult::Applied)
				written = ex::model.fieldIndex().readEncoded(id, output, ex::workspace).written;
		} else if (operation == "command") {
			const auto result = ex::model.commandIndex().executeEncoded(id, input, ex::workspace);
			dispatch = result.dispatch;
			endpoint = static_cast<unsigned>(result.endpointStatus);
		} else
			throw std::runtime_error("Unknown operation");
		save(argv[4], std::span{output}.first(written));
		std::cout << static_cast<unsigned>(dispatch) << ' ' << endpoint << ' ' << written << ' '
		          << ex::device.calls << '\n';
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
