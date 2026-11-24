/* Read-only runtime browsing through a real local peer, or a Descriptor file. MIT. */
#include <integration/inspector/InspectorModel.hpp>
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/Dispatcher.hpp>
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>
#include <vector>

namespace cl = telemetry::client;
namespace rt = telemetry::runtime;
namespace im = telemetry::integration::inspector;
namespace en = telemetry::integration::encoded;

// Native DTOs belong only to this demonstration peer. The inspector below uses
// runtime metadata and node handles; no client-side codec is generated.
namespace peer {
struct Voltage {
	float Ua, Ub, Uc;
};

struct Calibration {
	float voltage;
};

struct Identity {
	std::uint32_t version;
};

struct Device {
	Voltage voltage() const noexcept
	{
		return {230.1f, 229.8f, 231.2f};
	}

	std::uint64_t counter() const noexcept
	{
		return UINT64_MAX;
	}

	telemetry::CommandResult calibrate(const Calibration&) noexcept
	{
		return telemetry::CommandResult::Executed;
	}

	telemetry::ServiceResult<Identity> identity() const noexcept
	{
		return telemetry::ServiceResult<Identity>::success({42});
	}
};

Device device;
constexpr telemetry::Model model{
    telemetry::group("measure", telemetry::field<&Device::voltage>("Voltage", device),
                     telemetry::field<&Device::counter>("Counter", device)),
    telemetry::group("control", telemetry::command<&Device::calibrate>("Calibrate", device)),
    telemetry::group("device", telemetry::service<&Device::identity>("Identity", device))};
constexpr telemetry::descriptor::Descriptor descriptor{model};
constexpr auto bytes = telemetry::descriptor::packDescriptor<descriptor>();
} // namespace peer

using Inspector = im::InspectorModel<4, 256, 256>;

bool printNode(const Inspector& inspector, const im::NodeHandle& node, unsigned depth = 0)
{
	const auto info = inspector.info(node);
	if (!info)
		return false;
	if (info.value.arrayElement)
		std::printf("%*s[%u]", static_cast<int>(depth * 2), "", info.value.index);
	else
		std::printf("%*s%.*s", static_cast<int>(depth * 2), "",
		            static_cast<int>(info.value.name.size()),
		            info.value.name.empty() ? "" : info.value.name.data());
	if (info.value.kind == telemetry::TypeKind::Struct ||
	    info.value.kind == telemetry::TypeKind::Array) {
		std::printf(" (%u children)\n", info.value.children);
		for (std::uint32_t i = 0; i < info.value.children; ++i) {
			const auto child = inspector.child(node, i);
			if (!child || !printNode(inspector, child.value, depth + 1))
				return false;
		}
		return true;
	}
	const auto scalar = inspector.scalar(node);
	if (!scalar)
		return false;
	std::printf(" = ");
	std::array<char, 21> decimal{};
	const auto text = inspector.decimal(node, decimal);
	if (text)
		std::printf("%.*s", static_cast<int>(text.value), decimal.data());
	else
		std::visit(
		    [](const auto& value) {
			    using T = std::remove_cvref_t<decltype(value)>;
			    if constexpr (std::is_floating_point_v<T>)
				    std::printf("%.*g", std::numeric_limits<T>::max_digits10,
				                static_cast<double>(value));
		    },
		    scalar.value.value);
	if (scalar.value.enumValue)
		std::printf(" (%.*s)",
		            static_cast<int>(scalar.value.enumKnown ? scalar.value.enumName.size() : 12),
		            scalar.value.enumKnown ? scalar.value.enumName.data() : "unknown enum");
	std::printf("\n");
	return true;
}

