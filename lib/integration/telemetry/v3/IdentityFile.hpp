/*
 * @file IdentityFile.hpp
 * @brief The device's identity served as /telemetry/identity.bin.
 * @author Ruslan Kovtun (shpegun60)
 * SPDX-License-Identifier: MIT
 *
 * The Resource provider of the identity file whose bytes wire/IdentityWire.hpp
 * defines: which unit the device is (an id that never changes), what its user
 * calls it, and optionally its model and firmware. The application adds it to
 * its own file table after its profile's files:
 *
 *   IdentityFile identity;
 *   identity.set(uid, savedName, "PA-3", "1.4.2");
 *   auto files = resource::filesystem(profile.systemFiles(),
 *       resource::file(telemetry::protocol::identity::path, identity), ...);
 *
 * It owns its bytes (at most identity::writtenBytes, twice: the current ones
 * and those of the read in progress), so the texts given to set() and
 * rename() need not outlive the call. No heap.
 *
 * IdentityFile is read-only: Stat says so. A device that lets a client rename
 * it serves its own class with write(), and with save() when it keeps the name
 * across a restart; Stat then says Writable, and Savable. check() tells it
 * whether the bytes only rename it:
 *
 *   struct DeviceIdentity : resource::telemetry::v3::IdentityFile {
 *       resource::WriteResult write(resource::Cursor cursor, resource::Input bytes,
 *                                   bool final) noexcept
 *       {
 *           std::string_view name;
 *           if (cursor != 0 || !final || check(bytes, name) != resource::Status::Ok ||
 *               !rename(name))
 *               return {resource::Status::InvalidData, cursor};
 *           const auto taken = static_cast<std::uint32_t>(bytes.size());
 *           return {resource::Status::Ok, taken, taken, true};
 *       }
 *       resource::SaveResult save(resource::Cursor cursor) noexcept
 *       {
 *           if (!settings.storeName(name()))
 *               return {resource::Status::InternalError, cursor};
 *           return {resource::Status::Ok, cursor, true};
 *       }
 *   };
 *
 * save() runs inside the request: on slow storage it keeps the name in
 * bounded steps - {Ok, cursor + 1, false} until done - rather than waiting on
 * an erase. Savable is what tells a client the name outlives a restart: a
 * device that keeps the name already inside write() still adds a save() that
 * answers {Ok, cursor, true}, and one without save() keeps it only until it
 * restarts.
 *
 * A client renames the device by writing the whole file in one WRITE (cursor 0,
 * final, at most writtenBytes), then SAVE when Stat says Savable, then reading
 * it back.
 */
#ifndef RESOURCE_TELEMETRY_V3_IDENTITY_FILE_HPP
#define RESOURCE_TELEMETRY_V3_IDENTITY_FILE_HPP
#pragma once

#include <resource/Types.hpp>
#include <wire/IdentityWire.hpp>
#include <array>
#include <cstring>
#include <string_view>

namespace resource::telemetry::v3 {
// Public methods:
// - IdentityFile(): Empty until set(): size 0, which a client reads as no identity.
// - set(): Replace every text; false, and nothing changed, when one is invalid.
// - rename(): Replace the name only; false when it is invalid or nothing was set.
// - id(), name(), model(), firmware(): The texts as served.
// - check(): Whether bytes written as this file only rename the device.
// - size(), read(): The Resource provider (read-only; a device's own class adds write()).
// - bytes(): The file as served.
//
// Call set() and rename() from the context that serves the file (or with it
// paused). A READ at cursor 0 takes a snapshot that the read's later pieces
// come from, so a rename between pieces never mixes two versions; the next read
// from 0 sees the new name. The whole file fits one READ on any transport that
// carries writtenBytes.
class IdentityFile {
	using Identity = ::telemetry::protocol::identity::Identity;

public:
	constexpr IdentityFile() noexcept = default;

	constexpr bool set(std::string_view id, std::string_view name, std::string_view model = {},
	                   std::string_view firmware = {}) noexcept
	{
		std::array<std::byte, ::telemetry::protocol::identity::writtenBytes> next{};
		const auto written =
		    ::telemetry::protocol::identity::write(Identity{id, name, model, firmware}, next);
		if (written == 0) {
			return false;
		}
		bytes_ = next;
		size_ = written;
		return true;
	}

	bool rename(std::string_view name) noexcept
	{
		if (size_ == 0) {
			return false;
		}
		// write() reads the other texts from the bytes it is about to replace,
		// so it writes into a copy.
		return set(id(), name, model(), firmware());
	}

	std::string_view id() const noexcept
	{
		return parsed().id;
	}

	std::string_view name() const noexcept
	{
		return parsed().name;
	}

	std::string_view model() const noexcept
	{
		return parsed().model;
	}

	std::string_view firmware() const noexcept
	{
		return parsed().firmware;
	}

	// Whether bytes, written whole as this file, only rename the device: an
	// identity with this id, model and firmware and a name it can hold. Ok, with
	// name viewing bytes; InvalidData otherwise, and when nothing was set.
	[[nodiscard]] Status check(Input bytes, std::string_view& name) const noexcept
	{
		Identity incoming;
		if (size_ == 0 || bytes.size() > ::telemetry::protocol::identity::writtenBytes ||
		    ::telemetry::protocol::identity::parse(bytes, incoming) !=
		        ::telemetry::protocol::identity::Problem::None) {
			return Status::InvalidData;
		}
		const auto current = parsed();
		if (incoming.id != current.id || incoming.model != current.model ||
		    incoming.firmware != current.firmware) {
			return Status::InvalidData;
		}
		name = incoming.name;
		return Status::Ok;
	}

	[[nodiscard]] constexpr FileSize size() const noexcept
	{
		return static_cast<FileSize>(size_);
	}

	[[nodiscard]] ReadResult read(Cursor cursor, Output output) const noexcept
	{
		if (cursor == 0 && !output.empty()) {
			served_ = bytes_;
			servedSize_ = size_;
		}
		const auto size = cursor == 0 ? size_ : servedSize_;
		if (cursor > size) {
			return {Status::InvalidCursor, cursor};
		}
		if (cursor == size) {
			return {Status::Ok, cursor, 0, true};
		}
		if (output.empty()) {
			return {Status::BufferTooSmall, cursor};
		}
		const auto remaining = size - static_cast<std::size_t>(cursor);
		const auto count = output.size() < remaining ? output.size() : remaining;
		std::memcpy(output.data(), served_.data() + cursor, count);
		const auto next = cursor + count;
		return {Status::Ok, next, static_cast<std::uint32_t>(count), next == size};
	}

	std::span<const std::byte> bytes() const noexcept
	{
		return {bytes_.data(), size_};
	}

private:
	Identity parsed() const noexcept
	{
		Identity identity;
		::telemetry::protocol::identity::parse(bytes(), identity);
		return identity;
	}

	std::array<std::byte, ::telemetry::protocol::identity::writtenBytes> bytes_{};
	std::size_t size_ = 0;
	// The bytes of the read in progress, taken at its cursor 0.
	mutable std::array<std::byte, ::telemetry::protocol::identity::writtenBytes> served_{};
	mutable std::size_t servedSize_ = 0;
};

} // namespace resource::telemetry::v3
#endif
