/*
 * @file IdentityWire.hpp
 * @brief The bytes of /telemetry/identity.bin: which unit a device is and what its user calls it.
 * @author Ruslan Kovtun (shpegun60). SPDX-License-Identifier: MIT
 *
 * A device serves one small read-only file:
 * - id: unique to the unit and never changing - the MCU's unique ID, or a
 *   serial number assigned in production. Letters, digits, '.', '_' and '-'
 *   only, beginning with a letter or digit, so a client can put it in a
 *   folder name as it is.
 * - name: what the device's user calls it; may be empty, may change.
 * - model and firmware: optional, for people reading it.
 *
 * A client reads it once after connecting - in every mode, a files-only
 * device included, since it needs no descriptor - and keeps the device's
 * files under "<name> [<id>]". The descriptor cannot carry this: it is the
 * same for every unit of one firmware.
 *
 * The encoder (write) and the reader (parse) are both here, so a device and
 * every client agree on the bytes. No heap, no Qt. The device's provider is
 * resource::telemetry::v3::IdentityFile.
 *
 * Bytes, little-endian, as the other /telemetry/ files:
 *
 *   0   4 bytes  ASCII "TID1"
 *   4   u16      version = 1
 *   6   u16      flags = 0
 *   8   u16      totalBytes (the whole file)
 *   10  u16      textCount: texts that follow, at least 4
 *   12  texts, each u16 byteLength then that many UTF-8 bytes, no NUL:
 *       id, name, model, firmware, then any a later version adds
 *
 * A reader takes the first four texts and skips the rest. Each of the four
 * is at most maxText bytes; a file is at most maxBytes.
 */
