/*
 * @file RuntimeSchema.hpp
 * @brief Borrowed, bounded structural descriptor indexes and prepared wire paths.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 */
#ifndef TELEMETRY_RUNTIME_RUNTIME_SCHEMA_HPP
#define TELEMETRY_RUNTIME_RUNTIME_SCHEMA_HPP
#pragma once

#include <telemetry/descriptor/Format.hpp>
#include <telemetry/type/WireTypes.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace telemetry::runtime {

using Input = std::span<const std::byte>;

enum class Status : std::uint8_t {
	Ok,
	InvalidDescriptor,
	Capacity,
	Overlap,
	Stale,
	TypeNotFound,
	MemberNotFound,
	IndexOutOfRange,
	InvalidPath,
	InvalidPayload,
	TypeMismatch,
	GenerationExhausted
};

enum class ParseError : std::uint8_t {
	None,
	Header,
	Limit,
	Fingerprint,
	Record,
	Reserved,
	Utf8,
	TypeReference,
	TypeShape,
	EnumOrder,
	DuplicateName,
	Catalog,
	Endpoint,
	Trailing
};

// Public methods: explicit success check.
template<class T>
struct Result {
	Status status = Status::Stale;
	T value{};

	explicit operator bool() const noexcept
	{
		return status == Status::Ok;
	}
};

struct ParseLimits {
	std::uint32_t descriptorBytes = Limits::maxDescriptorBytes;
	std::uint32_t types = Limits::maxTypeCount;
	std::uint32_t members = Limits::maxStructMembers;
	std::uint32_t arrayElements = Limits::maxArrayElements;
	std::uint32_t stringBytes = Limits::maxStringBytes;
	std::uint32_t enumEntries = Limits::maxEnumEntriesTotal;
	std::uint32_t catalogs = Limits::maxCatalogCountTotal;
	std::uint32_t endpoints = Limits::maxEndpointCountTotal;
	std::uint32_t wireBytes = Limits::maxValueWireBytes;
	std::uint32_t depth = Limits::maxTypeDepth;
	std::uint32_t expandedNodes = Limits::maxExpandedValueNodes;
};

struct Requirements {
	std::size_t types = 0, members = 0, enumEntries = 0, catalogs = 0, endpoints = 0;
};

struct RuntimeType {
	TypeId id = 0;
	TypeKind kind = TypeKind::Void;
	std::uint32_t wireBytes = 0;
	ScalarCode scalarCode = ScalarCode::Bool;
	TypeId relatedTypeId = 0;
	std::uint32_t elementCount = 0, firstMember = 0, memberCount = 0;
	std::uint32_t firstEnum = 0, enumCount = 0, depth = 0, expandedNodes = 0;
};

struct RuntimeMember {
	TypeId typeId = 0;
	std::uint32_t wireOffset = 0;
	std::string_view name{};
};

struct RuntimeEnumEntry {
	// Raw little-endian code bits, zero-extended to 64 bits, including signed codes.
	std::uint64_t codeBits = 0;
	std::string_view name{};
};

struct RuntimeCatalog {
	descriptor::Category category = descriptor::Category::Field;
	// firstEndpoint indexes schema.endpoints(category), not global storage.
	std::uint32_t group = 0, firstEndpoint = 0, endpointCount = 0;
	std::string_view name{};
};

struct RuntimeEndpoint {
	descriptor::Category category = descriptor::Category::Field;
	std::uint32_t id = 0;
	TypeId typeId = 0, responseTypeId = 0;
	std::uint8_t capabilities = 0;
	// Index into schema.catalogs(category), equal to the category-local group.
	std::uint32_t catalogIndex = 0;
	std::string_view name{};
};

class RuntimeSchema;
class DynamicValueView;
struct ParseResult;

namespace detail {
class SchemaParser;
}

// The workspace object must outlive every schema, handle and value view. Supplied
// arrays and descriptor bytes remain alive and immutable for their published
// generation. After invalidation, stale access needs only the workspace object;
// another parse still requires all supplied arrays to be alive and writable.
// Parsing/resetting is owner-thread only; reset or any parse attempt invalidates
// previous views.
// Public methods: construction, reset, generation.
class RuntimeSchemaWorkspace {
public:
	RuntimeSchemaWorkspace(std::span<RuntimeType> types, std::span<RuntimeMember> members,
	                       std::span<RuntimeEnumEntry> enums, std::span<RuntimeCatalog> catalogs,
	                       std::span<RuntimeEndpoint> endpoints) noexcept
	    : types_(types), members_(members), enums_(enums), catalogs_(catalogs),
	      endpoints_(endpoints)
	{}

