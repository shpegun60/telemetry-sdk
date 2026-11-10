/* Fixed-storage canonical request editing. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
#ifndef TELEMETRY_INTEGRATION_WORKBENCH_REQUEST_EDITOR_HPP
#define TELEMETRY_INTEGRATION_WORKBENCH_REQUEST_EDITOR_HPP
#pragma once
#include <integration/inspector/SnapshotStore.hpp>
#include <algorithm>

namespace telemetry::integration::workbench {
namespace editor_detail {
template<class T>
inline void zero(std::span<std::byte> bytes, std::size_t offset) noexcept
{
	const T value{};
	const auto bits = runtime::writer_detail::scalarBits(value);
	for (std::size_t i = 0; i < sizeof(T); ++i)
		bytes[offset + i] = std::byte((bits >> (8 * i)) & 0xffu);
}

inline runtime::Status scalar(ScalarCode code, std::uint32_t width, std::span<std::byte> bytes,
                              std::uint32_t offset, bool write) noexcept
{
#define TELEMETRY_INITIAL_SCALAR(CODE, TYPE)                                                       \
	case ScalarCode::CODE:                                                                         \
		if (width != sizeof(TYPE))                                                                 \
			return runtime::Status::InvalidPayload;                                                \
		if (write)                                                                                 \
			zero<TYPE>(bytes, offset);                                                             \
		return runtime::Status::Ok
	switch (code) {
		TELEMETRY_INITIAL_SCALAR(Bool, bool);
		TELEMETRY_INITIAL_SCALAR(U8, std::uint8_t);
		TELEMETRY_INITIAL_SCALAR(S8, std::int8_t);
		TELEMETRY_INITIAL_SCALAR(U16, std::uint16_t);
		TELEMETRY_INITIAL_SCALAR(S16, std::int16_t);
		TELEMETRY_INITIAL_SCALAR(U32, std::uint32_t);
		TELEMETRY_INITIAL_SCALAR(S32, std::int32_t);
		TELEMETRY_INITIAL_SCALAR(U64, std::uint64_t);
		TELEMETRY_INITIAL_SCALAR(S64, std::int64_t);
		TELEMETRY_INITIAL_SCALAR(F32, float);
		TELEMETRY_INITIAL_SCALAR(F64, double);
	}
#undef TELEMETRY_INITIAL_SCALAR
	return runtime::Status::TypeMismatch;
}

inline runtime::Status initializeNode(const runtime::RuntimeSchema& schema, TypeId id,
                                      std::span<std::byte> bytes, std::uint32_t offset,
                                      std::uint32_t depth, std::uint32_t& visited,
                                      bool write) noexcept
{
	if (depth > Limits::maxTypeDepth || visited >= Limits::maxExpandedValueNodes)
		return runtime::Status::InvalidPayload;
	++visited;
	const auto* type = schema.type(id);
	if (!type || !runtime::value_detail::range(offset, type->wireBytes, bytes.size()))
		return runtime::Status::InvalidPayload;
	if (type->kind == TypeKind::Scalar)
		return scalar(type->scalarCode, type->wireBytes, bytes, offset, write);
	if (type->kind == TypeKind::Enum) {
		const auto* underlying = schema.type(type->relatedTypeId);
		if (!underlying || underlying->kind != TypeKind::Scalar ||
		    underlying->scalarCode < ScalarCode::U8 || underlying->scalarCode > ScalarCode::S64 ||
		    underlying->wireBytes != type->wireBytes)
			return runtime::Status::InvalidPayload;
		// Open enums admit zero even when it has no name; this is not an
		// application/device default or a choice of the first named enumerator.
		return scalar(underlying->scalarCode, underlying->wireBytes, bytes, offset, write);
	}
	if (type->kind == TypeKind::Struct) {
		for (const auto& member : schema.members(id)) {
			const auto relative = std::uint64_t{offset} + member.wireOffset;
			if (relative > bytes.size())
				return runtime::Status::InvalidPayload;
			const auto status =
			    initializeNode(schema, member.typeId, bytes, static_cast<std::uint32_t>(relative),
			                   depth + 1, visited, write);
			if (status != runtime::Status::Ok)
				return status;
		}
		return runtime::Status::Ok;
	}
	if (type->kind == TypeKind::Array) {
		const auto* element = schema.type(type->relatedTypeId);
		if (!element)
			return runtime::Status::InvalidPayload;
		if (element->wireBytes == 0)
			return runtime::Status::Ok;
		for (std::uint32_t i = 0; i < type->elementCount; ++i) {
			const auto relative = std::uint64_t{offset} + std::uint64_t{i} * element->wireBytes;
			if (relative > bytes.size())
				return runtime::Status::InvalidPayload;
			const auto status =
			    initializeNode(schema, element->id, bytes, static_cast<std::uint32_t>(relative),
			                   depth + 1, visited, write);
			if (status != runtime::Status::Ok)
				return status;
		}
		return runtime::Status::Ok;
	}
	return runtime::Status::TypeMismatch;
}
} // namespace editor_detail

// A value editor owns neither buffers, client, endpoint nor queue. The caller
// serializes source selection, schema changes, setters and commit/rearm. A
// selected source and all supplied storage must outlive this nonmoving editor.
class RequestEditor {
public:
	RequestEditor(std::span<std::byte> draft, std::span<std::byte> request) noexcept
	    : draft_(draft), request_(request)
	{}

	RequestEditor(const RequestEditor&) = delete;
	RequestEditor& operator=(const RequestEditor&) = delete;
	RequestEditor(RequestEditor&&) = delete;
	RequestEditor& operator=(RequestEditor&&) = delete;

	// Two passes: check the complete bounded shape before the first byte store,
	// then explicitly encode exact scalar initial values and validate the result.
	// Void has no object; a zero-sized Struct is still a real canonical value.
	[[nodiscard]] static runtime::Status initialize(const runtime::RuntimeSchema& schema,
	                                                TypeId root,
	                                                std::span<std::byte> output) noexcept
	{
		if (!schema.valid())
			return runtime::Status::Stale;
		const auto* type = schema.type(root);
		if (!type)
			return runtime::Status::TypeNotFound;
		if (output.size() != type->wireBytes)
			return runtime::Status::Capacity;
		if (runtime::writer_detail::overlaps(output, runtime::writer_detail::objectBytes(schema)) ||
		    runtime::writer_detail::overlapsSchema(schema, output))
			return runtime::Status::Overlap;
		if (type->kind == TypeKind::Void)
			return root == 0 && output.empty() ? runtime::Status::Ok
			                                   : runtime::Status::TypeMismatch;
		std::uint32_t visited = 0;
		const auto checked =
		    editor_detail::initializeNode(schema, root, output, 0, 0, visited, false);
		if (checked != runtime::Status::Ok)
			return checked;
		visited = 0;
		const auto initialized =
		    editor_detail::initializeNode(schema, root, output, 0, 0, visited, true);
		if (initialized != runtime::Status::Ok)
			return initialized;
		return schema.view(root, output).status;
	}

	[[nodiscard]] runtime::Status begin(const inspector::SnapshotStore& source) noexcept
	{
		const auto info = source.snapshotInfo();
		if (!source.isCurrent() || !info.selected || (info.type != 0 && !info.ready))
			return runtime::Status::Stale;
		if (info.bytes > draft_.size() || info.bytes > request_.size())
			return runtime::Status::Capacity;
		const auto& schema = source.schema_;
		const auto overlapsControls = [&](runtime::Input bytes) noexcept {
			return runtime::writer_detail::overlaps(bytes,
			                                        runtime::writer_detail::objectBytes(*this)) ||
			       runtime::writer_detail::overlaps(bytes,
			                                        runtime::writer_detail::objectBytes(source)) ||
			       runtime::writer_detail::overlaps(bytes,
			                                        runtime::writer_detail::objectBytes(schema)) ||
			       runtime::writer_detail::overlapsSchema(schema, bytes);
		};
		if (runtime::writer_detail::overlaps(draft_, request_) || overlapsControls(draft_) ||
		    overlapsControls(request_))
			return runtime::Status::Overlap;
		runtime::DynamicValueWriter candidate;
		if (info.type != 0) {
			const auto original = source.bytes();
			if (!original)
				return runtime::Status::Stale;
			candidate =
			    runtime::DynamicValueWriter::begin(schema, info.type, original.value, draft_);
			if (!candidate)
				return candidate.status();
		}
		writer_ = std::move(candidate);
		source_ = &source;
		schema_ = schema;
		identity_ = info;
		return runtime::Status::Ok;
	}

	[[nodiscard]] bool current() const noexcept
	{
		if (!source_ || !schema_.valid() || !source_->isCurrent())
			return false;
		const auto info = source_->snapshotInfo();
		return info.selected && info.admission == identity_.admission &&
		       info.revision == identity_.revision && info.type == identity_.type &&
		       (info.type == 0 || (info.ready && info.bytes == identity_.bytes));
	}

	[[nodiscard]] bool dirty() const noexcept
	{
		if (!current() || identity_.type == 0)
			return false;
		const auto original = source_->bytes();
		return original &&
		       !std::equal(original.value.begin(), original.value.end(), draft_.begin());
	}

	[[nodiscard]] inspector::SnapshotInfo snapshotInfo() const noexcept
	{
		return current() ? source_->snapshotInfo() : inspector::SnapshotInfo{};
	}

	[[nodiscard]] inspector::NodeResult root() const noexcept
	{
		return current() ? source_->root()
		                 : inspector::NodeResult{inspector::ModelStatus::Stale, {}};
	}

	[[nodiscard]] auto info(const inspector::NodeHandle& node) const noexcept
	{
		return current()
		           ? source_->info(node)
		           : inspector::Result<inspector::NodeInfo>{inspector::ModelStatus::Stale, {}};
	}

	[[nodiscard]] auto child(const inspector::NodeHandle& node, std::uint32_t index) const noexcept
	{
		return current() ? source_->child(node, index)
		                 : inspector::NodeResult{inspector::ModelStatus::Stale, {}};
	}

	[[nodiscard]] auto childCount(const inspector::NodeHandle& node) const noexcept
	{
		return current() ? source_->childCount(node)
		                 : inspector::Result<std::uint32_t>{inspector::ModelStatus::Stale, 0};
	}

	[[nodiscard]] inspector::Result<inspector::ScalarValue>
	scalar(const inspector::NodeHandle& node) const noexcept
	{
		return current()
		           ? source_->scalar(node, runtime::Input{draft_}.first(identity_.bytes))
		           : inspector::Result<inspector::ScalarValue>{inspector::ModelStatus::Stale, {}};
	}

	template<class T>
	[[nodiscard]] runtime::Status setScalar(const inspector::NodeHandle& node,
	                                        const T& value) noexcept
	{
		static_assert(runtime::value_detail::scalar<T>, "Use the exact fixed-width scalar type");
		if (!current())
			return runtime::Status::Stale;
		const auto metadata = source_->info(node);
		const auto prepared = source_->prepareScalar(node);
		if (!metadata || !prepared)
			return !metadata && metadata.status == inspector::ModelStatus::Stale
			           ? runtime::Status::Stale
			           : runtime::Status::TypeMismatch;
		if (!schema_.accepts(prepared.value) || prepared.value.rootType() != identity_.type)
			return runtime::Status::Stale;
		if (metadata.value.code != runtime::value_detail::scalarCode<T>())
			return runtime::Status::TypeMismatch;
		return metadata.value.kind == TypeKind::Enum
		           ? writer_.setEnumUnderlying(prepared.value, value)
		           : writer_.set(prepared.value, value);
	}

	[[nodiscard]] runtime::Result<runtime::Input> commit() noexcept
	{
		if (!current())
			return {runtime::Status::Stale, {}};
		const auto bytes = std::span{request_}.first(identity_.bytes);
		if (identity_.type != 0) {
			const auto done = writer_.commit(bytes);
			if (!done)
				return {done.status, {}};
		}
		return {runtime::Status::Ok, bytes};
	}

	// Capture canonical staging without consuming the writer or changing dirty.
	// Both refusals and successful captures preserve the editor and source.
	[[nodiscard]] runtime::Result<std::uint32_t>
	copyDraft(std::span<std::byte> output) const noexcept
	{
		if (!current())
			return {runtime::Status::Stale, 0};
		if (identity_.type != 0 && writer_.status() != runtime::Status::Ok)
			return {writer_.status(), 0};
		if (output.size() < identity_.bytes)
			return {runtime::Status::Capacity, 0};
		const auto target = output.first(identity_.bytes);
		const auto original = source_->bytes();
		if (runtime::writer_detail::overlaps(target, draft_) ||
		    runtime::writer_detail::overlaps(target, request_) ||
		    runtime::writer_detail::overlaps(target, runtime::writer_detail::objectBytes(*this)) ||
		    runtime::writer_detail::overlaps(target,
		                                     runtime::writer_detail::objectBytes(*source_)) ||
		    runtime::writer_detail::overlaps(target,
		                                     runtime::writer_detail::objectBytes(schema_)) ||
		    runtime::writer_detail::overlapsSchema(schema_, target) ||
		    (original && runtime::writer_detail::overlaps(target, original.value)))
			return {runtime::Status::Overlap, 0};
		const runtime::Input bytes = runtime::Input{draft_}.first(identity_.bytes);
		if (identity_.type != 0) {
			const auto validated = schema_.view(identity_.type, bytes);
			if (!validated)
				return {validated.status, 0};
		}
		std::copy(bytes.begin(), bytes.end(), target.begin());
		return {runtime::Status::Ok, static_cast<std::uint32_t>(identity_.bytes)};
	}

	[[nodiscard]] runtime::Status rearm() noexcept
	{
		if (!current())
			return runtime::Status::Stale;
		if (identity_.type == 0)
			return runtime::Status::Ok;
		writer_ = runtime::DynamicValueWriter::begin(
		    schema_, identity_.type, runtime::Input{request_}.first(identity_.bytes), draft_);
		return writer_.status();
	}

	void clear() noexcept
	{
		writer_ = {};
		source_ = nullptr;
		schema_ = {};
		identity_ = {};
	}

private:
	std::span<std::byte> draft_, request_;
	const inspector::SnapshotStore* source_ = nullptr;
	runtime::RuntimeSchema schema_{};
	inspector::SnapshotInfo identity_{};
	runtime::DynamicValueWriter writer_;
};
} // namespace telemetry::integration::workbench
#endif
