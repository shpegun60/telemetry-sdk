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
 * It owns its bytes (at most identity::writtenBytes), so the texts given to
 * set() and rename() need not outlive the call. No heap.
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
// - size(), read(): The Resource provider.
// - bytes(): The file as served.
//
// Call set() and rename() from the context that serves the file (or with it
// paused): a read that spans a change sees the new bytes from there on. The
// whole file fits one READ on any transport that carries writtenBytes.
class IdentityFile {
	using Identity = ::telemetry::protocol::identity::Identity;

public:
	constexpr IdentityFile() noexcept = default;

	constexpr bool set(std::string_view id, std::string_view name, std::string_view model = {},
	                   std::string_view firmware = {}) noexcept
	{
		std::array<std::byte, ::telemetry::protocol::identity::writtenBytes> next{};
		const auto written = ::telemetry::protocol::identity::write(Identity{id, name, model, firmware}, next);
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

	std::string_view id() const noexcept { return parsed().id; }
	std::string_view name() const noexcept { return parsed().name; }
	std::string_view model() const noexcept { return parsed().model; }
	std::string_view firmware() const noexcept { return parsed().firmware; }

	[[nodiscard]] constexpr FileSize size() const noexcept
	{
		return static_cast<FileSize>(size_);
	}

	[[nodiscard]] ReadResult read(Cursor cursor, Output output) const noexcept
	{
		if (cursor > size_) {
			return {Status::InvalidCursor, cursor};
		}
		if (cursor == size_) {
			return {Status::Ok, cursor, 0, true};
		}
		if (output.empty()) {
			return {Status::BufferTooSmall, cursor};
		}
		const auto remaining = size_ - static_cast<std::size_t>(cursor);
		const auto count = output.size() < remaining ? output.size() : remaining;
		std::memcpy(output.data(), bytes_.data() + cursor, count);
		const auto next = cursor + count;
		return {Status::Ok, next, static_cast<std::uint32_t>(count), next == size_};
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
};

} // namespace resource::telemetry::v3
#endif