	RuntimeSchemaWorkspace(const RuntimeSchemaWorkspace&) = delete;
	RuntimeSchemaWorkspace& operator=(const RuntimeSchemaWorkspace&) = delete;
	RuntimeSchemaWorkspace(RuntimeSchemaWorkspace&&) = delete;
	RuntimeSchemaWorkspace& operator=(RuntimeSchemaWorkspace&&) = delete;

	[[nodiscard]] Status reset() noexcept
	{
		published_ = false;
		if (generation_ == std::numeric_limits<std::uint64_t>::max())
			return Status::GenerationExhausted;
		++generation_;
		return Status::Ok;
	}

	[[nodiscard]] std::uint64_t generation() const noexcept
	{
		return generation_;
	}

private:
	std::span<RuntimeType> types_;
	std::span<RuntimeMember> members_;
	std::span<RuntimeEnumEntry> enums_;
	std::span<RuntimeCatalog> catalogs_;
	std::span<RuntimeEndpoint> endpoints_;
	std::uint64_t generation_ = 0;
	bool published_ = false;
	Input bytes_{};
	std::uint64_t fingerprint_ = 0;
	Requirements counts_{};
	std::array<std::uint32_t, 3> catFirst_{}, catCount_{}, epFirst_{}, epCount_{};
	friend class RuntimeSchema;
	friend class detail::SchemaParser;
};

// Public methods: default construction and immutable identity/type/offset getters.
class PreparedHandle {
public:
	PreparedHandle() noexcept = default;

	[[nodiscard]] const RuntimeSchemaWorkspace* owner() const noexcept
	{
		return owner_;
	}

	[[nodiscard]] std::uint64_t generation() const noexcept
	{
		return generation_;
	}

	[[nodiscard]] TypeId rootType() const noexcept
	{
		return rootType_;
	}

	[[nodiscard]] TypeId leafType() const noexcept
	{
		return leafType_;
	}

	[[nodiscard]] std::uint32_t wireOffset() const noexcept
	{
		return wireOffset_;
	}

private:
	const RuntimeSchemaWorkspace* owner_ = nullptr;
	std::uint64_t generation_ = 0;
	TypeId rootType_ = 0, leafType_ = 0;
	std::uint32_t wireOffset_ = 0;

	PreparedHandle(const RuntimeSchemaWorkspace* owner, std::uint64_t generation, TypeId root,
	               TypeId leaf, std::uint32_t offset) noexcept
	    : owner_(owner), generation_(generation), rootType_(root), leafType_(leaf),
	      wireOffset_(offset)
	{}
	friend class RuntimeSchema;
	friend class DynamicValueView;
};

// Public methods: member and index construct explicit path components.
struct PathStep {
	enum class Kind : std::uint8_t {
		Member,
		Index
	};
	Kind kind = Kind::Member;
	std::string_view name{};
	std::uint32_t element = 0;

	[[nodiscard]] static PathStep member(std::string_view name) noexcept
	{
		return {Kind::Member, name, 0};
	}

	[[nodiscard]] static PathStep index(std::uint32_t index) noexcept
	{
		return {Kind::Index, {}, index};
	}
};

// Views borrow a published workspace; no method dereferences an invalidated index.
// Public methods: parse, valid, owner, generation, descriptor, fingerprint, type,
// members, enumEntries, catalogs, endpoints, findEndpoint, findField, findCommand,
// findService, resolve, accepts, view.
class RuntimeSchema {
public:
	RuntimeSchema() noexcept = default;
	[[nodiscard]] static ParseResult parse(Input bytes, RuntimeSchemaWorkspace& workspace,
	                                       ParseLimits limits = {}) noexcept;

	[[nodiscard]] bool valid() const noexcept
	{
		return owner_ && owner_->published_ && owner_->generation_ == generation_;
	}

	[[nodiscard]] const RuntimeSchemaWorkspace* owner() const noexcept
	{
		return owner_;
	}

	[[nodiscard]] std::uint64_t generation() const noexcept
	{
		return generation_;
	}

	[[nodiscard]] Input descriptor() const noexcept
	{
		return valid() ? owner_->bytes_ : Input{};
	}

	[[nodiscard]] std::uint64_t fingerprint() const noexcept
	{
		return valid() ? owner_->fingerprint_ : 0;
	}

	[[nodiscard]] const RuntimeType* type(TypeId id) const noexcept
	{
		return valid() && id < owner_->counts_.types ? &owner_->types_[id] : nullptr;
	}