int main(int argc, char** argv)
{
	if (argc == 3 && std::strcmp(argv[1], "--write-demo-descriptor") == 0) {
		std::ofstream file(argv[2], std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char*>(peer::bytes.data()), peer::bytes.size());
		return file ? 0 : 1;
	}
	const bool fileMode = argc == 3 && std::strcmp(argv[1], "--descriptor") == 0;
	if (argc != 1 && !fileMode) {
		std::fprintf(stderr,
		             "Usage: inspector-host [--descriptor FILE | --write-demo-descriptor FILE]\n");
		return 1;
	}
	// The host application owns input bytes and index arrays. The portable model
	// borrows their immutable generation; its own selected-value snapshot is fixed.
	std::vector<std::byte> fileBytes;
	cl::Input descriptorBytes = peer::bytes;
	if (fileMode) {
		std::ifstream file(argv[2], std::ios::binary | std::ios::ate);
		const auto size = file.tellg();
		if (size < 64 || size > telemetry::Limits::maxDescriptorBytes)
			return 2;
		fileBytes.resize(static_cast<std::size_t>(size));
		file.seekg(0);
		if (!file.read(reinterpret_cast<char*>(fileBytes.data()), size))
			return 2;
		descriptorBytes = fileBytes;
	}
	std::array<rt::RuntimeType, 128> types{};
	std::array<rt::RuntimeMember, 256> members{};
	std::array<rt::RuntimeEnumEntry, 64> enums{};
	std::array<rt::RuntimeCatalog, 16> catalogs{};
	std::array<rt::RuntimeEndpoint, 64> endpoints{};
	rt::RuntimeSchemaWorkspace storage{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(descriptorBytes, storage);
	if (!parsed) {
		std::fprintf(stderr, "Descriptor rejected: status=%u error=%u\n",
		             static_cast<unsigned>(parsed.status), static_cast<unsigned>(parsed.error));
		return 3;
	}
	std::uint32_t now = 0;
	cl::Clock clock{&now, +[](void* p) noexcept {
		                return *static_cast<std::uint32_t*>(p);
	                }};
	std::array<std::byte, 512> tx{}, rx{}, port{}, reply{}, scratch{};
	en::EncodedBackend backend{tx, rx, clock};
	Inspector inspector{backend, clock};
	if (inspector.admit(parsed.schema, descriptorBytes.first(64), 1) != cl::Status::Ok)
		return 4;
	im::ReadResult completion;
	bool called = false;

	// Callback state is declared before this guard and survives error-path drain.
	// This local synchronous port has no physical TX borrow after dispatch returns.
	struct OwnerDrain {
		Inspector& model;
		std::uint32_t& now;

		~OwnerDrain()
		{
			model.close();
			for (unsigned i = 0; i < 6 && !model.drained(); ++i)
				model.process(now);
			if (!model.drained())
				std::abort();
		}
	} drain{inspector, now};

	std::printf("Telemetry Inspector / read-only / %s\n",
	            fileMode ? "file metadata-only; no transport connected"
	                     : "local demonstration peer");
	constexpr std::array categories{telemetry::descriptor::Category::Field,
	                                telemetry::descriptor::Category::Command,
	                                telemetry::descriptor::Category::Service};
	constexpr std::array labels{"Fields", "Commands (metadata only)", "Services (metadata only)"};
	for (std::size_t i = 0; i < categories.size(); ++i) {
		std::printf("%s\n", labels[i]);
		for (const auto& catalog : inspector.catalogs(categories[i])) {
			for (const auto& endpoint : inspector.endpoints(categories[i])
			                                .subspan(catalog.firstEndpoint, catalog.endpointCount))
				std::printf("  %.*s.%.*s id=%u type=%u\n", static_cast<int>(catalog.name.size()),
				            catalog.name.data(), static_cast<int>(endpoint.name.size()),
				            endpoint.name.data(), endpoint.id, endpoint.typeId);
		}
	}
	if (!fileMode) {
		telemetry::Workspace workspace{scratch};
		for (const auto& field : inspector.fields()) {
			const auto binding = inspector.bindField(field.id);
			completion = {};
			called = false;
			if (!binding ||
			    !inspector.readField(binding.endpoint, [&](const im::ReadResult& result) noexcept {
				    completion = result;
				    called = true;
			    }))
				return 5;
			for (unsigned step = 0; step < 32 && inspector.pending(); ++step) {
				inspector.process(now++);
				const auto packet = inspector.nextPacket();
				if (!packet)
					continue;
				if (packet.bytes.size() > port.size())
					return 6;
				std::copy(packet.bytes.begin(), packet.bytes.end(), port.begin());
				if (!inspector.prepareHandoff(packet.session, packet.requestId))
					return 6;
				if (!inspector.onSendAccepted(packet.session, packet.requestId))
					return 6;
				// The synchronous local port retains its TX copy through dispatch.
				const auto result =
				    en::dispatchEncoded(peer::model.view(), workspace, packet.session,
				                        cl::Input{port}.first(packet.bytes.size()), reply);
				if (!result || !inspector.onReply(cl::Input{reply}.first(result.written),
				                                  packet.session, packet.requestId))
					return 6;
				inspector.process(now);
			}
			const auto root = inspector.root();
			if (!called || !completion.published || !root || !printNode(inspector, root.value))
				return 7;
		}
	}
	inspector.close();
	inspector.process(now);
	return inspector.drained() ? 0 : 8;
}
