/* Minimal installed-only consumer: one Field fits the ordinary 64-byte callback. MIT. */
#include <integration/read_many/StaticClient.hpp>
#include <integration/encoded/Backend.hpp>
#include <static_many_api/ClientBindings.hpp>
#include <array>
#include <cstdio>

namespace cl = telemetry::client;
namespace rm = telemetry::integration::read_many;
namespace api = static_many_api;
static unsigned callbacks = 0;

static void completed(const rm::StaticReadManyResult<std::uint32_t>& result) noexcept
{
	if (result.operation.status == cl::Status::Cancelled && !result.get<0>().hasValue())
		++callbacks;
}

int main()
{
	std::array<std::byte, 128> tx{}, rx{};
	cl::Clock clock{nullptr, +[](void*) noexcept -> std::uint32_t {
		                return 0;
	                }};
	telemetry::integration::encoded::EncodedBackend backend{tx, rx, clock, 30000, true};
	api::client::DeviceClient<2, 128> client{backend, clock};
	if (client.admitDescriptor(api::schema::descriptorHeader, 17) != cl::Status::Ok)
		return 1;
	const auto binding = rm::bindReadMany(client, backend, {17, api::schema::fingerprint},
	                                      rm::fields(api::left::F05));
	if (!binding)
		return 2;
	const auto submitted = rm::readMany(client, binding.binding, completed);
	if (!submitted || client.pending() != 1)
		return 3;
	client.close();
	for (unsigned i = 0; i < 4 && !client.drained(); ++i)
		client.process(0);
	if (!client.drained() || callbacks != 1)
		return 4;
	std::printf("Static ReadMany installed consumer: callbackBytes=64 clientBytes=%zu\n",
	            sizeof(client));
	return 0;
}
