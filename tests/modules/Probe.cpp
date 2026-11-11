/* Installed module consumer; no source-tree includes. Authors: shpegun60, codexAi. MIT. */
#include <array>
#include <cstddef>
#include <cstdint>

#if defined(MODULE_CORE) || defined(MODULE_RESOURCE_ADAPTER) || defined(MODULE_ENCODED_SERVER) ||  \
    defined(MODULE_RPC_SERVER) || defined(MODULE_RPC_SESSIONS) || defined(MODULE_TCP_SERVER) ||    \
    defined(MODULE_BATCH_SERVER)
#include <telemetry/Telemetry.hpp>
#include <telemetry/descriptor/Descriptor.hpp>
static constexpr telemetry::ServiceCatalogTable services{};
static constexpr telemetry::Model model{telemetry::emptyFields, telemetry::emptyCommands, services};
#endif
#if defined(MODULE_RUNTIME) || defined(MODULE_VALUES_READER)
#include <telemetry/runtime/RuntimeSchema.hpp>

// The frozen empty v3 registry, constructed without importing its producer.
static std::array<std::byte, 348> emptyRegistry()
{
	std::array<std::byte, 348> descriptor{};
	auto number = [&](std::size_t offset, std::uint64_t value, unsigned width) {
		for (unsigned i = 0; i < width; ++i)
			descriptor[offset + i] = std::byte((value >> (8 * i)) & 0xffu);
	};
	descriptor[0] = std::byte{'T'};
	descriptor[1] = std::byte{'D'};
	descriptor[2] = std::byte{'S'};
	descriptor[3] = std::byte{'3'};
	number(4, 3, 2);
	number(8, 64, 2);
	number(12, descriptor.size(), 4);
	number(24, 12, 4);
	number(52, 64, 4);
	number(56, descriptor.size(), 4);
	number(60, descriptor.size(), 4);
	constexpr unsigned sizes[]{0, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8};
	std::size_t offset = 64;
	for (unsigned id = 0; id < 12; ++id) {
		number(offset, 1, 1);
		number(offset + 1, 1, 1);
		number(offset + 4, id ? 16 : 12, 4);
		number(offset + 8, id, 4);
		number(offset + 12, id ? 1 : 0, 1);
		number(offset + 16, sizes[id], 4);
		if (id)
			number(offset + 20, id, 1);
		offset += id ? 24 : 20;
	}
	auto hash = telemetry::descriptor::fingerprintBasis;
	for (auto value : descriptor)
		hash =
		    (hash ^ std::to_integer<std::uint8_t>(value)) * telemetry::descriptor::fingerprintPrime;
	number(16, hash, 8);
	return descriptor;
}
#endif
#if defined(MODULE_CORE)
#include <telemetry/encoded/Dispatcher.hpp>

int main()
{
	std::array<std::byte, 256> scratch{};
	telemetry::Workspace workspace{scratch};
	const auto result = telemetry::dispatchEncoded(model.view(), telemetry::Operation::FieldRead, 0,
	                                               {}, {}, workspace);
	return result.dispatch == telemetry::DispatchStatus::NotFound ? 0 : 1;
}
#elif defined(MODULE_RUNTIME)
#include <telemetry/runtime/DynamicValueView.hpp>
#include <telemetry/runtime/DynamicValueWriter.hpp>