	[[nodiscard]] std::span<const RuntimeMember> members(TypeId id) const noexcept
	{
		const auto* t = type(id);
		return t && t->kind == TypeKind::Struct
		           ? owner_->members_.subspan(t->firstMember, t->memberCount)
		           : std::span<const RuntimeMember>{};
	}

	[[nodiscard]] std::span<const RuntimeEnumEntry> enumEntries(TypeId id) const noexcept
	{
		const auto* t = type(id);
		return t && t->kind == TypeKind::Enum ? owner_->enums_.subspan(t->firstEnum, t->enumCount)
		                                      : std::span<const RuntimeEnumEntry>{};
	}

	[[nodiscard]] std::span<const RuntimeCatalog>
	catalogs(descriptor::Category category) const noexcept
	{
		const auto c = static_cast<unsigned>(category);
		return valid() && c >= 1 && c <= 3
		           ? owner_->catalogs_.subspan(owner_->catFirst_[c - 1], owner_->catCount_[c - 1])
		           : std::span<const RuntimeCatalog>{};
	}

	[[nodiscard]] std::span<const RuntimeEndpoint>
	endpoints(descriptor::Category category) const noexcept
	{
		const auto c = static_cast<unsigned>(category);
		return valid() && c >= 1 && c <= 3
		           ? owner_->endpoints_.subspan(owner_->epFirst_[c - 1], owner_->epCount_[c - 1])
		           : std::span<const RuntimeEndpoint>{};
	}

	// Exact, case-sensitive names in one category. The result borrows the
	// published workspace, like endpoints(); do not retain it across reset/parse.
	[[nodiscard]] const RuntimeEndpoint* findEndpoint(descriptor::Category category,
	                                                  std::string_view group,
	                                                  std::string_view name) const noexcept
	{
		const auto items = endpoints(category);
		for (const auto& catalog : catalogs(category)) {
			if (catalog.name != group)
				continue;
			for (const auto& endpoint : items.subspan(catalog.firstEndpoint, catalog.endpointCount))
				if (endpoint.name == name)
					return &endpoint;
		}
		return nullptr;
	}

	// A path is the literal group name, '.', and endpoint name. Names may
	// themselves contain dots: more than one matching pair is ambiguous and
	// returns nullptr. The separate-name overload always addresses either pair.
	[[nodiscard]] const RuntimeEndpoint* findEndpoint(descriptor::Category category,
	                                                  std::string_view path) const noexcept
	{
		const auto items = endpoints(category);
		const RuntimeEndpoint* found = nullptr;
		for (const auto& catalog : catalogs(category)) {
			if (path.size() <= catalog.name.size() ||
			    path.substr(0, catalog.name.size()) != catalog.name ||
			    path[catalog.name.size()] != '.')
				continue;
			const auto name = path.substr(catalog.name.size() + 1);
			for (const auto& endpoint :
			     items.subspan(catalog.firstEndpoint, catalog.endpointCount)) {
				if (endpoint.name != name)
					continue;
				if (found)
					return nullptr;
				found = &endpoint;
			}
		}
		return found;
	}

	[[nodiscard]] const RuntimeEndpoint* findField(std::string_view group,
	                                               std::string_view name) const noexcept
	{
		return findEndpoint(descriptor::Category::Field, group, name);
	}

	[[nodiscard]] const RuntimeEndpoint* findField(std::string_view path) const noexcept
	{
		return findEndpoint(descriptor::Category::Field, path);
	}

	[[nodiscard]] const RuntimeEndpoint* findCommand(std::string_view group,
	                                                 std::string_view name) const noexcept
	{
		return findEndpoint(descriptor::Category::Command, group, name);
	}

	[[nodiscard]] const RuntimeEndpoint* findCommand(std::string_view path) const noexcept
	{
		return findEndpoint(descriptor::Category::Command, path);
	}

	[[nodiscard]] const RuntimeEndpoint* findService(std::string_view group,
	                                                 std::string_view name) const noexcept
	{
		return findEndpoint(descriptor::Category::Service, group, name);
	}

	[[nodiscard]] const RuntimeEndpoint* findService(std::string_view path) const noexcept
	{
		return findEndpoint(descriptor::Category::Service, path);
	}

	[[nodiscard]] Result<PreparedHandle> resolve(TypeId root,
	                                             std::span<const PathStep> path) const noexcept;
	[[nodiscard]] Result<PreparedHandle> resolve(TypeId root, std::string_view path) const noexcept;

