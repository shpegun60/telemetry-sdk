/* Offline linked Generated+handwritten scalar clients, with no native Model. MIT. */
#include "Contracts.hpp"
#include <integration/read_many/StaticClient.hpp>
#include <integration/encoded/Backend.hpp>
#include <array>

namespace cl = telemetry::client;
namespace rm = telemetry::integration::read_many;
namespace api = static_many_api;
static volatile std::uint64_t observed = 0;

template<class Codec>
using Client = cl::DeviceClient<Codec, 2, 130, 256>;

template<class Codec>
static void qualify(Client<Codec>& client, telemetry::integration::encoded::EncodedBackend& backend,
                    telemetry::protocol::encoded::read_many::Capability capability,
                    std::uint32_t now) noexcept
{
	const auto binding =
	    rm::bindReadMany(client, backend, capability,
	                     rm::fields(api::mixed::Bool, api::mixed::U64, api::mixed::Enum));
	if (binding) {
		(void)rm::readMany(client, binding.binding, [](const auto& result) noexcept {
			if (result.template get<0>().hasValue() && result.template get<1>().hasValue() &&
			    result.template get<2>().hasValue())
				observed = result.template get<1>().value();
		});
	}
	client.process(now);
	client.close();
	client.process(now);
}

// The board/application provides admitted clients and transport-owned concrete
// backends/buffers. This linked root measures the selected typed client paths,
// not backend construction/vtables or a complete firmware image. Nothing runs
// on a target in this qualification.
extern "C" void
static_many_entry(Client<api::client::CodecPolicy>& generated,
                  telemetry::integration::encoded::EncodedBackend& generatedBackend,
                  Client<static_many_contracts::Codec>& handwritten,
                  telemetry::integration::encoded::EncodedBackend& handwrittenBackend,
                  telemetry::protocol::encoded::read_many::Capability capability,
                  std::uint32_t now) noexcept
{
	qualify(generated, generatedBackend, capability, now);
	qualify(handwritten, handwrittenBackend, capability, now);
}
