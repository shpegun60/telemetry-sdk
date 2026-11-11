/*
 * @file Framer.hpp
 * @brief How one medium cuts its byte stream into the client's reply frames.
 * @author Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT.
 *
 * A host transport is a link (bytes in and out of one medium) plus a framer
 * (where a frame starts and ends on that medium). The framer owns no I/O and
 * no Qt: it turns one outgoing packet into the bytes to write and finds reply
 * frames in whatever chunks the link delivers. The transport above it keeps
 * every exchange rule - discovery, ordering, deadlines - medium-neutral.
 *
 * Contract, the same for every framer:
 * - feed() consumes a prefix of its input and reports it; the caller keeps the
 *   unconsumed suffix and offers it again. At most one frame becomes Ready.
 * - A Ready frame borrows the framer's storage until release().
 * - Invalid latches until reset(): a broken stream is never resynchronised
 *   inside one connection.
 * - expect() changes how replies are read only at an empty frame boundary and
 *   reports false otherwise.
 * - reset() starts a new connection boundary; close() latches a closed one.
 */
#ifndef TELEMETRY_INTEGRATION_LINK_FRAMER_HPP
#define TELEMETRY_INTEGRATION_LINK_FRAMER_HPP
#pragma once

#include <resource/Types.hpp>
#include <resource/protocol/Wire.hpp>
#include <cstddef>
#include <cstdint>

namespace telemetry::integration::link {
using Input = resource::Input;
using Output = resource::Output;

// The reply the client waits for next. A Resource reply has no identity of its
// own, so its framing needs the one outstanding operation.
enum class Expect : std::uint8_t {
	ResourceReply,
	EncodedReply
};

enum class Status : std::uint8_t {
	NeedMore,
	Ready,
	Invalid
};

struct Feed {
	Status status = Status::NeedMore;
	std::size_t consumed = 0;
};

class Framer {
public:
	Framer() = default;
	Framer(const Framer&) = delete;
	Framer& operator=(const Framer&) = delete;
	virtual ~Framer() = default;

	// The largest frame this framer delivers and the largest packet it encodes.
	[[nodiscard]] virtual std::size_t maxFrameBytes() const noexcept = 0;
	// An upper bound on what encode() writes for a packet of packetBytes, found
	// without reading the packet; zero when no packet of that size fits.
	[[nodiscard]] virtual std::size_t maxEncodedBytes(std::size_t packetBytes) const noexcept = 0;
	// Writes the medium's bytes for one complete packet into out in one pass and
	// returns how many; zero refuses it and leaves out unspecified.
	[[nodiscard]] virtual std::size_t encode(Input packet, Output out) noexcept = 0;
	[[nodiscard]] virtual Feed feed(Input bytes) noexcept = 0;
	[[nodiscard]] virtual Input frame() const noexcept = 0;
	virtual void release() noexcept = 0;
	// The operation matters only for a Resource reply, which has no identity
	// of its own; pass it explicitly either way.
	[[nodiscard]] virtual bool expect(Expect reply, resource::protocol::Op operation) noexcept = 0;
	virtual void close() noexcept = 0;
	// A new connection boundary. No reply is expected until expect().
	virtual void reset() noexcept = 0;
};
} // namespace telemetry::integration::link
#endif