int main()
{
	namespace rt = telemetry::runtime;
	const auto descriptor = emptyRegistry();
	std::array<rt::RuntimeType, 12> types{};
	std::array<rt::RuntimeMember, 0> members{};
	std::array<rt::RuntimeEnumEntry, 0> enums{};
	std::array<rt::RuntimeCatalog, 0> catalogs{};
	std::array<rt::RuntimeEndpoint, 0> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(descriptor, workspace);
	if (parsed.status != rt::Status::Ok)
		return 1;
	const std::array payload{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
	const auto value = parsed.schema.view(6, payload);
	if (!value)
		return 2;
	const auto scalar = value.value.get<std::uint32_t>();
	if (!scalar || scalar.value != 0x78563412u)
		return 3;
	const auto handle = parsed.schema.resolve(6, "");
	if (!handle)
		return 4;
	std::array<std::byte, 4> scratch{}, output{};
	auto editor = rt::DynamicValueWriter::begin(parsed.schema, 6, payload, scratch);
	const std::uint32_t replacement = 0x01020304u;
	if (!editor || editor.set(handle.value, replacement) != rt::Status::Ok)
		return 5;
	const auto committed = editor.commit(output);
	if (!committed || committed.value != output.size())
		return 6;
	const auto edited = parsed.schema.view(6, output);
	if (!edited)
		return 7;
	const auto updated = edited.value.get<std::uint32_t>();
	return updated && updated.value == replacement && payload[0] == std::byte{0x12} ? 0 : 8;
}
#elif defined(MODULE_VALUES_READER)
#include <integration/telemetry/v3/ValuesReader.hpp>

int main()
{
	namespace rt = telemetry::runtime;
	namespace v3 = resource::telemetry::v3;
	const auto descriptor = emptyRegistry();
	std::array<rt::RuntimeType, 12> types{};
	std::array<rt::RuntimeMember, 0> members{};
	std::array<rt::RuntimeEnumEntry, 0> enums{};
	std::array<rt::RuntimeCatalog, 0> catalogs{};
	std::array<rt::RuntimeEndpoint, 0> endpoints{};
	rt::RuntimeSchemaWorkspace workspace{types, members, enums, catalogs, endpoints};
	const auto parsed = rt::RuntimeSchema::parse(descriptor, workspace);
	if (parsed.status != rt::Status::Ok)
		return 1;
	// The values of a device with no Fields: the header alone, naming its descriptor.
	std::array<std::byte, v3::valuesHeaderBytes> values{};
	auto number = [&](std::size_t offset, std::uint64_t value, unsigned width) {
		for (unsigned i = 0; i < width; ++i)
			values[offset + i] = std::byte((value >> (8 * i)) & 0xffu);
	};
	for (std::size_t i = 0; i < v3::valuesMagic.size(); ++i)
		values[i] = v3::valuesMagic[i];
	number(4, v3::valuesMajor, 2);
	number(6, v3::valuesMinor, 2);
	number(12, values.size(), 4);
	number(16, parsed.schema.fingerprint(), 8);
	unsigned visits = 0;
	const auto read = v3::readValues(values, parsed.schema, [&](const v3::ValuesEntry&) { ++visits; });
	values[16] ^= std::byte{1};
	const auto other = v3::readValues(values, parsed.schema, [&](const v3::ValuesEntry&) { ++visits; });
	return read == v3::ValuesProblem::None && other == v3::ValuesProblem::Fingerprint && visits == 0 ? 0 : 2;
}
#elif defined(MODULE_DYNAMIC_CLIENT)
#include <integration/dynamic/DynamicClient.hpp>
#include <type_traits>
using Dynamic = telemetry::integration::dynamic::DynamicClient<2, 64>;
static_assert(!std::is_copy_constructible_v<Dynamic>);

int main()
{
	return sizeof(Dynamic) > sizeof(void*) ? 0 : 1;
}
#elif defined(MODULE_BATCH_STATIC_CLIENT)
#include <integration/batch/StaticClient.hpp>
#include <type_traits>
using StaticBatch = telemetry::integration::batch::StaticBatchBinding<>;
static_assert(std::is_copy_constructible_v<StaticBatch>);

int main()
{
	return StaticBatch{} ? 1 : 0;
}
#elif defined(MODULE_READ_MANY_STATIC_CLIENT)
#include <integration/read_many/StaticClient.hpp>
#include <type_traits>
using StaticBinding = telemetry::integration::read_many::StaticReadManyBinding<std::uint32_t>;
static_assert(std::is_copy_constructible_v<StaticBinding>);

int main()
{
	return StaticBinding{} ? 1 : 0;
}
#elif defined(MODULE_READ_MANY_CLIENT)
#include <integration/read_many/Client.hpp>
#include <type_traits>
using ReadManyBinding = telemetry::integration::read_many::ReadManyBinding;
static_assert(std::is_copy_constructible_v<ReadManyBinding>);

int main()
{
	return sizeof(ReadManyBinding) > sizeof(void*) ? 0 : 1;
}
#elif defined(MODULE_BATCH_CLIENT)
#include <integration/batch/Client.hpp>
#include <type_traits>
using BatchBinding = telemetry::integration::batch::BatchBinding;
static_assert(std::is_copy_constructible_v<BatchBinding>);

int main()
{
	return sizeof(BatchBinding) > sizeof(void*) ? 0 : 1;
}
#elif defined(MODULE_READ_MANY_WORKBENCH)
#include <integration/read_many/Workbench.hpp>
#include <type_traits>
using Workbench = telemetry::integration::workbench::WorkbenchModel<2, 130, 64, 768>;
static_assert(!std::is_copy_constructible_v<Workbench>);

int main()
{
	return sizeof(Workbench) > sizeof(void*) ? 0 : 1;
}
#elif defined(MODULE_WORKBENCH)
#include <integration/workbench/WorkbenchModel.hpp>
#include <type_traits>
using Workbench = telemetry::integration::workbench::WorkbenchModel<2, 64, 64>;
static_assert(!std::is_copy_constructible_v<Workbench>);
static_assert(!std::is_move_constructible_v<Workbench>);

int main()
{
	return sizeof(Workbench) > sizeof(void*) ? 0 : 1;
}
#elif defined(MODULE_INSPECTOR)
#include <integration/inspector/InspectorModel.hpp>
#include <type_traits>
using Inspector = telemetry::integration::inspector::InspectorModel<2, 64, 64>;
static_assert(!std::is_copy_constructible_v<Inspector>);
static_assert(!std::is_move_constructible_v<Inspector>);

int main()
{
	return sizeof(Inspector) > sizeof(void*) ? 0 : 1;
}
#elif defined(MODULE_LINK)
#include <integration/link/Framer.hpp>

// The interface alone: a framer that frames nothing still satisfies it.
class Refusing final : public telemetry::integration::link::Framer {
public:
	std::size_t maxFrameBytes() const noexcept override
	{
		return 0;
	}

	std::size_t maxEncodedBytes(std::size_t) const noexcept override
	{
		return 0;
	}

	std::size_t encode(telemetry::integration::link::Input,
	                   telemetry::integration::link::Output) noexcept override
	{
		return false;
	}

	telemetry::integration::link::Feed feed(telemetry::integration::link::Input) noexcept override
	{
		return {telemetry::integration::link::Status::Invalid, 0};
	}

	telemetry::integration::link::Input frame() const noexcept override
	{
		return {};
	}

	void release() noexcept override
	{}

	bool expect(telemetry::integration::link::Expect, resource::protocol::Op) noexcept override
	{
		return false;
	}

	void close() noexcept override
	{}

	void reset() noexcept override
	{}
};

int main()
{
	Refusing refusing;
	telemetry::integration::link::Framer& framer = refusing;
	return framer.feed({}).status == telemetry::integration::link::Status::Invalid &&
	               !framer.expect(telemetry::integration::link::Expect::EncodedReply,
	                              resource::protocol::Op::Read)
	           ? 0
	           : 1;
}
#elif defined(MODULE_TCP_LINK)
#include <integration/tcp/ProfileFramer.hpp>

int main()
{
	// Local, not static: a static framer would register its destructor with
	// __cxa_atexit, which a bare-metal image without start files cannot link.
	telemetry::integration::tcp::ProfileFramer profile;
	telemetry::integration::link::Framer& framer = profile;
	framer.reset();
	return framer.maxFrameBytes() == telemetry::integration::tcp::ProfileFramer::frameBytes &&
	               framer.expect(telemetry::integration::link::Expect::EncodedReply,
	                             resource::protocol::Op::Read)
	           ? 0
	           : 1;
}
#elif defined(MODULE_FILES_CLIENT)
#include <integration/files/Client.hpp>

// Any client with submitResource drives a reader; this one refuses.
struct Refusing {
	template<class F>
	telemetry::client::Submission submitResource(telemetry::client::Input, std::uint32_t, F&&,
	                                             std::uint32_t) noexcept
	{
		return {telemetry::client::Status::NotAdmitted, {}};
	}
};

struct Sink {
	bool operator()(telemetry::client::Input) noexcept
	{
		return true;
	}
};

int main()
{
	Refusing client;
	telemetry::integration::files::FileReader<Refusing, Sink> reader{client, Sink{}};
	return reader.start(0) == telemetry::client::Status::NotAdmitted &&
	               reader.state() == telemetry::integration::files::ReadState::Failed
	           ? 0
	           : 1;
}
#elif defined(MODULE_FILES_WORKBENCH)
#include <integration/files/Workbench.hpp>
#include <type_traits>
using Workbench = telemetry::integration::workbench::WorkbenchModel<2, 64, 64>;
using Files = telemetry::integration::files::Submitter<Workbench>;
static_assert(std::is_empty_v<Files>);

int main()
{
	return sizeof(Workbench) > sizeof(void*) ? 0 : 1;
}
#elif defined(MODULE_RESOURCE)
#include <resource/Resource.hpp>

int main()
{
	return resource::has(resource::FileFlag::Readable, resource::FileFlag::Readable) ? 0 : 1;
}
#elif defined(MODULE_RESOURCE_PROTOCOL)
#include <resource/protocol/Protocol.hpp>

int main()
{
	std::array<std::byte, 8> out{};
	return resource::protocol::process({}, {}, out).written == 1 ? 0 : 1;
}
#elif defined(MODULE_CLIENT)
#include <client/DeviceClient.hpp>
#include <client/ThreadSafeClient.hpp>

int main()
{
	telemetry::client::RequestQueue<2, 64> queue;
	(void)queue.admit(1);
	queue.close();
	queue.markDrained();
	return queue.drained() ? 0 : 1;
}
#elif defined(MODULE_RESOURCE_ADAPTER)
#include <telemetry/descriptor/Descriptor.hpp>
#include <integration/telemetry/v3/DescriptorFile.hpp>
#include <integration/telemetry/v3/IdentityFile.hpp>
#include <integration/telemetry/v3/ValuesFile.hpp>

int main()
{
	telemetry::descriptor::Descriptor descriptor{model};
	resource::telemetry::v3::DescriptorFile file{descriptor};
	std::array<std::byte, 64> out{};
	const auto result = file.read(0, out);
	// The device's identity, as firmware serves it: an MCU UID as its id.
	const std::array<std::byte, 12> uid{std::byte{0x00}, std::byte{0x28}, std::byte{0x00}, std::byte{0x3a}};
	std::array<char, 24> text{};
	resource::telemetry::v3::IdentityFile identity;
	const bool named = identity.set(telemetry::protocol::identity::hexId(uid, text), "Probe", "M7", "1.0") &&
	                   identity.rename("Probe 2");
	std::array<std::byte, telemetry::protocol::identity::writtenBytes> whole{};
	const auto served = identity.read(0, whole);
	telemetry::protocol::identity::Identity parsed;
	const bool read = served.eof &&
	                  telemetry::protocol::identity::parse(std::span(whole).first(served.written), parsed) ==
	                      telemetry::protocol::identity::Problem::None &&
	                  parsed.name == "Probe 2";
	return result.status == resource::Status::Ok && result.written == 64 && named && read ? 0 : 1;
}
#elif defined(MODULE_ENCODED_SERVER)
#include <integration/encoded/Dispatcher.hpp>

int main()
{
	std::array<std::byte, 256> scratch{};
	telemetry::Workspace workspace{scratch};
	const auto result =
	    telemetry::integration::encoded::dispatchEncoded(model.view(), workspace, 1, {}, {});
	return result.status == telemetry::integration::encoded::EncodedDispatchStatus::Malformed ? 0
	                                                                                          : 1;
}
#elif defined(MODULE_BATCH_SERVER)
#include <integration/encoded/BatchDispatcher.hpp>

int main()
{
	std::array<std::byte, 256> scratch{};
	telemetry::Workspace workspace{scratch};
	const telemetry::protocol::encoded::batch::Capability capability{1, 0};
	const auto result = telemetry::integration::encoded::dispatchEncoded(model.view(), workspace, 1,
	                                                                     {}, {}, capability);
	return result.status == telemetry::integration::encoded::EncodedDispatchStatus::Malformed ? 0
	                                                                                          : 1;
}
#elif defined(MODULE_TCP_SERVER)
#include <integration/tcp/Server.hpp>

int main()
{
	std::array<std::byte, 256> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 64> rx{}, tx{}, request{};
	telemetry::integration::tcp::Server server(model.view(), workspace, {}, rx, tx);
	const std::array<std::byte, 9> list{std::byte{1}};
	const auto listed = server.receive(list);
	if (listed.status != telemetry::integration::tcp::Status::Ready ||
	    listed.consumed != list.size() || server.reply().size() != 12 ||
	    server.reply()[0] != std::byte{0})
		return 1;
	server.releaseReply();
	telemetry::protocol::encoded::Header header{};
	header.session = 1;
	header.requestId = 1;
	const auto built = telemetry::protocol::encoded::write(request, header);
	if (!built)
		return 2;
	const auto accepted =
	    server.receive(telemetry::integration::tcp::Input{request}.first(built.written));
	const auto reply = telemetry::protocol::encoded::parse(server.reply());
	return accepted.status == telemetry::integration::tcp::Status::Ready && reply &&
	               reply.header.dispatch ==
	                   static_cast<unsigned>(telemetry::DispatchStatus::NotFound)
	           ? 0
	           : 3;
}
#elif defined(MODULE_RPC_SERVER) || defined(MODULE_RPC_SESSIONS)
#include <integration/telemetry/v3/RpcMailbox.hpp>
#if defined(MODULE_RPC_SESSIONS)
#include <integration/telemetry/v3/RpcSessionPool.hpp>
#endif
int main()
{
	auto view = model.view();
	std::array<std::byte, 256> scratch{};
	telemetry::Workspace workspace{scratch};
	std::array<std::byte, 64> request{}, response{};
	resource::telemetry::v3::RpcMailbox mailbox{view, workspace, request, response};
	(void)mailbox.process(0);
	return mailbox.state() == telemetry::protocol::rpc::RpcState::Idle ? 0 : 1;
}
#elif defined(MODULE_ENCODED_BACKEND)
#include <integration/encoded/Backend.hpp>
#include <integration/encoded/RetryingBackend.hpp>

int main()
{
	std::array<std::byte, 64> tx{}, rx{};
	telemetry::client::Clock clock{nullptr, +[](void*) noexcept -> std::uint32_t {
		                               return 0;
	                               }};
	telemetry::integration::encoded::EncodedBackend backend{tx, rx, clock};
	std::array<std::byte, 64> retryTx{}, retryRx{};
	telemetry::integration::encoded::RetryingEncodedBackend retry{
	    retryTx, retryRx, clock, {.replyTimeoutMs = 100, .maxRetries = 3}};
	return backend.openSession(1) && retry.openSession(1) ? 0 : 1;
}
#elif defined(MODULE_RPC_CLIENT) || defined(MODULE_RPC_BACKEND)
#include <client/rpc/RpcClient.hpp>
#if defined(MODULE_RPC_BACKEND)
#include <integration/rpc/Backend.hpp>
#endif
int main()
{
	std::array<std::byte, 64> request{}, result{}, packet{};
	telemetry::client::rpc::RpcClient client{0, request, result, packet};
#if defined(MODULE_RPC_BACKEND)
	telemetry::client::Clock clock{nullptr, +[](void*) noexcept -> std::uint32_t {
		                               return 0;
	                               }};
	telemetry::integration::rpc::RpcBackend backend{client, clock};
	return backend.openSession(1) ? 0 : 1;
#else
	return client.startSession(1) == telemetry::client::rpc::RpcClientStatus::Ok ? 0 : 1;
#endif
}
#endif
