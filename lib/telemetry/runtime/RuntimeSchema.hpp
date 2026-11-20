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
	// Group-tree ceilings (v3.1). Hosts are lenient so that every device reads;
	// a producer's defaults are 8 levels, 64-byte names and 256-byte paths.
	// groupDepth counts levels (depths 0..groupDepth-1); nameBytes bounds one
	// group, endpoint or member name of a tree and one part of a type name;
	// pathBytes bounds group/.../endpoint and a whole type name.
	std::uint32_t groupDepth = 255;
	std::uint32_t nameBytes = Limits::maxStringBytes;
	std::uint32_t pathBytes = Limits::maxStringBytes;
};

// RuntimeCatalog::parent of a root group, and of every flat (v3.0) catalog.
inline constexpr std::uint32_t noParentGroup = 0xffffffffu;

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
	// v3.1 type name of an Enum or Struct, unique in the descriptor; empty when
	// unnamed. It borrows the descriptor bytes like every other name.
	std::string_view name{};
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
	// GroupTree (v3.1): the parent group's index (noParentGroup at a root) and
	// the depth. A tree publishes one row per group in each family, so every
	// family's catalogs() lists all G groups; a flat catalog has no parent.
	std::uint32_t parent = noParentGroup;
	std::uint32_t depth = 0;
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
	std::uint16_t minor_ = 0;
	bool groupTree_ = false;
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

// An endpoint found by its path, and the member/element path after it.
struct EndpointTarget {
	const RuntimeEndpoint* endpoint = nullptr;
	PreparedHandle handle{};
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
// Paths follow one grammar: group/.../endpoint, then .member and [i]. In a
// GroupTree descriptor names hold no / . [ ] or backslash and a path is unique;
// in a flat one a name may, and its path escapes them with a backslash.
// Public methods: parse, valid, owner, generation, descriptor, fingerprint,
// minorVersion, groupTree, groups, findGroup, groupPath, endpointPath, type,
// typeName, findType, members, enumEntries, catalogs, endpoints, findEndpoint,
// resolvePath, findField, findCommand, findService, resolve, accepts, view.
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

	// 0 for a descriptor without type names and attributes (v3.0), 1 otherwise.
	[[nodiscard]] std::uint16_t minorVersion() const noexcept
	{
		return valid() ? owner_->minor_ : 0;
	}

	// v3.1 GroupTree mode: one shared tree of groups replaces the family catalogs.
	[[nodiscard]] bool groupTree() const noexcept
	{
		return valid() && owner_->groupTree_;
	}

	// The tree's groups in pre-order (each family's catalogs() holds the same
	// G groups with that family's ranges); empty for a flat descriptor.
	[[nodiscard]] std::span<const RuntimeCatalog> groups() const noexcept
	{
		return groupTree() ? catalogs(descriptor::Category::Field)
		                   : std::span<const RuntimeCatalog>{};
	}

	// The group at group/.../group in a tree; nullptr if absent or flat.
	[[nodiscard]] const RuntimeCatalog* findGroup(std::string_view path) const noexcept
	{
		std::uint32_t parent = noParentGroup;
		const RuntimeCatalog* found = nullptr;
		std::size_t pos = 0;
		if (!groupTree() || path.empty())
			return nullptr;
		while (pos <= path.size()) {
			const auto segment = nextSegment(path, pos);
			if (segment.empty() || (pos < path.size() && path[pos] != '/'))
				return nullptr;
			found = childGroup(parent, segment);
			if (!found)
				return nullptr;
			parent = found->group;
			if (pos == path.size())
				return found;
			++pos;
		}
		return nullptr;
	}

	// Write the path of one tree group into out and return its length; when out
	// is too small nothing is written and the needed length is returned.
	[[nodiscard]] std::size_t groupPath(std::uint32_t group, std::span<char> out) const noexcept
	{
		const auto rows = groups();
		if (group >= rows.size())
			return 0;
		std::size_t size = 0;
		for (auto at = group; at != noParentGroup; at = rows[at].parent)
			size += rows[at].name.size() + (rows[at].parent == noParentGroup ? 0 : 1);
		if (size > out.size())
			return size;
		auto end = size;
		for (auto at = group; at != noParentGroup; at = rows[at].parent) {
			end -= rows[at].name.size();
			for (std::size_t i = 0; i < rows[at].name.size(); ++i)
				out[end + i] = rows[at].name[i];
			if (rows[at].parent != noParentGroup)
				out[--end] = '/';
		}
		return size;
	}

	// The path of one endpoint, group/.../endpoint in a tree and escaped
	// group/endpoint in a flat descriptor, written like groupPath().
	[[nodiscard]] std::size_t endpointPath(const RuntimeEndpoint& endpoint,
	                                       std::span<char> out) const noexcept
	{
		const auto rows = catalogs(endpoint.category);
		if (endpoint.catalogIndex >= rows.size())
			return 0;
		std::size_t size = 0;
		if (groupTree()) {
			size = groupPath(endpoint.catalogIndex, {}) + 1 + endpoint.name.size();
			if (size > out.size())
				return size;
			const auto prefix = groupPath(endpoint.catalogIndex, out);
			out[prefix] = '/';
			for (std::size_t i = 0; i < endpoint.name.size(); ++i)
				out[prefix + 1 + i] = endpoint.name[i];
			return size;
		}
		size = escapedSize(rows[endpoint.catalogIndex].name) + 1 + escapedSize(endpoint.name);
		if (size > out.size())
			return size;
		auto at = escape(rows[endpoint.catalogIndex].name, out, 0);
		out[at++] = '/';
		escape(endpoint.name, out, at);
		return size;
	}

