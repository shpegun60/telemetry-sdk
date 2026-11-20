/*
 * @file SnapshotStore.hpp
 * @brief Guarded caller-owned runtime value storage shared by client wrappers.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_INTEGRATION_INSPECTOR_SNAPSHOT_STORE_HPP
#define TELEMETRY_INTEGRATION_INSPECTOR_SNAPSHOT_STORE_HPP
#pragma once

#include <telemetry/runtime/DynamicValueWriter.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

namespace telemetry::integration::workbench {
class RequestEditor;
template<std::size_t, std::size_t, std::size_t, std::size_t>
class WorkbenchModel;

namespace detail {
template<std::size_t, std::size_t, std::size_t, std::size_t, bool>
class WorkbenchState;
}
} // namespace telemetry::integration::workbench

namespace telemetry::integration::inspector {

template<std::size_t, std::size_t, std::size_t, std::size_t>
class InspectorModel;

namespace detail {
template<std::size_t, std::size_t, std::size_t, std::size_t, bool>
class InspectorState;
}

enum class ModelStatus : std::uint8_t {
	Ok,
	NoSelection,
	NoSnapshot,
	Stale,
	TypeMismatch,
	IndexOutOfRange,
	SnapshotTooLarge,
	RevisionExhausted,
	InvalidPayload,
	BufferTooSmall
};

template<class T>
struct Result {
	ModelStatus status = ModelStatus::Stale;
	T value{};

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return status == ModelStatus::Ok;
	}
};

// Handles borrow exactly one model lifetime and must not survive its destruction.
// Constructing another model at the same address does not extend that lifetime;
// these checks do not distinguish handles retained from a destroyed instance.
// Handles never contain pointers to rows or payload. A new accepted read,
// selection, admission or invalidation retires them within the live instance.
class NodeHandle {
public:
	NodeHandle() noexcept = default;

private:
	const void* owner_ = nullptr;
	std::uint64_t admission_ = 0, revision_ = 0;
	TypeId type_ = 0, parentType_ = 0;
	std::uint32_t offset_ = 0, index_ = 0;
	bool member_ = false, arrayElement_ = false;
	friend class SnapshotStore;
};

using NodeResult = Result<NodeHandle>;

struct NodeInfo {
	TypeKind kind = TypeKind::Void;
	TypeId type = 0;
	// code applies only to scalar/enum nodes; enums expose their underlying code.
	ScalarCode code = ScalarCode::Bool;
	std::uint32_t children = 0, index = 0;
	bool arrayElement = false;
	// Names, like the metadata enumeration spans, borrow the live schema generation.
	std::string_view name{};
	// The node type's v3.1 name (an Enum or Struct); empty when unnamed.
	std::string_view typeName{};
};

using ScalarData =
    std::variant<bool, std::uint8_t, std::int8_t, std::uint16_t, std::int16_t, std::uint32_t,
                 std::int32_t, std::uint64_t, std::int64_t, float, double>;

struct ScalarValue {
	ScalarCode code = ScalarCode::Bool;
	ScalarData value{};
	bool enumValue = false, enumKnown = false;
	// Numeric value is owned. Only this optional name borrows schema storage.
	std::string_view enumName{};
};

struct SnapshotInfo {
	std::uint32_t fieldId = 0, session = 0;
	TypeId type = 0;
	std::uint64_t admission = 0, revision = 0;
	std::size_t bytes = 0;
	bool selected = false, ready = false;
};

// A wrapper may bind its owner-only admission query here. It must outlive the
// store, must not recurse into it, and must not advance the client state machine.
struct SnapshotGuard {
	const void* context = nullptr;
	bool (*current)(const void*) noexcept = nullptr;
};

// Storage is exclusive, caller-owned, and remains disjoint from the store and
// live schema metadata. Admission verifies those ranges before any payload copy.
// Neither the guard nor this store owns a client, queue, endpoint or transport.
// Every access checks wrapper admission and schema generation before metadata.
// All calls, schema changes and editing leases are serialized by one owner.
class SnapshotStore {
public:
	explicit SnapshotStore(std::span<std::byte> storage, SnapshotGuard guard = {}) noexcept
	    : storage_(storage), guard_(guard)
	{}

	SnapshotStore(const SnapshotStore&) = delete;
	SnapshotStore& operator=(const SnapshotStore&) = delete;
	SnapshotStore(SnapshotStore&&) = delete;
	SnapshotStore& operator=(SnapshotStore&&) = delete;

	[[nodiscard]] bool canAdmit() const noexcept
	{
		return admission_ != std::numeric_limits<std::uint64_t>::max();
	}

	[[nodiscard]] ModelStatus admit(const runtime::RuntimeSchema& schema,
	                                std::uint32_t session) noexcept
	{
		if (!schema.valid() || session == 0)
			return ModelStatus::Stale;
		if (!canAdmit())
			return ModelStatus::RevisionExhausted;
		if (runtime::writer_detail::overlaps(storage_,
		                                     runtime::writer_detail::objectBytes(*this)) ||
		    runtime::writer_detail::overlaps(storage_,
		                                     runtime::writer_detail::objectBytes(schema)) ||
		    runtime::writer_detail::overlapsSchema(schema, storage_))
			return ModelStatus::InvalidPayload;
		schema_ = schema;
		session_ = session;
		++admission_;
		revision_ = 0;
		active_ = true;
		selected_ = ready_ = false;
		return ModelStatus::Ok;
	}

	[[nodiscard]] std::uint64_t admission() const noexcept
	{
		return admission_;
	}

	[[nodiscard]] std::uint64_t revision() const noexcept
	{
		return revision_;
	}

	[[nodiscard]] std::size_t capacity() const noexcept
	{
		return storage_.size();
	}

	[[nodiscard]] bool canAdvance() const noexcept
	{
		return revision_ != std::numeric_limits<std::uint64_t>::max();
	}

	[[nodiscard]] ModelStatus select(std::uint32_t fieldId, TypeId type) noexcept
	{
		return selectEndpoint(fieldId, type, descriptor::Category::Field);
	}

	[[nodiscard]] ModelStatus selectEndpoint(std::uint32_t fieldId, TypeId type,
	                                         descriptor::Category category) noexcept
	{
		if (!current())
			return ModelStatus::Stale;
		if (!schema_.type(type))
			return ModelStatus::TypeMismatch;
		if (!canAdvance())
			return ModelStatus::RevisionExhausted;
		retire();
		fieldId_ = fieldId;
		type_ = type;
		category_ = category;
		selected_ = true;
		return ModelStatus::Ok;
	}

	void retire() const noexcept
	{
		ready_ = false;
		if (canAdvance())
			++revision_;
	}

	void clearSelection() const noexcept
	{
		retire();
		selected_ = false;
	}

	void invalidate() const noexcept
	{
		active_ = false;
		clearSelection();
	}

	[[nodiscard]] bool isCurrent() const noexcept
	{
		return current();
	}

	// Capture admission/revision before submission and select only after it is
	// accepted. A superseded callback cannot publish into another selection.
	[[nodiscard]] ModelStatus publish(runtime::Input bytes, std::uint64_t admission,
	                                  std::uint64_t revision) noexcept
	{
		return publishBytes(bytes, admission, revision, false);
	}

	// For an owning wrapper's editor only. This view must not outlive retirement,
	// admission/schema invalidation, or this store. Inspector does not expose it.
	[[nodiscard]] Result<runtime::Input> bytes() const noexcept
	{
		if (!current())
			return {ModelStatus::Stale, {}};
		if (!ready_)
			return {ModelStatus::NoSnapshot, {}};
		return {ModelStatus::Ok, {storage_.data(), snapshotSize_}};
	}

	[[nodiscard]] SnapshotInfo snapshotInfo() const noexcept
	{
		if (!current())
			return {};
		return {selected_ ? fieldId_ : 0,
		        session_,
		        selected_ ? type_ : 0,
		        admission_,
		        revision_,
		        ready_ ? snapshotSize_ : 0,
		        selected_,
		        ready_};
	}

	[[nodiscard]] NodeResult root() const noexcept
	{
		if (!current())
			return {ModelStatus::Stale, {}};
		if (!ready_)
			return {ModelStatus::NoSnapshot, {}};
		return {ModelStatus::Ok, makeNode(type_, 0)};
	}

	[[nodiscard]] Result<NodeInfo> info(const NodeHandle& node) const noexcept
	{
		const auto checked = checkNode(node);
		if (!checked)
			return {checked.status, {}};
		const auto& type = *checked.value;
		NodeInfo result{type.kind,          type.id, type.scalarCode, children(type), node.index_,
		                node.arrayElement_, {},      type.name};
		if (type.kind == TypeKind::Enum) {
			const auto* underlying = schema_.type(type.relatedTypeId);
			if (!underlying)
				return {ModelStatus::InvalidPayload, {}};
			result.code = underlying->scalarCode;
		}
		if (node.member_) {
			const auto members = schema_.members(node.parentType_);
			if (node.index_ >= members.size())
				return {ModelStatus::InvalidPayload, {}};
			result.name = members[node.index_].name;
		} else if (!node.arrayElement_) {
			for (const auto& endpoint : schema_.endpoints(category_))
				if (endpoint.id == fieldId_) {
					result.name = endpoint.name;
					break;
				}
		}
		return {ModelStatus::Ok, result};
	}

	[[nodiscard]] Result<std::uint32_t> childCount(const NodeHandle& node) const noexcept
	{
		const auto checked = checkNode(node);
		return checked ? Result<std::uint32_t>{ModelStatus::Ok, children(*checked.value)}
		               : Result<std::uint32_t>{checked.status, 0};
	}

	[[nodiscard]] NodeResult child(const NodeHandle& parent, std::uint32_t index) const noexcept
	{
		const auto checked = checkNode(parent);
		if (!checked)
			return {checked.status, {}};
		const auto& type = *checked.value;
		if (type.kind != TypeKind::Struct && type.kind != TypeKind::Array)
			return {ModelStatus::TypeMismatch, {}};
		if (index >= children(type))
			return {ModelStatus::IndexOutOfRange, {}};
		TypeId childType = 0;
		std::uint64_t relative = 0;
		if (type.kind == TypeKind::Struct) {
			const auto& member = schema_.members(type.id)[index];
			childType = member.typeId;
			relative = member.wireOffset;
		} else {
			const auto* element = schema_.type(type.relatedTypeId);
			if (!element)
				return {ModelStatus::InvalidPayload, {}};
			childType = element->id;
			relative = std::uint64_t{index} * element->wireBytes;
		}
		const auto offset = std::uint64_t{parent.offset_} + relative;
		if (offset > snapshotSize_ || offset > std::numeric_limits<std::uint32_t>::max())
			return {ModelStatus::InvalidPayload, {}};
		auto result = makeNode(childType, static_cast<std::uint32_t>(offset));
		result.parentType_ = type.id;
		result.index_ = index;
		result.member_ = type.kind == TypeKind::Struct;
		result.arrayElement_ = type.kind == TypeKind::Array;
		return {ModelStatus::Ok, result};
	}

	[[nodiscard]] Result<ScalarValue> scalar(const NodeHandle& node) const noexcept
	{
		const auto checked = checkNode(node);
		if (!checked)
			return {checked.status, {}};
		return scalarBytes(node, *checked.value, {storage_.data(), snapshotSize_});
	}

	// Alternative bytes belong to an exclusive editing lease of this same root.
	// Check the node first; stale access must not touch the editing buffer either.
	[[nodiscard]] Result<ScalarValue> scalar(const NodeHandle& node,
	                                         runtime::Input bytes) const noexcept
	{
		const auto checked = checkNode(node);
		if (!checked)
			return {checked.status, {}};
		if (bytes.size() != snapshotSize_)
			return {ModelStatus::InvalidPayload, {}};
		return scalarBytes(node, *checked.value, bytes);
	}

	// Integer/enum decimal conversion never passes through floating point. Failure
	// leaves output unchanged. Output must not alias this store, its storage,
	// alternative value bytes or borrowed schema metadata.
	[[nodiscard]] Result<std::size_t> decimal(const NodeHandle& node,
	                                          std::span<char> output) const noexcept
	{
		return decimalValue(scalar(node), output);
	}

	[[nodiscard]] Result<std::size_t> decimal(const NodeHandle& node, std::span<char> output,
	                                          runtime::Input bytes) const noexcept
	{
		return decimalValue(scalar(node, bytes), output);
	}

	// Reconstruct only the path to a nonzero scalar, without scanning payload or
	// expanding arrays. Its root-relative offset uniquely selects each ancestor.
	[[nodiscard]] Result<runtime::PreparedHandle>
	prepareScalar(const NodeHandle& node) const noexcept
	{
		const auto checked = checkNode(node);
		if (!checked)
			return {checked.status, {}};
		if (checked.value->kind != TypeKind::Scalar && checked.value->kind != TypeKind::Enum)
			return {ModelStatus::TypeMismatch, {}};
		std::array<runtime::PathStep, Limits::maxTypeDepth> path{};
		std::size_t depth = 0;
		const auto* type = schema_.type(type_);
		std::uint32_t offset = 0;
		while (type && (type->id != node.type_ || offset != node.offset_)) {
			if (depth == path.size() || node.offset_ < offset)
				return {ModelStatus::InvalidPayload, {}};
			if (type->kind == TypeKind::Struct) {
				const runtime::RuntimeType* selected = nullptr;
				for (const auto& member : schema_.members(type->id)) {
					const auto* child = schema_.type(member.typeId);
					const auto begin = std::uint64_t{offset} + member.wireOffset;
					if (child && begin <= node.offset_ && node.offset_ - begin < child->wireBytes) {
						path[depth++] = runtime::PathStep::member(member.name);
						offset = static_cast<std::uint32_t>(begin);
						selected = child;
						break;
					}
				}
				type = selected;
			} else if (type->kind == TypeKind::Array) {
				const auto* element = schema_.type(type->relatedTypeId);
				if (!element || element->wireBytes == 0)
					return {ModelStatus::InvalidPayload, {}};
				const auto index = (node.offset_ - offset) / element->wireBytes;
				if (index >= type->elementCount)
					return {ModelStatus::InvalidPayload, {}};
				path[depth++] = runtime::PathStep::index(index);
				offset += index * element->wireBytes;
				type = element;
			} else {
				return {ModelStatus::InvalidPayload, {}};
			}
		}
		if (!type)
			return {ModelStatus::InvalidPayload, {}};
		const auto handle =
		    schema_.resolve(type_, std::span<const runtime::PathStep>{path}.first(depth));
		if (!handle || handle.value.leafType() != node.type_ ||
		    handle.value.wireOffset() != node.offset_)
			return {ModelStatus::InvalidPayload, {}};
		return {ModelStatus::Ok, handle.value};
	}

private:
	template<std::size_t, std::size_t, std::size_t, std::size_t>
	friend class InspectorModel;
	template<std::size_t, std::size_t, std::size_t, std::size_t, bool>
	friend class detail::InspectorState;
	template<std::size_t, std::size_t, std::size_t, std::size_t, bool>
	friend class ::telemetry::integration::workbench::detail::WorkbenchState;
	friend class ::telemetry::integration::workbench::RequestEditor;
	template<std::size_t, std::size_t, std::size_t, std::size_t>
	friend class ::telemetry::integration::workbench::WorkbenchModel;

	runtime::RuntimeSchema schema_{};
	std::span<std::byte> storage_;
	SnapshotGuard guard_{};
	std::uint32_t session_ = 0, fieldId_ = 0;
	TypeId type_ = 0;
	descriptor::Category category_ = descriptor::Category::Field;
	std::uint64_t admission_ = 0;
	mutable std::uint64_t revision_ = 0;
	std::size_t snapshotSize_ = 0;
	mutable bool active_ = false, selected_ = false, ready_ = false;

	// Inspector has already checked DynamicResult::hasValue(), so its callback
	// holds the same root/type/canonical proof. This private route avoids another
	// recursive validation of a potentially large DTO before the owning copy.
	[[nodiscard]] ModelStatus publishValidated(runtime::Input bytes, std::uint64_t admission,
	                                           std::uint64_t revision) noexcept
	{
		return publishBytes(bytes, admission, revision, true);
	}

	[[nodiscard]] ModelStatus publishBytes(runtime::Input bytes, std::uint64_t admission,
	                                       std::uint64_t revision, bool validated) noexcept
	{
		if (admission != admission_ || revision != revision_ || !current() || !selected_)
			return ModelStatus::Stale;
		const auto* type = schema_.type(type_);
		if (!type || bytes.size() != type->wireBytes || bytes.size() > storage_.size() ||
		    runtime::writer_detail::overlaps(storage_, runtime::writer_detail::objectBytes(*this)))
			return ModelStatus::InvalidPayload;
		if (!validated && !schema_.view(type_, bytes))
			return ModelStatus::InvalidPayload;
		if (!bytes.empty())
			std::memmove(storage_.data(), bytes.data(), bytes.size());
		snapshotSize_ = bytes.size();
		ready_ = true;
		return ModelStatus::Ok;
	}

	[[nodiscard]] bool current() const noexcept
	{
		if (!active_)
			return false;
		if (!schema_.valid() || (guard_.current && !guard_.current(guard_.context))) {
			invalidate();
			return false;
		}
		return true;
	}

	[[nodiscard]] NodeHandle makeNode(TypeId type, std::uint32_t offset) const noexcept
	{
		NodeHandle node;
		node.owner_ = this;
		node.admission_ = admission_;
		node.revision_ = revision_;
		node.type_ = type;
		node.offset_ = offset;
		return node;
	}

	[[nodiscard]] Result<const runtime::RuntimeType*>
	checkNode(const NodeHandle& node) const noexcept
	{
		// Identity/revision checks precede every schema row and payload access.
		if (node.owner_ != this || node.admission_ != admission_ || node.revision_ != revision_ ||
		    !current() || !ready_)
			return {ModelStatus::Stale, nullptr};
		const auto* type = schema_.type(node.type_);
		if (!type || node.offset_ > snapshotSize_ || type->wireBytes > snapshotSize_ - node.offset_)
			return {ModelStatus::InvalidPayload, nullptr};
		return {ModelStatus::Ok, type};
	}

	[[nodiscard]] static std::uint32_t children(const runtime::RuntimeType& type) noexcept
	{
		return type.kind == TypeKind::Struct  ? type.memberCount
		       : type.kind == TypeKind::Array ? type.elementCount
		                                      : 0;
	}

	template<class T>
	[[nodiscard]] static Result<ScalarValue> readScalar(const runtime::DynamicValueView& view,
	                                                    ScalarValue value) noexcept
	{
		const auto number = value.enumValue ? view.enumUnderlying<T>() : view.get<T>();
		if (!number)
			return {ModelStatus::InvalidPayload, {}};
		value.value = number.value;
		return {ModelStatus::Ok, value};
	}

	[[nodiscard]] Result<ScalarValue> scalarBytes(const NodeHandle& node,
	                                              const runtime::RuntimeType& type,
	                                              runtime::Input bytes) const noexcept
	{
		if (type.kind != TypeKind::Scalar && type.kind != TypeKind::Enum)
			return {ModelStatus::TypeMismatch, {}};
		const auto view = schema_.view(type.id, bytes.subspan(node.offset_, type.wireBytes));
		if (!view)
			return {ModelStatus::InvalidPayload, {}};
		ScalarValue result;
		result.enumValue = type.kind == TypeKind::Enum;
		result.code = type.scalarCode;
		if (result.enumValue) {
			const auto* underlying = schema_.type(type.relatedTypeId);
			const auto name = view.value.enumName();
			if (!underlying || !name)
				return {ModelStatus::InvalidPayload, {}};
			result.code = underlying->scalarCode;
			result.enumKnown = name.value.known;
			result.enumName = name.value.name;
		}
		switch (result.code) {
			case ScalarCode::Bool:
				return readScalar<bool>(view.value, result);
			case ScalarCode::U8:
				return readScalar<std::uint8_t>(view.value, result);
			case ScalarCode::S8:
				return readScalar<std::int8_t>(view.value, result);
			case ScalarCode::U16:
				return readScalar<std::uint16_t>(view.value, result);
			case ScalarCode::S16:
				return readScalar<std::int16_t>(view.value, result);
			case ScalarCode::U32:
				return readScalar<std::uint32_t>(view.value, result);
			case ScalarCode::S32:
				return readScalar<std::int32_t>(view.value, result);
			case ScalarCode::U64:
				return readScalar<std::uint64_t>(view.value, result);
			case ScalarCode::S64:
				return readScalar<std::int64_t>(view.value, result);
			case ScalarCode::F32:
				return readScalar<float>(view.value, result);
			case ScalarCode::F64:
				return readScalar<double>(view.value, result);
		}
		return {ModelStatus::TypeMismatch, {}};
	}

	[[nodiscard]] static Result<std::size_t> decimalValue(const Result<ScalarValue>& value,
	                                                      std::span<char> output) noexcept
	{
		if (!value)
			return {value.status, 0};
		std::array<char, 21> text{};
		std::size_t length = 0;
		const bool integral = std::visit(
		    [&](const auto& number) noexcept {
			    using T = std::remove_cvref_t<decltype(number)>;
			    if constexpr (std::is_integral_v<T>) {
				    const auto converted = [&]() noexcept {
					    if constexpr (std::is_same_v<T, bool>)
						    return std::to_chars(text.data(), text.data() + text.size(),
						                         number ? 1 : 0);
					    else
						    return std::to_chars(text.data(), text.data() + text.size(), number);
				    }();
				    length = static_cast<std::size_t>(converted.ptr - text.data());
				    return converted.ec == std::errc{};
			    } else
				    return false;
		    },
		    value.value.value);
		if (!integral)
			return {ModelStatus::TypeMismatch, 0};
		if (output.size() < length)
			return {ModelStatus::BufferTooSmall, 0};
		std::copy_n(text.data(), length, output.data());
		return {ModelStatus::Ok, length};
	}
};

} // namespace telemetry::integration::inspector
#endif // TELEMETRY_INTEGRATION_INSPECTOR_SNAPSHOT_STORE_HPP
