/* Bounded integration example, not a transport framework in the library. MIT. */

#ifndef TELEMETRY_TESTS_STRUCTURED_EXCHANGE_FAKETRANSPORT_HPP
#define TELEMETRY_TESTS_STRUCTURED_EXCHANGE_FAKETRANSPORT_HPP
#pragma once

#include "Fixture.hpp"
#include <resource/protocol/Protocol.hpp>
#include <optional>
#include <utility>

namespace example {
using namespace fixture;

// This generation belongs to the fake connection layer, never a telemetry
// packet. A real transport must already exclude frames from old connections.
struct Connection {
	unsigned position;
	std::uint32_t generation;
};

// Two bounded peer contexts exercise admission, stale generations and shared scratch.
// API: open/close/reboot own fake lifecycles; bind/exchange/resource route complete
// packets; queue/drain defer controls; canCloseDuringHandler checks active-use refusal.
class Transport {
	// Per-peer data owns agreement state, scratch and its live Values provider.
	struct Peer {
		bool admitted = false;
		bool busy = false;
		std::uint32_t generation = 0;
		rs::Binding binding;
		std::array<std::byte, model.maxScratch()> storage{};
		ts::Workspace workspace{storage};
		// The Values file of the descriptor's model, whatever its form.
		decltype(resource::telemetry::v3::ValuesFile{
		    descriptor, std::declval<ts::Workspace&>()}) values{descriptor, workspace};
	};

	std::array<Peer, 2> peers_{};

	struct Queued {
		Connection connection{};
		std::array<std::byte, 24> packet{};
		bool used = false;
	};

	std::array<Queued, 2> queue_{};

	Peer* get(Connection connection) noexcept
	{
		if (connection.position >= peers_.size())
			return nullptr;
		auto& peer = peers_[connection.position];
		return peer.admitted && peer.generation == connection.generation ? &peer : nullptr;
	}

	// Scope guard keeps a peer busy throughout a synchronous handler.
	// API: Active() sets busy; ~Active() releases it.
	struct Active {
		Peer& peer;

		explicit Active(Peer& value) : peer(value)
		{
			peer.busy = true;
		}

		~Active()
		{
			peer.busy = false;
		}
	};

public:
	unsigned agreements = 0;

	[[nodiscard]] std::optional<Connection> open() noexcept
	{
		for (unsigned i = 0; i < peers_.size(); ++i) {
			auto& peer = peers_[i];
			if (peer.admitted || peer.generation == UINT32_MAX)
				continue;
			peer.admitted = true;
			++peer.generation;
			peer.binding.reset();
			return Connection{i, peer.generation};
		}
		return std::nullopt; // Includes Unbound peers; no silent eviction.
	}

	bool close(Connection connection) noexcept
	{
		auto* peer = get(connection);
		if (!peer || peer->busy)
			return false;
		peer->binding.reset();
		for (auto& queued : queue_)
			if (queued.used && queued.connection.position == connection.position &&
			    queued.connection.generation == connection.generation)
				queued.used = false;
		peer->admitted = false;
		return true;
	}

	void reboot() noexcept
	{
		for (unsigned i = 0; i < peers_.size(); ++i)
			if (peers_[i].admitted) {
				const bool closed = close({i, peers_[i].generation});
				assert(closed);
			}
	}

	rs::PacketResult bind(Connection connection, Input request, Output response) noexcept
	{
		auto* peer = get(connection);
		if (!peer || peer->busy)
			return {D::NotReady};
		Active active{*peer};
		++agreements; // Only here is the cached fingerprint supplied/compared.
		return rs::Bind::process(peer->binding, view, descriptor.fingerprint(), request, response);
	}

	rs::PacketResult exchange(Connection connection, Input request, Output response) noexcept
	{
		auto* peer = get(connection);
		if (!peer || peer->busy)
			return {D::NotReady};
		Active active{*peer};
		return rs::Exchange::process(peer->binding, request, response, peer->workspace);
	}

	std::optional<resource::protocol::Reply> resource(Connection connection, Input request,
	                                                  Output response) noexcept
	{
		auto* peer = get(connection);
		if (!peer || peer->busy || request.empty())
			return std::nullopt;
		// Discovery and descriptor access do not depend on Ready. Values do.
		const auto op = static_cast<resource::protocol::Op>(request[0]);
		const bool discovery =
		    op == resource::protocol::Op::List || op == resource::protocol::Op::Stat ||
		    (op == resource::protocol::Op::Read && request.size() == 13 && get32(request, 1) == 0);
		if (!discovery && !peer->binding.ready())
			return std::nullopt;
		Active active{*peer};
		const auto files = resource::filesystem(resource::file("/descriptor.bin", descriptorFile),
		                                        resource::file("/values.bin", peer->values));
		return resource::protocol::process(files.view(), request, response);
	}

	bool queue(Connection connection, const std::array<std::byte, 24>& packet) noexcept
	{
		if (!get(connection))
			return false;
		for (auto& item : queue_)
			if (!item.used) {
				item = {connection, packet, true};
				return true;
			}
		return false;
	}

	void drain() noexcept
	{
		std::array<std::byte, 24> output{};
		for (auto& item : queue_)
			if (item.used) {
				(void)exchange(item.connection, item.packet, output);
				item.used = false;
			}
	}

	// Simulate a handler holding the context while a close is attempted.
	bool canCloseDuringHandler(Connection connection) noexcept
	{
		auto* peer = get(connection);
		assert(peer);
		Active active{*peer};
		return close(connection);
	}
};

// Minimal client-side correlation example. Timeout leaves a slot occupied:
// only an exact late reply or a connection reset establishes a reuse boundary.
// Two correlation slots stay occupied until an exact reply or connection reset.
// API: add allocates a request ID; complete matches ID/op/endpoint; reset clears slots.
class Pending {
	struct Entry {
		bool used = false;
		std::uint32_t request = 0, endpoint = 0;
		Op operation{};
	};

	std::array<Entry, 2> entries_{};

public:
	std::uint32_t next = 0;

	std::optional<std::uint32_t> add(Op operation, std::uint32_t endpoint) noexcept
	{
		auto empty = std::find_if(entries_.begin(), entries_.end(), [](const auto& e) {
			return !e.used;
		});
		if (empty == entries_.end())
			return std::nullopt;
		while (std::any_of(entries_.begin(), entries_.end(), [&](const auto& e) {
			return e.used && e.request == next;
		}))
			++next;
		const auto chosen = next++;
		*empty = {true, chosen, endpoint, operation};
		return chosen;
	}

	bool complete(std::uint32_t request, Op operation, std::uint32_t endpoint) noexcept
	{
		for (auto& e : entries_)
			if (e.used && e.request == request && e.endpoint == endpoint &&
			    e.operation == operation) {
				e.used = false;
				return true;
			}
		return false;
	}

	void reset() noexcept
	{
		entries_ = {};
	}
};
} // namespace example

#endif // TELEMETRY_TESTS_STRUCTURED_EXCHANGE_FAKETRANSPORT_HPP