	[[nodiscard]] const RuntimeType* type(TypeId id) const noexcept
	{
		return valid() && id < owner_->counts_.types ? &owner_->types_[id] : nullptr;
	}

	// The v3.1 name of an Enum or Struct; empty when unnamed or out of range.
	[[nodiscard]] std::string_view typeName(TypeId id) const noexcept
	{
		const auto* t = type(id);
		return t ? t->name : std::string_view{};
	}

	// Exact, case-sensitive type name. Names are unique in a descriptor, so the
	// name is a key; an empty name or an unnamed descriptor finds nothing.
	[[nodiscard]] const RuntimeType* findType(std::string_view name) const noexcept
	{
		if (!valid() || name.empty())
			return nullptr;
		for (const auto& item : owner_->types_.first(owner_->counts_.types))
			if (item.name == name)
				return &item;
		return nullptr;
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

	// The endpoint at group/.../endpoint in any family. In a tree the path is
	// unique. In a flat descriptor the group is one segment with / . [ ] and
	// backslash escaped by a backslash, and a path naming endpoints of two
	// families is ambiguous (nullptr). A member suffix is not an endpoint path:
	// resolvePath() takes one.
	[[nodiscard]] const RuntimeEndpoint* findEndpoint(std::string_view path) const noexcept
	{
		std::size_t end = 0;
		const auto* found = locate(path, end);
		return found && end == path.size() ? found : nullptr;
	}

	// group/.../endpoint followed by .member and [i] as needed, for example
	// adc/Config.Gain or adc/Status.raw[1]: the endpoint and a prepared handle
	// into its value type (a Field's value, a Command's or Service's request).
	[[nodiscard]] Result<EndpointTarget> resolvePath(std::string_view path) const noexcept
	{
		if (!valid())
			return {Status::Stale, {}};
		std::size_t end = 0;
		const auto* found = locate(path, end);
		if (!found)
			return {Status::InvalidPath, {}};
		auto rest = path.substr(end);
		if (!rest.empty() && rest.front() == '.')
			rest.remove_prefix(1);
		else if (!rest.empty() && rest.front() != '[')
			return {Status::InvalidPath, {}};
		const auto handle = resolve(found->typeId, rest);
		if (!handle)
			return {handle.status, {}};
		return {Status::Ok, {found, handle.value}};
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

	static bool pathCharacter(char c) noexcept
	{
		return c == '/' || c == '.' || c == '[' || c == ']' || c == '\\';
	}

	// One raw segment from pos up to an unescaped '/', '.' or '[', or the end;
	// pos stops on that character. Empty for a malformed segment.
	static std::string_view nextSegment(std::string_view path, std::size_t& pos) noexcept
	{
		const auto begin = pos;
		while (pos < path.size() && path[pos] != '/' && path[pos] != '.' && path[pos] != '[') {
			if (path[pos] == ']')
				return {};
			if (path[pos] == '\\' && ++pos == path.size())
				return {};
			++pos;
		}
		return path.substr(begin, pos - begin);
	}

	// A raw segment, escapes included, equals a stored name.
	static bool segmentEquals(std::string_view segment, std::string_view name) noexcept
	{
		std::size_t at = 0;
		for (std::size_t i = 0; i < segment.size(); ++i) {
			if (segment[i] == '\\')
				++i;
			if (at == name.size() || name[at++] != segment[i])
				return false;
		}
		return at == name.size();
	}

	static std::size_t escapedSize(std::string_view name) noexcept
	{
		std::size_t size = name.size();
		for (const char c : name)
			size += pathCharacter(c) ? 1 : 0;
		return size;
	}

	static std::size_t escape(std::string_view name, std::span<char> out, std::size_t at) noexcept
	{
		for (const char c : name) {
			if (pathCharacter(c))
				out[at++] = '\\';
			out[at++] = c;
		}
		return at;
	}

	const RuntimeCatalog* childGroup(std::uint32_t parent, std::string_view segment) const noexcept
	{
		for (const auto& group : groups())
			if (group.parent == parent && segmentEquals(segment, group.name))
				return &group;
		return nullptr;
	}

	// The endpoint named by the segments of path; end stops after its name.
	const RuntimeEndpoint* locate(std::string_view path, std::size_t& end) const noexcept
	{
		if (!valid())
			return nullptr;
		std::size_t pos = 0;
		std::uint32_t parent = noParentGroup;
		std::string_view group{}, leaf{};
		for (unsigned count = 0;; ++count) {
			const auto segment = nextSegment(path, pos);
			if (segment.empty())
				return nullptr;
			if (pos == path.size() || path[pos] != '/') {
				leaf = segment;
				if (count == 0)
					return nullptr;
				break;
			}
			++pos;
			if (groupTree()) {
				const auto* found = childGroup(parent, segment);
				if (!found)
					return nullptr;
				parent = found->group;
			} else if (count == 0) {
				group = segment;
			} else {
				return nullptr;
			}
		}
		end = pos;
		const RuntimeEndpoint* found = nullptr;
		for (unsigned c = 1; c <= 3; ++c) {
			const auto category = static_cast<descriptor::Category>(c);
			const auto items = endpoints(category);
			for (const auto& catalog : catalogs(category)) {
				if (groupTree() ? catalog.group != parent : !segmentEquals(group, catalog.name))
					continue;
				for (const auto& endpoint :
				     items.subspan(catalog.firstEndpoint, catalog.endpointCount)) {
					if (!segmentEquals(leaf, endpoint.name))
						continue;
					if (found)
						return nullptr;
					found = &endpoint;
				}
			}
		}
		return found;
	}
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