	[[nodiscard]] bool accepts(const PreparedHandle& handle) const noexcept
	{
		if (!valid() || handle.owner_ != owner_ || handle.generation_ != generation_)
			return false;
		const auto* root = type(handle.rootType_);
		const auto* leaf = type(handle.leafType_);
		return root && leaf && handle.wireOffset_ <= root->wireBytes &&
		       leaf->wireBytes <= root->wireBytes - handle.wireOffset_;
	}

	[[nodiscard]] Result<DynamicValueView> view(TypeId root, Input bytes) const noexcept;

private:
	const RuntimeSchemaWorkspace* owner_ = nullptr;
	std::uint64_t generation_ = 0;
	Result<PreparedHandle> step(const PreparedHandle& handle, const PathStep& part) const noexcept;
	friend class detail::SchemaParser;
};

// Capacity reports necessary index lengths, not semantic acceptance. Only an
// Ok result publishes a schema. All metadata/string pointers borrow storage.
// Public methods: explicit success check.
struct ParseResult {
	Status status = Status::InvalidDescriptor;
	ParseError error = ParseError::None;
	Requirements requirements{};
	RuntimeSchema schema{};

	explicit operator bool() const noexcept
	{
		return status == Status::Ok;
	}
};

inline Result<PreparedHandle> RuntimeSchema::step(const PreparedHandle& h,
                                                  const PathStep& part) const noexcept
{
	if (!accepts(h))
		return {Status::Stale, {}};
	const auto* current = type(h.leafType_);
	if (part.kind == PathStep::Kind::Member) {
		if (current->kind != TypeKind::Struct)
			return {Status::TypeMismatch, {}};
		for (const auto& member : members(h.leafType_))
			if (member.name == part.name)
				return {Status::Ok,
				        {owner_, generation_, h.rootType_, member.typeId,
				         h.wireOffset_ + member.wireOffset}};
		return {Status::MemberNotFound, {}};
	}
	if (part.kind != PathStep::Kind::Index)
		return {Status::InvalidPath, {}};
	if (current->kind != TypeKind::Array)
		return {Status::TypeMismatch, {}};
	if (part.element >= current->elementCount)
		return {Status::IndexOutOfRange, {}};
	const auto offset = h.wireOffset_ + part.element * type(current->relatedTypeId)->wireBytes;
	return {Status::Ok, {owner_, generation_, h.rootType_, current->relatedTypeId, offset}};
}

inline Result<PreparedHandle> RuntimeSchema::resolve(TypeId root,
                                                     std::span<const PathStep> path) const noexcept
{
	if (!valid())
		return {Status::Stale, {}};
	if (!type(root))
		return {Status::TypeNotFound, {}};
	Result<PreparedHandle> result{Status::Ok, {owner_, generation_, root, root, 0}};
	for (const auto& part : path) {
		result = step(result.value, part);
		if (!result)
			return result;
	}
	return result;
}

inline Result<PreparedHandle> RuntimeSchema::resolve(TypeId root,
                                                     std::string_view path) const noexcept
{
	if (!valid())
		return {Status::Stale, {}};
	if (!type(root))
		return {Status::TypeNotFound, {}};
	Result<PreparedHandle> result{Status::Ok, {owner_, generation_, root, root, 0}};
	std::size_t pos = 0;
	while (pos < path.size()) {
		if (path[pos] == '[') {
			++pos;
			const auto begin = pos;
			std::uint32_t index = 0;
			while (pos < path.size() && path[pos] >= '0' && path[pos] <= '9') {
				const auto digit = static_cast<unsigned>(path[pos++] - '0');
				if (index > (std::numeric_limits<std::uint32_t>::max() - digit) / 10)
					return {Status::InvalidPath, {}};
				index = index * 10 + digit;
			}
			if (pos == begin || pos == path.size() || path[pos++] != ']')
				return {Status::InvalidPath, {}};
			result = step(result.value, PathStep::index(index));
		} else {
			const auto begin = pos;
			while (pos < path.size() && path[pos] != '.' && path[pos] != '[' && path[pos] != ']')
				++pos;
			if (begin == pos)
				return {Status::InvalidPath, {}};
			result = step(result.value, PathStep::member(path.substr(begin, pos - begin)));
		}
		if (!result)
			return result;
		if (pos < path.size() && path[pos] != '[') {
			if (path[pos++] != '.' || pos == path.size() || path[pos] == '[')
				return {Status::InvalidPath, {}};
		}
	}
	return result;
}

} // namespace telemetry::runtime

#include "detail/SchemaParser.hpp"
#endif // TELEMETRY_RUNTIME_RUNTIME_SCHEMA_HPP
