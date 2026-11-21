// Generated from a validated Telemetry Descriptor v3. Do not edit.
// Descriptor SHA-256: f244f765841ad74cce0b79fc2f06ca20fc605bf47a0e1f8b942e24e97a3d53e6
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace device_api {
namespace schema {
inline constexpr std::uint16_t major = 3, minor = 1;
inline constexpr std::uint64_t fingerprint = 0xf652e8451aecab94ULL;
inline constexpr char descriptorSha256[] = "f244f765841ad74cce0b79fc2f06ca20fc605bf47a0e1f8b942e24e97a3d53e6";
inline constexpr std::array<std::byte, 64> descriptorHeader{std::byte{0x54}, std::byte{0x44}, std::byte{0x53}, std::byte{0x33}, std::byte{0x03}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x40}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x03}, std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0x94}, std::byte{0xab}, std::byte{0xec}, std::byte{0x1a}, std::byte{0x45}, std::byte{0xe8}, std::byte{0x52}, std::byte{0xf6}, std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x08}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x40}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x5c}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0xbf}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00}};
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
