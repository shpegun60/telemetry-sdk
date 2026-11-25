/* Minimal installed-only consumer: all four operations, default Callback64. MIT. */
#include <integration/batch/StaticClient.hpp>
#include <integration/encoded/Backend.hpp>
#include <static_batch_api/ClientBindings.hpp>
#include <array>
#include <cstdio>

namespace api = static_batch_api;
namespace bt = telemetry::integration::batch;
namespace cl = telemetry::client;

int main()
{
	cl::Clock clock{nullptr, +[](void*) noexcept -> std::uint32_t {
		                return 0;
	                }};
	std::array<std::byte, 512> tx{}, rx{};
	telemetry::integration::encoded::EncodedBackend backend{tx, rx, clock, 30000, false, true};
	api::client::DeviceClient<2, 256> client{backend, clock};
	if (client.admitDescriptor(api::schema::descriptorHeader, 17) != cl::Status::Ok)
		return 1;
	api::control::types::Config arguments{};
	const auto request =
	    bt::items(bt::write(api::control::Config, arguments), bt::read(api::control::Config),
	              bt::command(api::control::Configure, arguments),
	              bt::service(api::control::Echo, arguments));
	bt::StaticBatchStorage<api::client::CodecPolicy, decltype(request)> storage;
	const auto binding = bt::bindBatch(client, backend, {17, api::schema::fingerprint}, request);
	if (!binding)
		return 2;
	unsigned calls = 0;
	const auto submitted =
	    bt::batch(client, binding.binding, request, storage, [&](const auto& result) noexcept {
		    if (storage.busy() && result.operation.status == cl::Status::Cancelled &&
		        !result.template get<0>() && !result.template get<1>())
			    ++calls;
	    });
	if (!submitted || !storage.busy() || client.pending() != 1)
		return 3;
	client.close();
	for (unsigned i = 0; i < 4 && !client.drained(); ++i)
		client.process(0);
	if (!client.drained() || storage.busy() || calls != 1)
		return 4;
	std::printf(
	    "Static Batch installed consumer: callbackBytes=64 clientBytes=%zu storeBytes=%zu\n",
	    sizeof(client), sizeof(storage));
	return 0;
}
