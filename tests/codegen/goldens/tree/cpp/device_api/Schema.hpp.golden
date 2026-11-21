// Generated from a validated Telemetry Descriptor v3. Do not edit.
// Descriptor SHA-256: f8ebd8f588cc54580ba53d3e45c2d1cabe747d05d81d8733b2b8b3648c52092e
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace device_api {
namespace schema {
inline constexpr std::uint16_t major = 3, minor = 1;
inline constexpr std::uint64_t fingerprint = 0xe700bc41478f6af8ULL;
inline constexpr char descriptorSha256[] = "f8ebd8f588cc54580ba53d3e45c2d1cabe747d05d81d8733b2b8b3648c52092e";
inline constexpr std::array<std::byte, 64> descriptorHeader{std::byte{0x54}, std::byte{0x44}, std::byte{0x53}, std::byte{0x33}, std::byte{0x03}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x40}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x6a}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0xf8}, std::byte{0x6a}, std::byte{0x8f}, std::byte{0x47}, std::byte{0x41}, std::byte{0xbc}, std::byte{0x00}, std::byte{0xe7}, std::byte{0x13}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x05}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x40}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x12}, std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x9e}, std::byte{0x03}, std::byte{0x00}, std::byte{0x00}};
// Declared header identity only: no body hash check or peer authentication.
[[nodiscard]] constexpr bool matchesHeader(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() != descriptorHeader.size()) return false;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        if (bytes[i] != descriptorHeader[i]) return false;
    return true;
}
} // namespace schema
} // namespace device_api
