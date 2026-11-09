// Application-facing resource API, without telemetry in the consumer (MIT).
// Authors: Ruslan Kovtun (shpegun60), codexAi.
// Checks the application DeviceResources facade through its public resource protocol.
// The consumer includes no telemetry API and validates listing plus both generated descriptor streams.

#include "fixtures/resources/DeviceResources.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(...)                                                                                 \
	do {                                                                                           \
		if (!(__VA_ARGS__))                                                                        \
			std::abort();                                                                          \
	} while (false)

int main()
{
	namespace api = device::resources;
	CHECK(api::fileCount() == 2);
	CHECK(api::path(api::fileIds::Descriptor) == "/telemetry/descriptor.bin");
	CHECK(api::path(api::fileIds::Values) == "/telemetry/values.bin");
	for (resource::FileIndex i = 0; i < 2; ++i) {
		const auto stat = api::stat(i);
		CHECK(stat.status == resource::Status::Ok && stat.flags == resource::FileFlag::Readable);
		resource::Cursor cursor = 0;
		std::array<std::byte, 63> bytes{};
		std::string result;
		for (std::size_t tries = 0;; ++tries) {
			CHECK(tries < stat.size + 1);
			const auto read = api::read(i, cursor, bytes);
			CHECK(read.status == resource::Status::Ok && read.written <= bytes.size());
			result.append(reinterpret_cast<const char*>(bytes.data()), read.written);
			if (read.eof) {
				break;
			}
			CHECK(read.next != cursor);
			cursor = read.next;
		}
		CHECK(result.size() == stat.size && result.substr(0, 4) == (i == 0 ? "TDS3" : "TVL3"));
		CHECK(api::write(i, 0, {}).status == resource::Status::NotWritable);
	}
	std::array<std::byte, 9> list{};
	list[0] = std::byte{1};
	std::array<std::byte, 200> reply{};
	CHECK(api::handle(list, reply) > 12 && reply[0] == std::byte{0} && reply[9] == std::byte{1});
	std::puts("Device resource facade: passed");
}
