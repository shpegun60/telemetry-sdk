"""Optional C++ async binding policy over the existing standalone codec. MIT."""


def generate(schema):
    native = sorted(({endpoint.value_type for endpoint in schema.endpoints if endpoint.category == 1} |
                     {endpoint.response_type for endpoint in schema.endpoints if endpoint.category == 3}) - {0})
    sizes = ", ".join(["std::size_t{1}"] + [f"sizeof(detail::Type_{index})" for index in native])
    aligns = ", ".join(["std::size_t{1}"] + [f"alignof(detail::Type_{index})" for index in native])
    text = f'''/* Generated asynchronous bindings. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
#pragma once
#include "Codec.hpp"
#include <client/DeviceClient.hpp>
#include <algorithm>

namespace {schema.namespace}::client {{
struct CodecPolicy {{
    static constexpr std::uint64_t fingerprint = schema::fingerprint;
    template<class T> static constexpr std::size_t wireSize = codec::wireSize<T>;
    static constexpr std::size_t scratchBytes = std::max({{{sizes}}});
    static constexpr std::size_t scratchAlign = std::max({{{aligns}}});
    static std::span<const std::byte> descriptorHeader() noexcept {{ return schema::descriptorHeader; }}
    template<class T> static std::uint8_t encode(const T& value, std::span<std::byte> bytes) noexcept {{
        return static_cast<std::uint8_t>(codec::encode(value, bytes));
    }}
    template<class T> static std::uint8_t decode(std::span<const std::byte> bytes, T& value) noexcept {{
        return static_cast<std::uint8_t>(codec::decode(bytes, value));
    }}
}};
template<std::size_t N = 8, std::size_t PayloadBytes = 4096, std::size_t CallbackBytes = 64,
         class Lock = ::telemetry::client::NoLock>
using DeviceClient = ::telemetry::client::DeviceClient<CodecPolicy, N, PayloadBytes, CallbackBytes,
                                                     CodecPolicy::scratchBytes, CodecPolicy::scratchAlign, Lock>;
}} // namespace {schema.namespace}::client
'''
    if schema.minor >= 1:
        # v3.1: endpoint handles of a bound device, nested and by path.
        from .handles import cpp_handles
        text += cpp_handles(schema)
    return {f"cpp/{schema.namespace.replace('::', '/')}/ClientBindings.hpp": text}
