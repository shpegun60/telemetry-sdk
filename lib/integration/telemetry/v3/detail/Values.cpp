/*
 * @file Values.cpp
 * @brief Bounded values emission; no hash or prefix getter replay during READ.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Resume directly at a validated token boundary and sample only tokens that
 * fit in the caller output. A completed prefix may be returned before a
 * later provider error; earlier getters are never replayed for that retry.
 */

#include <integration/telemetry/v3/detail/Values.hpp>
#include <integration/telemetry/v3/ValuesFormat.hpp>
#include <telemetry/descriptor/Format.hpp>
#include <cstring>

namespace resource::telemetry::v3::detail {
// A values file carries the version of the descriptor it is read with.
static_assert(valuesMajor == ::telemetry::descriptor::binaryMajor &&
              valuesMinor == ::telemetry::descriptor::binaryMinor);

namespace {

template<class U>
void littleEndian(std::byte* out, U value) noexcept
{
	for (std::size_t i = 0; i < sizeof(U); ++i) {
		out[i] = static_cast<std::byte>(value >> (8 * i));
	}
}

} // namespace

ReadResult readValues(const ValuesView& file, const ValueToken* tokens, Cursor cursor,
                      Output output, CurrentValuesAbiTag) noexcept
{
	using ::telemetry::DispatchStatus;
	if (file.totalBytes < valuesHeaderBytes) {
		return {Status::InvalidData, cursor};
	}
	if (cursor > file.totalBytes) {
		return {Status::InvalidCursor, cursor};
	}
	if (cursor == file.totalBytes) {
		return {Status::Ok, cursor, 0, true};
	}

	std::uint32_t position = 0;
	if (cursor >= valuesHeaderBytes) {
		// Exact token starts only. The EOF sentinel was handled above.
		std::uint32_t end = file.count;
		while (position < end) {
			const auto middle = position + (end - position) / 2;
			if (tokens[middle].offset < cursor) {
				position = middle + 1;
			} else {
				end = middle;
			}
		}
		if (position == file.count || tokens[position].offset != cursor) {
			return {Status::InvalidCursor, cursor};
		}
	}
	if (output.empty()) {
		return {Status::BufferTooSmall, cursor};
	}

	const auto available = file.totalBytes - static_cast<std::uint32_t>(cursor);
	if (output.size() > available) {
		output = output.first(available);
	}
	// Protect the whole output before writing even a header prefix. A file
	// composed entirely of local-storage fields never accesses Workspace.
	if (file.maxScratch != 0 && ::telemetry::buffersOverlap(output, file.workspace->storage())) {
		return {Status::InvalidData, cursor};
	}

	std::uint32_t used = 0;
	if (cursor < valuesHeaderBytes) {
		std::byte header[valuesHeaderBytes];
		std::memcpy(header, valuesMagic.data(), valuesMagic.size());
		littleEndian(header + 4, valuesMajor);
		littleEndian(header + 6, valuesMinor);
		littleEndian(header + 8, file.count);
		littleEndian(header + 12, file.totalBytes);
		littleEndian(header + 16, file.fingerprint);
		const auto remaining = valuesHeaderBytes - static_cast<std::uint32_t>(cursor);
		used = static_cast<std::uint32_t>(output.size() < remaining ? output.size() : remaining);
		std::memcpy(output.data(), header + cursor, used);
	}

	while (position < file.count && cursor + used >= valuesHeaderBytes) {
		const auto tokenBytes = tokens[position + 1].offset - tokens[position].offset;
		if (tokenBytes > output.size() - used) {
			break; // No getter or reservation yet.
		}
		const auto* field = file.fields.find(tokens[position].id);
		auto* token = output.data() + used;
		// Length and overlap have already been checked here. The existing thunk
		// checks available scratch before resolving a slot or invoking a getter.
		const auto result = field->read(field->readContext, token + 1, *file.workspace);
		if (result.dispatch == DispatchStatus::Unavailable) {
			std::memset(token + 1, 0, tokenBytes - 1);
			token[0] = static_cast<std::byte>(ValueStatus::Unavailable);
		} else if (result.dispatch == DispatchStatus::Ok && result.written == tokenBytes - 1) {
			token[0] = static_cast<std::byte>(ValueStatus::Ok);
		} else {
			// Insufficient scratch is a provider configuration error, not an
			// absent value. Commit an already completed prefix first; retry at
			// this token reports the error without replaying earlier getters.
			if (used != 0) {
				break;
			}
			return {Status::InternalError, cursor};
		}
		used += tokenBytes;
		++position;
	}

	if (used == 0) {
		return {Status::BufferTooSmall, cursor};
	}
	return {Status::Ok, cursor + used, used, cursor + used == file.totalBytes};
}

} // namespace resource::telemetry::v3::detail
