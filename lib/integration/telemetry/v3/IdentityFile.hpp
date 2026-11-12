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
 * it serves a RenamableIdentityFile instead, with the hook that keeps the new
 * name - in non-volatile memory, typically:
 *
 *   RenamableIdentityFile identity{[](void*, std::string_view name) noexcept {
 *       return settings.storeName(name) ? resource::Status::Ok
 *                                       : resource::Status::InternalError;
 *   }};
 *
 * The client writes the whole file in one WRITE (cursor 0, final, at most
 * writtenBytes). Only the name may change: a file whose id, model or firmware
 * differ, that is not an identity, or that comes in pieces is refused with
 * InvalidData and nothing changes; the hook's refusal is the WRITE's answer.
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
// - size(), read(): The Resource provider (read-only; RenamableIdentityFile adds write()).
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


// The identity a client may rename: IdentityFile with write(), and Stat then
// says writable. Only the name may change; the hook keeps it first.
// Public methods:
// - RenamableIdentityFile(): The hook that keeps a new name, and its context.
// - write(): The whole file in one piece; only the name may differ.
class RenamableIdentityFile : public IdentityFile {
	using Identity = ::telemetry::protocol::identity::Identity;

public:
	// Keeps a new name before it is served: Ok lets it in; any other status
	// refuses the rename and is the WRITE's answer. Called from write(), in
	// the context that serves the file; it must not call back into it.
	using RenameHook = Status (*)(void* context, std::string_view name) noexcept;

	constexpr explicit RenamableIdentityFile(RenameHook hook, void* context = nullptr) noexcept
	    : renameHook_(hook), renameContext_(context)
	{}

	[[nodiscard]] WriteResult write(Cursor cursor, Input input, bool final) noexcept
	{
		namespace identity = ::telemetry::protocol::identity;
		if (!renameHook_) {
			return {Status::NotWritable, cursor};
		}
		if (cursor != 0 || !final || input.size() > identity::writtenBytes || size() == 0) {
			return {Status::InvalidData, cursor};
		}
		Identity incoming;
		if (identity::parse(input, incoming) != identity::Problem::None) {
			return {Status::InvalidData, cursor};
		}
		if (incoming.id != id() || incoming.model != model() || incoming.firmware != firmware()) {
			return {Status::InvalidData, cursor};
		}
		// The texts view input, valid for this call: the hook keeps the name,
		// then rename() copies it.
		const auto verdict = renameHook_(renameContext_, incoming.name);
		if (verdict != Status::Ok) {
			return {verdict, cursor};
		}
		if (!rename(incoming.name)) {
			return {Status::InternalError, cursor};
		}
		const auto taken = static_cast<std::uint32_t>(input.size());
		return {Status::Ok, taken, taken, true};
	}

private:
	RenameHook renameHook_ = nullptr;
	void* renameContext_ = nullptr;
};

} // namespace resource::telemetry::v3
#endif
