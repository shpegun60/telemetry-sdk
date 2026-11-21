/* Explicit provider composition, with no init() lifecycle. MIT. */
#include "DeviceResources.hpp"
#ifdef WITH_V3
#include "Fixture.hpp"
#endif
#ifdef WITH_RPC
#include <integration/telemetry/v3/RpcMailbox.hpp>
#include <telemetry/model/Model.hpp>
#endif
#ifdef WITH_RPC_CLIENT
#include <client/rpc/RpcClient.hpp>
#endif
#ifdef WITH_RPC_SESSIONS
#include <integration/telemetry/v3/RpcSessionPool.hpp>
#endif

namespace {
// Independent generic byte provider proves the core-only build needs no telemetry types.
// API: size(), read().
struct Plain {
	constexpr resource::FileSize size() const noexcept
	{
		return 0;
	}

	resource::ReadResult read(resource::Cursor c, resource::Output) const noexcept
	{
		return {c == 0 ? resource::Status::Ok : resource::Status::InvalidCursor, c, 0, c == 0};
	}
};

constexpr Plain plain;
#ifdef WITH_RPC
// Deliberately independent of Fixture.hpp: RPC-only must not need Values.cpp.
std::uint32_t rpcReads = 41;

std::uint32_t readRpcCounter() noexcept
{
	return ++rpcReads;
}

constexpr telemetry::Model rpcModel{
    telemetry::group("rpc", telemetry::field<&readRpcCounter>("Counter"))};
constexpr auto rpcView = rpcModel.view();
std::array<std::byte, rpcModel.maxScratch()> rpcScratch{};
telemetry::Workspace rpcWorkspace{rpcScratch};
std::array<std::byte, 24> rpcRequest{};
std::array<std::byte, 4> rpcResult{};
resource::telemetry::v3::RpcMailbox rpcMailbox{rpcView, rpcWorkspace, rpcRequest, rpcResult};
#endif
constinit auto files =
    resource::filesystem(resource::file("/plain", plain)
#ifdef WITH_V3
                             ,
                         resource::file("/v3/descriptor.bin", fixture::packedFile),
                         resource::file("/v3/values.bin", fixture::values)
#endif
#ifdef WITH_RPC
                             ,
                         resource::file("/telemetry/rpc.bin", rpcMailbox)
#endif
    );
} // namespace

resource::FileSystemView deviceResources() noexcept
{
	return files.view();
}

void processDeviceResources(std::uint32_t nowMs) noexcept
{
#ifdef WITH_RPC
	rpcMailbox.process(nowMs);
#else
	(void)nowMs;
#endif
}

#ifdef WITH_RPC_CLIENT
bool exerciseDeviceRpcClient() noexcept
{
	using namespace ::telemetry::client::rpc;
	std::array<std::byte, 24> request{};
	std::array<std::byte, 4> result{};
	std::array<std::byte, 64> packet{};
	RpcClient client{0, request, result, packet};
	if (client.startSession(1) != RpcClientStatus::Ok ||
	    client.beginReadField(FieldEndpoint<std::uint32_t>{0, trustedRpcMapping}) !=
	        RpcClientStatus::Ok)
		return false;
	client.process(0);
	const auto outgoing = client.nextPacket();
	if (!outgoing || !client.onSendAccepted(outgoing.generation, outgoing.requestId))
		return false;
	client.loseSession(1);
	return !client.active();
}
#endif

#ifdef WITH_RPC_SESSIONS
bool exerciseDeviceRpcSessions() noexcept
{
	using namespace resource::telemetry::v3;
	std::array<RpcSessionBinding, 1> bindings{RpcSessionBinding{
	    rpcMailbox, files.view(), static_cast<resource::FileIndex>(files.view().fileCount() - 1)}};
	RpcSessionPool<1> pool{bindings};
	const auto admitted = pool.acquire({17, 1, 0});
	if (admitted.status != RpcSessionStatus::Ok || !pool.valid(admitted.session))
		return false;
	const auto processed = pool.process(10, 0);
	return processed.status == RpcSessionStatus::Ok && processed.dispatched == 0 &&
	       pool.release(admitted.session) == RpcSessionStatus::Ok && !pool.valid(admitted.session);
}
#endif
