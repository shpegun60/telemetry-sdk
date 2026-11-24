/* Stable mailbox, model and Resource fixtures for session qualification. MIT. */
#ifndef TELEMETRY_TESTS_STRUCTURED_RPC_SESSIONS_FIXTURE_HPP
#define TELEMETRY_TESTS_STRUCTURED_RPC_SESSIONS_FIXTURE_HPP
#pragma once

#include <integration/telemetry/v3/RpcSessionPool.hpp>
#include <telemetry/Telemetry.hpp>
#include <algorithm>
#include <array>
#include <utility>

namespace rpc_sessions_fixture {
namespace ts = ::telemetry;
namespace rpc = resource::telemetry::v3;
namespace pc = resource::protocol::client;
using resource::Input;
using resource::Output;
using Status = rpc::RpcSessionStatus;

struct Add {
	std::uint32_t amount;
};

struct Report {
	std::uint32_t value;
};

struct Counter {
	std::uint32_t value = 0;
	unsigned calls = 0;
	unsigned depth = 0;
	unsigned maxDepth = 0;
	void (*hook)() noexcept = nullptr;

	ts::ServiceResult<Report> add(const Add& request) noexcept
	{
		++calls;
		++depth;
		maxDepth = std::max(maxDepth, depth);
		value += request.amount;
		if (hook)
			hook();
		--depth;
		return ts::ServiceResult<Report>::success(Report{value});
	}
};

inline Counter first, second;
inline constexpr ts::Model firstModel{
    ts::group("counter", ts::service<&Counter::add>("Add", first))};
inline constexpr ts::Model secondModel{
    ts::group("counter", ts::service<&Counter::add>("Add", second))};
inline constexpr auto firstView = firstModel.view();
inline constexpr auto secondView = secondModel.view();
inline constexpr auto AddId = ts::idOf<firstModel, "counter/Add">();
static_assert(AddId == ts::idOf<secondModel, "counter/Add">());

// A second file exercises routing and cannot touch a mailbox or Workspace.
struct ExtraFile {
	unsigned writes = 0;
	std::byte value{0x5a};

	constexpr resource::FileSize size() const noexcept
	{
		return 1;
	}

	resource::ReadResult read(resource::Cursor at, Output output) const noexcept
	{
		if (at > 1)
			return {resource::Status::InvalidCursor, at};
		if (at == 1)
			return {resource::Status::Ok, 1, 0, true};
		if (output.empty())
			return {resource::Status::BufferTooSmall, at};
		output[0] = value;
		return {resource::Status::Ok, 1, 1, true};
	}

	resource::WriteResult write(resource::Cursor at, Input input, bool final) noexcept
	{
		if (at != 0 || input.size() != 1 || !final)
			return {resource::Status::InvalidData, at};
		++writes;
		value = input[0];
		return {resource::Status::Ok, 1, 1, true};
	}
};

struct Slot {
	std::array<std::byte, 66> request{};
	std::array<std::byte, 34> result{};
	rpc::RpcMailbox mailbox;
	ExtraFile extra;
	const resource::FileSystem<2> files;

	Slot(const ts::ModelView& view, ts::Workspace& workspace) noexcept
	    : mailbox(view, workspace, Output{request}.subspan(1, 64), Output{result}.subspan(1, 32)),
	      files(resource::filesystem(resource::file("/telemetry/rpc.bin", mailbox),
	                                 resource::file("/shared.bin", extra)))
	{
		request.front() = request.back() = std::byte{0xcd};
		result.front() = result.back() = std::byte{0xcd};
	}

	bool intact() const noexcept
	{
		return request.front() == std::byte{0xcd} && request.back() == std::byte{0xcd} &&
		       result.front() == std::byte{0xcd} && result.back() == std::byte{0xcd};
	}
};

template<std::size_t N>
struct Harness {
	std::array<std::byte, 128> scratch{};
	ts::Workspace workspace{scratch};
	Slot a{firstView, workspace};
	Slot b;
	Slot c{firstView, workspace};
	Slot d{firstView, workspace};
	const std::array<rpc::RpcSessionBinding, 4> all;
	rpc::RpcSessionPool<N> pool;

	template<std::size_t... I>
	auto bindings(std::index_sequence<I...>) noexcept
	{
		return std::array<rpc::RpcSessionBinding, N>{all[I]...};
	}

	explicit Harness(bool differentModel = false) noexcept
	    : b(differentModel ? secondView : firstView, workspace),
	      all{rpc::RpcSessionBinding{a.mailbox, a.files.view()},
	          rpc::RpcSessionBinding{b.mailbox, b.files.view()},
	          rpc::RpcSessionBinding{c.mailbox, c.files.view()},
	          rpc::RpcSessionBinding{d.mailbox, d.files.view()}},
	      pool(bindings(std::make_index_sequence<N>{}))
	{}

	Slot& slot(std::size_t index) noexcept
	{
		return index == 0 ? a : index == 1 ? b : index == 2 ? c : d;
	}
};

template<class U>
inline void put(Output output, std::size_t at, U value) noexcept
{
	for (std::size_t i = 0; i < sizeof(U); ++i)
		output[at + i] = std::byte(value >> (8 * i));
}

inline auto request(std::uint32_t amount, std::uint32_t tx = 1) noexcept
{
	std::array<std::byte, ::telemetry::protocol::rpc::rpcRequestHeaderBytes + ts::wireSize<Add>>
	    output{};
	output[0] = std::byte{'T'};
	output[1] = std::byte{'R'};
	output[2] = std::byte{'P'};
	output[3] = std::byte{'C'};
	output[4] = std::byte{1};
	put(output, 8, tx);
	put(output, 12, AddId);
	put(output, 16, std::uint32_t{ts::wireSize<Add>});
	output[20] = std::byte(::telemetry::protocol::rpc::RpcOperation::Service);
	(void)ts::encode(Add{amount},
	                 Output{output}.subspan(::telemetry::protocol::rpc::rpcRequestHeaderBytes));
	return output;
}

inline auto writePacket(Input bytes, resource::Cursor cursor = 0, bool final = true,
                        resource::FileIndex file = 0) noexcept
{
	std::array<std::byte, 128> output{};
	const auto result = pc::makeWrite(output, file, cursor, bytes, final);
	if (result.status != pc::BuildStatus::Ok)
		std::abort();
	return std::pair{output, result.written};
}

template<std::size_t N>
inline auto submit(rpc::RpcSessionPool<N>& pool, rpc::RpcSessionHandle session, Input bytes,
                   Output response, resource::Cursor cursor = 0, bool final = true,
                   resource::FileIndex file = 0) noexcept
{
	const auto packet = writePacket(bytes, cursor, final, file);
	return pool.handle(session, Input{packet.first}.first(packet.second), response);
}
} // namespace rpc_sessions_fixture
#endif // TELEMETRY_TESTS_STRUCTURED_RPC_SESSIONS_FIXTURE_HPP
