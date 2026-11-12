/*
 * @file ValuesFile.hpp
 * @brief Fixed-size live values resource with whole-value snapshot tokens.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Publish current Fields using the identity of their validated descriptor.
 * The header can be split, but each value token is emitted whole; endpoint
 * owners and the shared Workspace remain application-owned and serialized.
 */
#ifndef RESOURCE_TELEMETRY_V3_VALUES_FILE_HPP
#define RESOURCE_TELEMETRY_V3_VALUES_FILE_HPP
#pragma once

#include <telemetry/descriptor/Descriptor.hpp>
#include <integration/telemetry/v3/ValuesFormat.hpp>
#include "detail/Values.hpp"
#include <array>
#include <concepts>
#include <cstdlib>

namespace resource::telemetry::v3 {
namespace ts = ::telemetry;

namespace detail {
[[noreturn]] inline void valuesSizeExceeded() noexcept
{
	std::abort();
}
} // namespace detail

template<class Fields>
// Public methods:
// - ValuesFile(): Index declared Fields.
// - size(): Report values extent.
// - fingerprint(): Read descriptor identity.
// - fieldCount(): Count value tokens.
// - maxTokenSize(): Report token capacity.
// - requiredWorkspace(): Report read scratch.
// - read(): Emit whole tokens.
class ValuesFile {
	static constexpr std::size_t count = Fields::RootTypes::size;
	static_assert(count <= UINT32_MAX, "Values field count exceeds u32");

public:
	// The descriptor supplies both identity and fields. Its cached information
	// is copied, so only the underlying tables/names and Workspace are borrowed.
	// Deduction of W rejects temporaries and conversion proxies, even when the
	// ValuesFile class type is supplied explicitly.
	template<class Commands, class Services, class Profile, class W>
	    requires(std::same_as<W, ts::Workspace&>)
	constexpr ValuesFile(
	    const ::telemetry::descriptor::Descriptor<Fields, Commands, Services, Profile>& descriptor,
	    W&& workspace) noexcept
	    : view_{descriptor.fieldIndex(),           &workspace, descriptor.fingerprint(),
	            static_cast<std::uint32_t>(count), 0,          0}
	{
		if (!descriptor.valid()) {
			return;
		}
		std::uint64_t next = valuesHeaderBytes;
		std::size_t position = 0;
		for (std::uint32_t group = 0; group < view_.fields.count(); ++group) {
			const auto& catalog = view_.fields.catalogs()[group];
			for (std::uint32_t entry = 0; entry < catalog.count; ++entry) {
				const auto& field = catalog.entries[entry];
				tokens_[position++] = {static_cast<std::uint32_t>(next), (group << 16) | entry};
				const auto token = std::uint64_t{1} + field.wireBytes;
				next += token;
				if (next > UINT32_MAX) {
					if (std::is_constant_evaluated()) {
						detail::valuesSizeExceeded();
					}
					view_.fingerprint = 0;
					return;
				}
				if (token > maxToken_) {
					maxToken_ = static_cast<std::uint32_t>(token);
				}
				if (field.readScratchBytes > view_.maxScratch) {
					view_.maxScratch = field.readScratchBytes;
				}
			}
		}
		tokens_[count] = {static_cast<std::uint32_t>(next), 0};
		view_.totalBytes = static_cast<FileSize>(next);
	}

	[[nodiscard]] constexpr FileSize size() const noexcept
	{
		return view_.totalBytes;
	}

	[[nodiscard]] constexpr std::uint64_t fingerprint() const noexcept
	{
		return view_.fingerprint;
	}

	[[nodiscard]] constexpr std::uint32_t fieldCount() const noexcept
	{
		return static_cast<std::uint32_t>(count);
	}

	// Check these capacities at integration/startup. Large tokens are not split;
	// READ returns BufferTooSmall at that token if a transport cannot carry it.
	[[nodiscard]] constexpr std::uint32_t maxTokenSize() const noexcept
	{
		return maxToken_;
	}

	[[nodiscard]] constexpr std::uint32_t requiredWorkspace() const noexcept
	{
		return view_.maxScratch;
	}

	// A fitting token samples one Field once. Different tokens may observe
	// different instants; a coherent application snapshot needs external policy.
	// Keep the whole output disjoint from live owner objects and shared scratch.
	[[nodiscard]] ReadResult read(Cursor cursor, Output output) const noexcept
	{
		return detail::readValues(view_, tokens_.data(), cursor, output,
		                          detail::CurrentValuesAbiTag{});
	}

private:
	// Persist the common metadata instead of rebuilding a view on the stack
	// for every READ. The index pointer is supplied separately so copies stay
	// valid without self-referential pointers or custom relocation machinery.
	detail::ValuesView view_;
	std::array<detail::ValueToken, count + 1> tokens_{};
	std::uint32_t maxToken_ = 0;
};

template<class F, class C, class S, class P, class W>
ValuesFile(const ::telemetry::descriptor::Descriptor<F, C, S, P>&, W&&) -> ValuesFile<F>;

} // namespace resource::telemetry::v3
#endif
