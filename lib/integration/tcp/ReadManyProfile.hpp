/* Optional Resource/TCP ReadMany bootstrap, with one connection-owned session.
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_INTEGRATION_TCP_READ_MANY_PROFILE_HPP
#define TELEMETRY_INTEGRATION_TCP_READ_MANY_PROFILE_HPP
#pragma once
#include <resource/BytesFile.hpp>
#include <resource/FileSystem.hpp>
#include <wire/ReadManyWire.hpp>

namespace telemetry::integration::tcp {
class Server;

// Stable, noncopyable per-connection providers. The Descriptor bytes outlive
// this object, which in turn outlives Server and every borrowed Resource reply.
// A normal Resource WRITE (cursor zero, final, four LE bytes) chooses the host's
// nonzero session exactly once. It invokes no Model getter or mutation. Reads
// and STAT never choose a session; reconnect requires a fresh profile object.
class ReadManyProfile {
	using Wire = protocol::encoded::read_many::Capability;

	struct SessionFile {
		ReadManyProfile& owner;

		resource::FileSize size() const noexcept
		{
			return 4;
		}

		resource::WriteResult write(resource::Cursor cursor, resource::Input bytes,
		                            bool final) noexcept
		{
			if (cursor != 0 || !final || bytes.size() != 4 || owner.bound_ || owner.frozen_)
				return {resource::Status::InvalidData, cursor};
			std::uint32_t session = 0;
			for (unsigned i = 0; i < 4; ++i)
				session |= std::uint32_t(std::to_integer<unsigned>(bytes[i])) << (8 * i);
			auto candidate = owner.capability_;
			candidate.session = session;
			const auto result =
			    protocol::encoded::read_many::writeCapability(owner.bytes_, candidate);
			if (!result)
				return {resource::Status::InvalidData, cursor};
			owner.capability_ = candidate;
			owner.bound_ = true;
			return {resource::Status::Ok, 4, 4, true};
		}
	};

	struct CapabilityFile {
		ReadManyProfile& owner;

		resource::FileSize size() const noexcept
		{
			return protocol::encoded::read_many::capabilityBytes;
		}

		resource::ReadResult read(resource::Cursor cursor, resource::Output bytes) const noexcept
		{
			if (!owner.bound_)
				return {resource::Status::InvalidData, cursor};
			return resource::BytesFile{std::span<const std::byte>{owner.bytes_}}.read(cursor,
			                                                                          bytes);
		}
	};

public:
	explicit ReadManyProfile(
	    std::span<const std::byte> descriptor, std::uint64_t fingerprint,
	    std::uint8_t itemLimit = protocol::encoded::read_many::maxItems,
	    std::uint16_t requestLimit = protocol::encoded::read_many::maxRequestPayload,
	    std::uint16_t replyLimit = protocol::encoded::read_many::maxReplyPayload) noexcept
	    : descriptor_(descriptor), capability_{0, fingerprint, itemLimit, requestLimit, replyLimit},
	      sessionFile_{*this}, capabilityFile_{*this},
	      files_{std::array<resource::FileEntry, 3>{
	          resource::file("/telemetry/descriptor.bin", descriptor_),
	          resource::file("/telemetry/session.bin", sessionFile_),
	          resource::file("/telemetry/read-many.bin", capabilityFile_)}}
	{}

	ReadManyProfile(const ReadManyProfile&) = delete;
	ReadManyProfile& operator=(const ReadManyProfile&) = delete;
	ReadManyProfile(ReadManyProfile&&) = delete;
	ReadManyProfile& operator=(ReadManyProfile&&) = delete;

	resource::FileSystemView files() const noexcept
	{
		return files_.view();
	}

	const Wire& capability() const noexcept
	{
		return capability_;
	}

	bool bound() const noexcept
	{
		return bound_;
	}

	bool frozen() const noexcept
	{
		return frozen_;
	}

private:
	friend class Server;
	resource::BytesFile descriptor_;
	Wire capability_;
	std::array<std::byte, protocol::encoded::read_many::capabilityBytes> bytes_{};
	bool bound_ = false, frozen_ = false;
	SessionFile sessionFile_;
	CapabilityFile capabilityFile_;
	resource::FileSystem<3> files_;
};
} // namespace telemetry::integration::tcp
#endif
