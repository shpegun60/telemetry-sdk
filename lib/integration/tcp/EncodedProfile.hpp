/* Explicit combined Resource/TCP TRM1+TBM1 profile. MIT. */
#ifndef TELEMETRY_INTEGRATION_TCP_ENCODED_PROFILE_HPP
#define TELEMETRY_INTEGRATION_TCP_ENCODED_PROFILE_HPP
#pragma once
#include <integration/tcp/Framing.hpp>
#include <integration/tcp/ReadManyProfile.hpp>
#include <integration/encoded/BatchDispatcher.hpp>
#include <integration/encoded/Dispatcher.hpp>

namespace telemetry::integration::tcp {
// One write-once session shared by both optional capabilities. Reuses the
// ReadMany profile's control provider; no second handshake or execution queue.
// Descriptor and this stable object outlive Server and physical TX completion.
class EncodedProfile {
	using BatchCapability = protocol::encoded::batch::Capability;

	struct SessionFile {
		EncodedProfile& owner;

		resource::FileSize size() const noexcept
		{
			return 4;
		}

		resource::WriteResult write(resource::Cursor cursor, resource::Input bytes,
		                            bool final) noexcept
		{
			auto candidate = owner.batch_;
			candidate.session = 1;
			if (owner.frozen_ || !protocol::encoded::batch::valid(candidate))
				return {resource::Status::InvalidData, cursor};
			const auto result = owner.readMany_.files().write(1, cursor, bytes, final);
			if (result.status == resource::Status::Ok) {
				candidate.session = owner.readMany_.capability().session;
				owner.batch_ = candidate;
			}
			return result;
		}
	};

	struct ReadManyFile {
		EncodedProfile& owner;

		resource::FileSize size() const noexcept
		{
			return protocol::encoded::read_many::capabilityBytes;
		}

		resource::ReadResult read(resource::Cursor cursor, resource::Output output) const noexcept
		{
			return owner.readMany_.files().read(2, cursor, output);
		}
	};

	struct BatchFile {
		EncodedProfile& owner;

		resource::FileSize size() const noexcept
		{
			return protocol::encoded::batch::capabilityBytes;
		}

		resource::ReadResult read(resource::Cursor cursor, resource::Output output) const noexcept
		{
			if (!owner.bound())
				return {resource::Status::InvalidData, cursor};
			std::array<std::byte, protocol::encoded::batch::capabilityBytes> bytes{};
			if (!protocol::encoded::batch::writeCapability(bytes, owner.batch_))
				return {resource::Status::InvalidData, cursor};
			return resource::BytesFile{bytes}.read(cursor, output);
		}
	};

public:
	explicit EncodedProfile(
	    resource::Input descriptor, std::uint64_t fingerprint,
	    std::uint8_t itemLimit = protocol::encoded::batch::maxItems,
	    std::uint16_t requestLimit = protocol::encoded::batch::maxRequestPayload,
	    std::uint16_t replyLimit = protocol::encoded::batch::maxReplyPayload) noexcept
	    : descriptor_(descriptor), readMany_(descriptor, fingerprint),
	      batch_{0, fingerprint, itemLimit, requestLimit, replyLimit}, sessionFile_{*this},
	      readManyFile_{*this}, batchFile_{*this},
	      files_{std::array<resource::FileEntry, 4>{
	          resource::file("/telemetry/descriptor.bin", descriptor_),
	          resource::file("/telemetry/session.bin", sessionFile_),
	          resource::file("/telemetry/read-many.bin", readManyFile_),
	          resource::file("/telemetry/batch.bin", batchFile_)}}
	{}

	EncodedProfile(const EncodedProfile&) = delete;
	EncodedProfile& operator=(const EncodedProfile&) = delete;
	EncodedProfile(EncodedProfile&&) = delete;
	EncodedProfile& operator=(EncodedProfile&&) = delete;

	resource::FileSystemView files() const noexcept
	{
		return files_.view();
	}

	const auto& readManyCapability() const noexcept
	{
		return readMany_.capability();
	}

	const BatchCapability& batchCapability() const noexcept
	{
		return batch_;
	}

	bool bound() const noexcept
	{
		return readMany_.bound();
	}

	bool frozen() const noexcept
	{
		return frozen_;
	}

private:
	friend class Server;

	encoded::EncodedDispatchResult dispatch(ModelView model, Workspace& workspace,
	                                        std::uint32_t session, Input request,
	                                        Output response) const noexcept
	{
		const auto parsed = protocol::encoded::parse(request);
		if (parsed && parsed.header.operation == protocol::encoded::Operation::Batch)
			return encoded::dispatchEncoded(model, workspace, session, request, response, batch_);
		return encoded::dispatchEncoded(model, workspace, session, request, response,
		                                readMany_.capability());
	}

	resource::BytesFile descriptor_;
	ReadManyProfile readMany_;
	BatchCapability batch_;
	bool frozen_ = false;
	SessionFile sessionFile_;
	ReadManyFile readManyFile_;
	BatchFile batchFile_;
	resource::FileSystem<4> files_;
};
} // namespace telemetry::integration::tcp
#endif
