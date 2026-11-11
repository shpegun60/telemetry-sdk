/*
 * @file ProfileFramer.hpp
 * @brief The TCP reference profile's client framer: no extra wire bytes.
 * @author Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT.
 *
 * Resource and Encoded packets delimit themselves on a TCP stream, so encoding
 * is a copy and reply framing is FrameReader over storage this object owns.
 * Every FrameReader failure (invalid, oversized, closed, blocked) reads as
 * Invalid: the transport treats them all as one broken stream.
 */
#ifndef TELEMETRY_INTEGRATION_TCP_PROFILE_FRAMER_HPP
#define TELEMETRY_INTEGRATION_TCP_PROFILE_FRAMER_HPP
#pragma once

#include <integration/link/Framer.hpp>
#include <integration/tcp/Framing.hpp>
#include <array>
#include <cstring>

namespace telemetry::integration::tcp {
class ProfileFramer final : public link::Framer {
public:
	// One Encoded frame of the largest body the client accepts, with its header.
	static constexpr std::size_t frameBytes = 65568;

	[[nodiscard]] std::size_t maxFrameBytes() const noexcept override
	{
		return storage_.size();
	}

	[[nodiscard]] std::size_t encodedBytes(link::Input packet) const noexcept override
	{
		return packet.size() <= storage_.size() ? packet.size() : 0;
	}

	[[nodiscard]] bool encode(link::Input packet, link::Output out) noexcept override
	{
		if (packet.empty() || out.size() != encodedBytes(packet))
			return false;
		std::memcpy(out.data(), packet.data(), packet.size());
		return true;
	}

	[[nodiscard]] link::Feed feed(link::Input bytes) noexcept override
	{
		const auto fed = reader_.feed(bytes);
		const auto status = fed.status == Status::NeedMore ? link::Status::NeedMore
		                    : fed.status == Status::Ready  ? link::Status::Ready
		                                                   : link::Status::Invalid;
		return {status, fed.consumed};
	}

	[[nodiscard]] link::Input frame() const noexcept override
	{
		return reader_.frame();
	}

	void release() noexcept override
	{
		reader_.release();
	}

	[[nodiscard]] bool expect(link::Expect reply,
	                          resource::protocol::Op operation) noexcept override
	{
		return reply == link::Expect::EncodedReply ? reader_.expect(Mode::EncodedReply)
		                                           : reader_.expect(Mode::ResourceReply, operation);
	}

	void close() noexcept override
	{
		reader_.close();
	}

	void reset() noexcept override
	{
		reader_.reset();
	}

private:
	std::array<std::byte, frameBytes> storage_{};
	FrameReader reader_{storage_, Mode::ResourceReply, resource::protocol::Op::List};
};
} // namespace telemetry::integration::tcp
#endif