#ifndef TELEMETRY_PROTOCOL_IDENTITY_WIRE_HPP
#define TELEMETRY_PROTOCOL_IDENTITY_WIRE_HPP
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace telemetry::protocol::identity {
inline constexpr std::string_view path = "/telemetry/identity.bin";
inline constexpr std::array<std::byte, 4> magic{std::byte{'T'}, std::byte{'I'}, std::byte{'D'},
                                                std::byte{'1'}};
inline constexpr std::uint16_t version = 1;
inline constexpr std::size_t headerBytes = 12;
inline constexpr std::size_t texts = 4;
// One text's limit, in UTF-8 bytes.
inline constexpr std::size_t maxText = 64;
// What write() produces at most, and what a reader accepts at most.
inline constexpr std::size_t writtenBytes = headerBytes + texts * (2 + maxText);
inline constexpr std::size_t maxBytes = 512;

// Why bytes are not an identity.
enum class Problem : std::uint8_t {
	None,
	Empty,     // no bytes at all
	Truncated, // shorter than its header or than a text says
	Magic,     // not "TID1"
	Version,   // a version this reader does not know
	Flags,     // flags this reader does not know
	Size,      // totalBytes is not the size, or above maxBytes
	Texts,     // fewer than four texts
	Id,        // empty, too long, or a character outside [A-Za-z0-9._-]
	Text       // name, model or firmware too long, not UTF-8, or with a control character
};

// Views into the parsed bytes: valid while they are.
struct Identity {
	std::string_view id, name, model, firmware;
};

// 1..maxText of [A-Za-z0-9._-], beginning with a letter or a digit (so never
// "." or ".." as a folder name).
constexpr bool validId(std::string_view id) noexcept
{
	if (id.empty() || id.size() > maxText) {
		return false;
	}
	const auto alnum = [](char c) {
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
	};
	if (!alnum(id.front())) {
		return false;
	}
	for (const char c : id) {
		if (!alnum(c) && c != '.' && c != '_' && c != '-') {
			return false;
		}
	}
	return true;
}

// At most maxText bytes of well-formed UTF-8 without control characters.
constexpr bool validText(std::string_view text) noexcept
{
	if (text.size() > maxText) {
		return false;
	}
	std::size_t i = 0;
	while (i < text.size()) {
		const auto lead = static_cast<unsigned char>(text[i]);
		if (lead < 0x80) {
			if (lead < 0x20 || lead == 0x7f) {
				return false;
			}
			++i;
			continue;
		}
		std::size_t length = 0;
		std::uint32_t point = 0;
		if (lead >= 0xc2 && lead <= 0xdf) {
			length = 2;
			point = lead & 0x1fu;
		} else if (lead >= 0xe0 && lead <= 0xef) {
			length = 3;
			point = lead & 0x0fu;
		} else if (lead >= 0xf0 && lead <= 0xf4) {
			length = 4;
			point = lead & 0x07u;
		} else {
			return false;
		}
		if (i + length > text.size()) {
			return false;
		}
		for (std::size_t k = 1; k < length; ++k) {
			const auto next = static_cast<unsigned char>(text[i + k]);
			if ((next & 0xc0u) != 0x80u) {
				return false;
			}
			point = (point << 6) | (next & 0x3fu);
		}
		// Overlong forms, surrogates, beyond U+10FFFF, and C1 controls.
		if ((length == 3 && point < 0x800) || (length == 4 && point < 0x10000) ||
		    (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff ||
		    (point >= 0x80 && point < 0xa0)) {
			return false;
		}
		i += length;
	}
	return true;
}

// Encodes an identity into out; returns the bytes written, or 0 when a text
// is invalid or out is too small (writtenBytes always suffices).
constexpr std::size_t write(const Identity& identity, std::span<std::byte> out) noexcept
{
	if (!validId(identity.id) || !validText(identity.name) || !validText(identity.model) ||
	    !validText(identity.firmware)) {
		return 0;
	}
	const std::array<std::string_view, texts> values{identity.id, identity.name, identity.model,
	                                                 identity.firmware};
	std::size_t total = headerBytes;
	for (const auto& value : values) {
		total += 2 + value.size();
	}
	if (out.size() < total) {
		return 0;
	}
	const auto put16 = [&](std::size_t at, std::size_t value) {
		out[at] = static_cast<std::byte>(value & 0xffu);
		out[at + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
	};
	for (std::size_t i = 0; i < magic.size(); ++i) {
		out[i] = magic[i];
	}
	put16(4, version);
	put16(6, 0);
	put16(8, total);
	put16(10, texts);
	std::size_t at = headerBytes;
	for (const auto& value : values) {
		put16(at, value.size());
		at += 2;
		for (const char c : value) {
			out[at++] = static_cast<std::byte>(c);
		}
	}
	return total;
}

// Reads an identity file; out views into bytes. Problem::None when it is one.
inline Problem parse(std::span<const std::byte> bytes, Identity& out) noexcept
{
	out = {};
	const auto u16 = [&](std::size_t at) {
		return static_cast<std::size_t>(static_cast<unsigned>(bytes[at]) |
		                                (static_cast<unsigned>(bytes[at + 1]) << 8));
	};
	if (bytes.empty()) {
		return Problem::Empty;
	}
	if (bytes.size() < headerBytes) {
		return Problem::Truncated;
	}
	for (std::size_t i = 0; i < magic.size(); ++i) {
		if (bytes[i] != magic[i]) {
			return Problem::Magic;
		}
	}
	if (u16(4) != version) {
		return Problem::Version;
	}
	if (u16(6) != 0) {
		return Problem::Flags;
	}
	if (u16(8) != bytes.size() || bytes.size() > maxBytes) {
		return Problem::Size;
	}
	const auto count = u16(10);
	if (count < texts) {
		return Problem::Texts;
	}
	std::array<std::string_view, texts> found{};
	std::size_t at = headerBytes;
	for (std::size_t i = 0; i < count; ++i) {
		if (at + 2 > bytes.size()) {
			return Problem::Truncated;
		}
		const auto length = u16(at);
		at += 2;
		if (at + length > bytes.size()) {
			return Problem::Truncated;
		}
		if (i < texts) {
			found[i] = {reinterpret_cast<const char*>(bytes.data() + at), length};
		}
		at += length;
	}
	if (!validId(found[0])) {
		return Problem::Id;
	}
	for (std::size_t i = 1; i < texts; ++i) {
		if (!validText(found[i])) {
			return Problem::Text;
		}
	}
	out = {found[0], found[1], found[2], found[3]};
	return Problem::None;
}

// The problem in plain English words, for a client's message.
constexpr std::string_view describe(Problem problem) noexcept
{
	switch (problem) {
		case Problem::None:
			return "an identity";
		case Problem::Empty:
			return "the file is empty";
		case Problem::Truncated:
			return "the file ends early";
		case Problem::Magic:
			return "not an identity file";
		case Problem::Version:
			return "an identity version this program does not read";
		case Problem::Flags:
			return "identity flags this program does not read";
		case Problem::Size:
			return "its size does not match its header";
		case Problem::Texts:
			return "fewer texts than an identity has";
		case Problem::Id:
			return "its id is empty or has characters other than letters, digits, '.', '_' and '-'";
		case Problem::Text:
			return "its name, model or firmware is too long or not readable text";
	}
	return "unknown";
}

// The usual id of an MCU: its unique ID bytes as uppercase hex, first byte
// first. Writes 2 * raw.size() characters into out and returns them; empty
// when out is too small or the result would exceed maxText.
constexpr std::string_view hexId(std::span<const std::byte> raw, std::span<char> out) noexcept
{
	constexpr char digits[] = "0123456789ABCDEF";
	const auto length = raw.size() * 2;
	if (length == 0 || length > out.size() || length > maxText) {
		return {};
	}
	for (std::size_t i = 0; i < raw.size(); ++i) {
		const auto byte = static_cast<unsigned>(raw[i]);
		out[2 * i] = digits[byte >> 4];
		out[2 * i + 1] = digits[byte & 0x0fu];
	}
	return {out.data(), length};
}
} // namespace telemetry::protocol::identity
#endif
