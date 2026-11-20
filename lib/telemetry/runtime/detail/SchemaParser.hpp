/*
 * @file SchemaParser.hpp
 * @brief Allocation-free validation and publication of descriptor v3 indexes.
 * @author Ruslan Kovtun (shpegun60), codexAi
 * SPDX-License-Identifier: MIT
 *
 * Reads v3.0 and v3.1. A v3.1 type name is validated, checked for uniqueness
 * and published in RuntimeType::name; an attribute area is validated and
 * skipped, so nothing is stored for it and the model is that of the same
 * descriptor without attributes.
 */
#ifndef TELEMETRY_RUNTIME_DETAIL_SCHEMA_PARSER_HPP
#define TELEMETRY_RUNTIME_DETAIL_SCHEMA_PARSER_HPP
#pragma once

#include "../RuntimeSchema.hpp"

#include <functional>
#include <utility>

namespace telemetry::runtime::detail {

// Public methods: construction, good, remaining, attributes, number, zeros, text,
// record, done.
class SchemaReader {
public:
	SchemaReader(Input bytes, std::size_t begin, std::size_t end, ParseError& error,
	             const ParseLimits& limits, std::uint16_t minor = 0) noexcept
	    : bytes_(bytes), pos_(begin), end_(end), error_(error), limits_(limits), minor_(minor)
	{}

	// v3.1: this record's header flags an attribute area after its payload.
	bool attributes() const noexcept
	{
		return attributes_;
	}

	bool good() const noexcept
	{
		return error_ == ParseError::None;
	}

	std::size_t remaining() const noexcept
	{
		return end_ - pos_;
	}

	std::uint64_t number(unsigned width) noexcept
	{
		if (!take(width))
			return 0;
		std::uint64_t value = 0;
		for (unsigned i = 0; i < width; ++i)
			value |= std::uint64_t{std::to_integer<std::uint8_t>(bytes_[pos_ - width + i])}
			         << (8 * i);
		return value;
	}

	void zeros(unsigned width) noexcept
	{
		if (number(width) != 0)
			fail(ParseError::Reserved);
	}

	std::string_view text() noexcept
	{
		const auto size = number(4);
		if (!good())
			return {};
		if (size == 0 || size > limits_.stringBytes) {
			fail(ParseError::Limit);
			return {};
		}
		if (!take(static_cast<std::size_t>(size)))
			return {};
		const auto begin = pos_ - size;
		for (std::size_t i = begin; i < pos_;) {
			const auto lead = std::to_integer<unsigned>(bytes_[i++]);
			if (lead >= 1 && lead <= 0x7f)
				continue;
			unsigned continuation = 0, low = 0x80, high = 0xbf;
			if (lead >= 0xc2 && lead <= 0xdf)
				continuation = 1;
			else if (lead >= 0xe0 && lead <= 0xef) {
				continuation = 2;
				if (lead == 0xe0)
					low = 0xa0;
				if (lead == 0xed)
					high = 0x9f;
			} else if (lead >= 0xf0 && lead <= 0xf4) {
				continuation = 3;
				if (lead == 0xf0)
					low = 0x90;
				if (lead == 0xf4)
					high = 0x8f;
			} else {
				fail(ParseError::Utf8);
				return {};
			}
			if (continuation > pos_ - i) {
				fail(ParseError::Utf8);
				return {};
			}
			const auto second = std::to_integer<unsigned>(bytes_[i++]);
			if (second < low || second > high) {
				fail(ParseError::Utf8);
				return {};
			}
			for (unsigned j = 1; j < continuation; ++j) {
				const auto ch = std::to_integer<unsigned>(bytes_[i++]);
				if (ch < 0x80 || ch > 0xbf) {
					fail(ParseError::Utf8);
					return {};
				}
			}
		}
		return {reinterpret_cast<const char*>(bytes_.data() + begin),
		        static_cast<std::size_t>(size)};
	}

	SchemaReader record(unsigned kind) noexcept
	{
		if (number(1) != kind || number(1) != descriptor::recordVersion)
			fail(ParseError::Record);
		// v3.0 reserves the record flags; v3.1 defines bit 0 only.
		const auto flags = number(2);
		constexpr auto area = static_cast<unsigned>(descriptor::RecordFlag::Attributes);
		if (flags != 0 && (minor_ < descriptor::typeNamesMinor || flags != area))
			fail(ParseError::Reserved);
		const auto size = number(4);
		if (!good() || size > remaining()) {
			fail(ParseError::Record);
			return {bytes_, pos_, pos_, error_, limits_, minor_};
		}
		const auto begin = pos_;
		pos_ += static_cast<std::size_t>(size);
		SchemaReader result{bytes_, begin, pos_, error_, limits_, minor_};
		result.attributes_ = flags == area;
		return result;
	}

	// Without an attribute area the payload ends exactly here. With one, it
	// runs to the record end as {u16 tag, u16 target, u32 length, value}
	// sorted by (target, tag); members > 0 only for a Struct Type record,
	// whose target k names member k-1. Advisory and application tags are
	// validated and skipped; reserved and (all, in v3.1) critical tags refuse.
	void done(std::uint32_t members = 0) noexcept
	{
		if (!attributes_) {
			if (pos_ != end_)
				fail(ParseError::Trailing);
			return;
		}
		if (good() && pos_ == end_)
			fail(ParseError::Record);
		std::uint32_t previous = 0;
		for (bool first = true; good() && pos_ < end_; first = false) {
			if (remaining() < descriptor::attributeHeaderBytes) {
				fail(ParseError::Record);
				return;
			}
			const auto tag = static_cast<std::uint16_t>(number(2));
			const auto target = static_cast<std::uint32_t>(number(2));
			const auto length = number(4);
			if (length > remaining() || target > members) {
				fail(ParseError::Record);
				return;
			}
			pos_ += static_cast<std::size_t>(length);
			const auto key = target << 16 | tag;
			if (!first && key <= previous) {
				fail(ParseError::Record);
				return;
			}
			previous = key;
			const auto category = descriptor::attributeTagClass(tag);
			if (category == descriptor::AttributeTagClass::Reserved)
				fail(ParseError::Reserved);
			else if (category == descriptor::AttributeTagClass::Critical)
				fail(ParseError::Record);
		}
	}

private:
	Input bytes_;
	std::size_t pos_, end_;
	ParseError& error_;
	const ParseLimits& limits_;
	std::uint16_t minor_;
	bool attributes_ = false;

	void fail(ParseError error) noexcept
	{
		if (good())
			error_ = error;
	}

	bool take(std::size_t count) noexcept
	{
		if (!good())
			return false;
		if (count > remaining()) {
			fail(ParseError::Record);
			return false;
		}
		pos_ += count;
		return true;
	}
};

// Public methods: parse validates privately, then publishes one generation.
class SchemaParser {
public:
	static ParseResult parse(Input bytes, RuntimeSchemaWorkspace& workspace,
	                         const ParseLimits& limits) noexcept
	{
		// Determine aliases before reset: even rejected input may refer to the
		// workspace object itself. No descriptor bytes are read after that reset.
		const bool overlap = aliases(bytes, workspace);
		const auto reset = workspace.reset();
		if (reset != Status::Ok)
			return {reset, ParseError::None, {}, {}};
		if (overlap)
			return {Status::Overlap, ParseError::None, {}, {}};
		SchemaParser parser(bytes, workspace, limits);
		return parser.run();
	}

private:
	Input bytes_;
	RuntimeSchemaWorkspace& ws_;
	const ParseLimits& limits_;
	ParseError error_ = ParseError::None;
	Requirements required_{};
	std::array<std::uint32_t, 3> cats_{}, eps_{};
	std::uint32_t catStart_ = 0, epStart_ = 0;
	std::uint64_t fingerprint_ = 0;
	std::uint16_t minor_ = 0;
	// Minor 1 must carry at least one type name or attribute area.
	bool extended_ = false;

	SchemaParser(Input bytes, RuntimeSchemaWorkspace& workspace, const ParseLimits& limits) noexcept
	    : bytes_(bytes), ws_(workspace), limits_(limits)
	{}

	bool require(bool condition, ParseError error) noexcept
	{
		if (!condition && error_ == ParseError::None)
			error_ = error;
		return error_ == ParseError::None;
	}

	bool good() const noexcept
	{
		return error_ == ParseError::None;
	}

	ParseResult refused(Status status = Status::InvalidDescriptor) const noexcept
	{
		return {status, error_, required_, {}};
	}

	static bool intersects(Input a, Input b) noexcept
	{
		if (a.empty() || b.empty())
			return false;
		const std::less<const void*> less{};
		return less(a.data(), b.data() + b.size()) && less(b.data(), a.data() + a.size());
	}

	static bool aliases(Input bytes, RuntimeSchemaWorkspace& ws) noexcept
	{
		const std::array<Input, 6> storage{
		    std::as_bytes(ws.types_),     std::as_bytes(ws.members_),
		    std::as_bytes(ws.enums_),     std::as_bytes(ws.catalogs_),
		    std::as_bytes(ws.endpoints_), std::as_bytes(std::span{&ws, 1})};
		for (std::size_t i = 0; i < storage.size(); ++i) {
			if (intersects(bytes, storage[i]))
				return true;
			for (std::size_t j = 0; j < i; ++j)
				if (intersects(storage[i], storage[j]))
					return true;
		}
		return false;
	}

	bool limitsValid() noexcept
	{
		return require(limits_.descriptorBytes <= Limits::maxDescriptorBytes &&
		                   limits_.types <= Limits::maxTypeCount &&
		                   limits_.members <= Limits::maxStructMembers &&
		                   limits_.arrayElements <= Limits::maxArrayElements &&
		                   limits_.stringBytes <= Limits::maxStringBytes &&
		                   limits_.enumEntries <= Limits::maxEnumEntriesTotal &&
		                   limits_.catalogs <= Limits::maxCatalogCountTotal &&
		                   limits_.endpoints <= Limits::maxEndpointCountTotal &&
		                   limits_.wireBytes <= Limits::maxValueWireBytes &&
		                   limits_.depth <= Limits::maxTypeDepth &&
		                   limits_.expandedNodes <= Limits::maxExpandedValueNodes,
		               ParseError::Limit);
	}

	bool header() noexcept
	{
		if (!limitsValid() ||
		    !require(bytes_.size() >= 64 && bytes_.size() <= limits_.descriptorBytes,
		             ParseError::Limit))
			return false;
		SchemaReader row(bytes_, 0, 64, error_, limits_);
		require(row.number(4) == 0x33534454, ParseError::Header);
		require(row.number(2) == descriptor::binaryMajor, ParseError::Header);
		minor_ = static_cast<std::uint16_t>(row.number(2));
		require((minor_ == descriptor::binaryMinor || minor_ == descriptor::typeNamesMinor) &&
		            row.number(2) == 64 && row.number(2) == 0,
		        ParseError::Header);
		require(row.number(4) == bytes_.size(), ParseError::Header);
		fingerprint_ = row.number(8);
		required_.types = static_cast<std::size_t>(row.number(4));
		std::uint64_t catalogTotal = 0, endpointTotal = 0;
		for (unsigned c = 0; c < 3; ++c) {
			cats_[c] = static_cast<std::uint32_t>(row.number(4));
			eps_[c] = static_cast<std::uint32_t>(row.number(4));
			catalogTotal += cats_[c];
			endpointTotal += eps_[c];
			require(cats_[c] <= 65536, ParseError::Catalog);
		}
		if (!require(catalogTotal <= limits_.catalogs && endpointTotal <= limits_.endpoints,
		             ParseError::Limit))
			return false;
		required_.catalogs = static_cast<std::size_t>(catalogTotal);
		required_.endpoints = static_cast<std::size_t>(endpointTotal);
		const auto typeStart = row.number(4);
		catStart_ = static_cast<std::uint32_t>(row.number(4));
		epStart_ = static_cast<std::uint32_t>(row.number(4));
		require(required_.types >= 12 && required_.types <= limits_.types &&
		            required_.catalogs <= limits_.catalogs &&
		            required_.endpoints <= limits_.endpoints,
		        ParseError::Limit);
		if (!require(typeStart == 64 && catStart_ >= 64 && epStart_ >= catStart_ &&
		                 epStart_ <= bytes_.size(),
		             ParseError::Header))
			return false;
		require(required_.types * 20 <= catStart_ - 64 &&
		            required_.catalogs * 29 <= epStart_ - catStart_ &&
		            required_.endpoints * 21 <= bytes_.size() - epStart_,
		        ParseError::Header);
		if (!good())
			return false;
		auto hash = descriptor::fingerprintBasis;
		for (std::size_t i = 0; i < bytes_.size(); ++i) {
			hash ^= i >= 16 && i < 24 ? 0 : std::to_integer<std::uint8_t>(bytes_[i]);
			hash *= descriptor::fingerprintPrime;
		}
		return require(hash == fingerprint_, ParseError::Fingerprint);
	}

	bool typePrefix(SchemaReader& row, std::uint32_t index, RuntimeType& item) noexcept
	{
		item = {};
		item.id = index;
		require(row.number(4) == index, ParseError::TypeShape);
		item.kind = static_cast<TypeKind>(row.number(1));
		// v3.1 typeFlags: bit 0 Named, only under minor 1; bits 1..7 reserved.
		const auto flags = row.number(1);
		constexpr auto named = static_cast<unsigned>(descriptor::TypeFlag::Named);
		require(flags == 0 || (minor_ == descriptor::typeNamesMinor && flags == named),
		        ParseError::Reserved);
		row.zeros(2);
		item.wireBytes = static_cast<std::uint32_t>(row.number(4));
		require(item.wireBytes <= limits_.wireBytes, ParseError::Limit);
		const bool compound = item.kind == TypeKind::Enum || item.kind == TypeKind::Struct ||
		                      item.kind == TypeKind::Array;
		require((index == 0 && item.kind == TypeKind::Void) ||
		            (index >= 1 && index <= 11 && item.kind == TypeKind::Scalar) ||
		            (index >= 12 && compound),
		        ParseError::TypeShape);
		// Builtin records keep fixed bytes: no attribute area on Void/Scalar.
		require(!row.attributes() || compound, ParseError::TypeShape);
		if (flags == named && good()) {
			require(item.kind == TypeKind::Enum || item.kind == TypeKind::Struct,
			        ParseError::TypeShape);
			item.name = row.text();
		}
		extended_ = extended_ || row.attributes() || !item.name.empty();
		return good();
	}

	bool countStorage() noexcept
	{
		SchemaReader reader(bytes_, 64, catStart_, error_, limits_, minor_);
		for (std::uint32_t index = 0; index < required_.types && good(); ++index) {
			auto row = reader.record(1);
			RuntimeType item;
			if (!typePrefix(row, index, item))
				break;
			if (item.kind == TypeKind::Enum) {
				row.number(4);
				const auto count = row.number(4);
				require(count <= limits_.enumEntries - required_.enumEntries &&
				            count * 6 <= row.remaining(),
				        ParseError::Limit);
				if (good())
					required_.enumEntries += static_cast<std::size_t>(count);
			} else if (item.kind == TypeKind::Struct) {
				const auto count = row.number(4);
				require(count <= limits_.members && count * 9 <= row.remaining(),
				        ParseError::Limit);
				if (good())
					required_.members += static_cast<std::size_t>(count);
			}
		}
		reader.done();
		return good();
	}

	const RuntimeType* reference(std::uint64_t id, std::uint32_t current) noexcept
	{
		if (!require(id > 0 && id < current, ParseError::TypeReference))
			return nullptr;
		return &ws_.types_[static_cast<std::size_t>(id)];
	}

	bool types() noexcept
	{
		constexpr unsigned widths[]{0, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8};
		SchemaReader reader(bytes_, 64, catStart_, error_, limits_, minor_);
		std::uint32_t memberPos = 0, enumPos = 0;
		for (std::uint32_t index = 0; index < required_.types && good(); ++index) {
			auto row = reader.record(1);
			auto& item = ws_.types_[index];
			if (!typePrefix(row, index, item))
				break;
			std::uint64_t expected = 0, nodes = 0, depth = 0;
			if (item.kind == TypeKind::Scalar) {
				const auto code = row.number(1);
				row.zeros(3);
				if (!require(code == index, ParseError::TypeShape))
					break;
				item.scalarCode = static_cast<ScalarCode>(code);
				expected = widths[code];
				nodes = 1;
			} else if (item.kind == TypeKind::Enum) {
				item.relatedTypeId = static_cast<TypeId>(row.number(4));
				const auto* underlying = reference(item.relatedTypeId, index);
				if (!underlying)
					break;
				const auto code = static_cast<unsigned>(underlying->scalarCode);
				if (!require(underlying->kind == TypeKind::Scalar && code >= 2 && code <= 9,
				             ParseError::TypeShape))
					break;
				const auto count = row.number(4);
				expected = underlying->wireBytes;
				nodes = 1;
				if (!require(count <= required_.enumEntries - enumPos &&
				                 count * (expected + 5) <= row.remaining(),
				             ParseError::Limit))
					break;
				item.firstEnum = enumPos;
				item.enumCount = static_cast<std::uint32_t>(count);
				const auto flip = (code == 3 || code == 5 || code == 7 || code == 9)
				                      ? std::uint64_t{1} << (expected * 8 - 1)
				                      : 0;
				std::uint64_t previous = 0;
				for (std::uint32_t i = 0; i < count && good(); ++i) {
					const auto bits = row.number(static_cast<unsigned>(expected));
					const auto ordered = bits ^ flip;
					require(i == 0 || previous < ordered, ParseError::EnumOrder);
					previous = ordered;
					const auto name = row.text();
					if (good())
						ws_.enums_[enumPos++] = {bits, name};
				}
			} else if (item.kind == TypeKind::Struct) {
				const auto count = row.number(4);
				if (!require(count <= limits_.members && count <= required_.members - memberPos &&
				                 count * 9 <= row.remaining(),
				             ParseError::Limit))
					break;
				item.firstMember = memberPos;
				item.memberCount = static_cast<std::uint32_t>(count);
				depth = nodes = 1;
				for (std::uint32_t i = 0; i < count && good(); ++i) {
					const auto childId = row.number(4);
					const auto* child = reference(childId, index);
					if (!child)
						break;
					const auto name = row.text();
					for (std::uint32_t j = item.firstMember; j < memberPos && good(); ++j)
						require(ws_.members_[j].name != name, ParseError::DuplicateName);
					if (!require(expected <= limits_.wireBytes, ParseError::Limit))
						break;
					if (good())
						ws_.members_[memberPos++] = {static_cast<TypeId>(childId),
						                             static_cast<std::uint32_t>(expected), name};
					expected += child->wireBytes;
					nodes += child->expandedNodes;
					if (depth <= child->depth)
						depth = std::uint64_t{child->depth} + 1;
				}
			} else if (item.kind == TypeKind::Array) {
				item.relatedTypeId = static_cast<TypeId>(row.number(4));
				const auto* child = reference(item.relatedTypeId, index);
				if (!child)
					break;
				item.elementCount = static_cast<std::uint32_t>(row.number(4));
				require(item.elementCount <= limits_.arrayElements, ParseError::Limit);
				expected = std::uint64_t{item.elementCount} * child->wireBytes;
				nodes = 1 + std::uint64_t{item.elementCount} * child->expandedNodes;
				depth = std::uint64_t{child->depth} + 1;
			}
			require(expected == item.wireBytes, ParseError::TypeShape);
			require(depth <= limits_.depth && nodes <= limits_.expandedNodes, ParseError::Limit);
			item.depth = static_cast<std::uint32_t>(depth);
			item.expandedNodes = static_cast<std::uint32_t>(nodes);
			// Attribute targets k >= 1 name struct member k-1.
			row.done(item.kind == TypeKind::Struct ? item.memberCount : 0);
		}
		reader.done();
		return good();
	}

	// In-place heapsort gives bounded O(n log n) duplicate checks without a
	// secondary caller workspace. Restore canonical positional order afterward.
	template<class T, class Less>
	static void sort(std::span<T> rows, Less less) noexcept
	{
		const auto sift = [&](std::size_t root, std::size_t count) {
			while (root < count / 2) {
				auto child = root * 2 + 1;
				if (child + 1 < count && less(rows[child], rows[child + 1]))
					++child;
				if (!less(rows[root], rows[child]))
					return;
				std::swap(rows[root], rows[child]);
				root = child;
			}
		};
		for (auto i = rows.size() / 2; i > 0; --i)
			sift(i - 1, rows.size());
		for (auto n = rows.size(); n > 1; --n) {
			std::swap(rows[0], rows[n - 1]);
			sift(0, n - 1);
		}
	}

	template<class T, class Less>
	bool uniqueNames(std::span<T> rows, Less canonical) noexcept
	{
		sort(rows, [](const auto& a, const auto& b) {
			return a.name < b.name;
		});
		for (std::size_t i = 1; i < rows.size(); ++i)
			require(rows[i - 1].name != rows[i].name, ParseError::DuplicateName);
		sort(rows, canonical);
		return good();
	}

	// Type names are unique among type names (byte comparison). Sorting the
	// published rows by name, then by TypeId, needs no secondary storage; one
	// comparator type keeps a single sort instantiation in a client image.
	bool typeNames() noexcept
	{
		struct Order {
			bool byName;

			bool operator()(const RuntimeType& a, const RuntimeType& b) const noexcept
			{
				return byName ? a.name < b.name : a.id < b.id;
			}
		};

		const auto rows = ws_.types_.first(required_.types);
		sort(rows, Order{true});
		for (std::size_t i = 1; i < rows.size(); ++i)
			require(rows[i].name.empty() || rows[i - 1].name != rows[i].name,
			        ParseError::DuplicateName);
		sort(rows, Order{false});
		return good();
	}

	bool catalogs() noexcept
	{
		SchemaReader reader(bytes_, catStart_, epStart_, error_, limits_, minor_);
		std::uint32_t pos = 0, endpointBase = 0;
		for (unsigned c = 0; c < 3 && good(); ++c) {
			ws_.catFirst_[c] = pos;
			ws_.catCount_[c] = cats_[c];
			ws_.epFirst_[c] = endpointBase;
			ws_.epCount_[c] = eps_[c];
			endpointBase += eps_[c];
			std::uint32_t first = 0;
			for (std::uint32_t group = 0; group < cats_[c] && good(); ++group) {
				auto row = reader.record(2);
				require(row.number(1) == c + 1, ParseError::Catalog);
				row.zeros(3);
				require(row.number(4) == group && row.number(4) == first, ParseError::Catalog);
				const auto count = row.number(4);
				if (!require(count <= 65536 && count <= eps_[c] - first, ParseError::Catalog))
					break;
				const auto name = row.text();
				row.done();
				if (good())
					ws_.catalogs_[pos++] = {static_cast<descriptor::Category>(c + 1), group, first,
					                        static_cast<std::uint32_t>(count), name};
				first += static_cast<std::uint32_t>(count);
			}
			require(first == eps_[c], ParseError::Catalog);
			if (good())
				uniqueNames(ws_.catalogs_.subspan(ws_.catFirst_[c], cats_[c]),
				            [](const auto& a, const auto& b) {
					            return a.group < b.group;
				            });
		}
		reader.done();
		return good();
	}

	bool endpoints() noexcept
	{
		SchemaReader reader(bytes_, epStart_, bytes_.size(), error_, limits_, minor_);
		std::uint32_t pos = 0;
		for (unsigned c = 0; c < 3 && good(); ++c) {
			for (std::uint32_t g = 0; g < cats_[c] && good(); ++g) {
				const auto catalogIndex = ws_.catFirst_[c] + g;
				const auto& catalog = ws_.catalogs_[catalogIndex];
				const auto begin = pos;
				for (std::uint32_t local = 0; local < catalog.endpointCount && good(); ++local) {
					auto row = reader.record(c + 3);
					RuntimeEndpoint item;
					item.category = static_cast<descriptor::Category>(c + 1);
					item.catalogIndex = g;
					item.id = static_cast<std::uint32_t>(row.number(4));
					require(item.id == (g << 16 | local), ParseError::Endpoint);
					item.typeId = static_cast<TypeId>(row.number(4));
					if (!require(item.typeId < required_.types, ParseError::TypeReference))
						break;
					if (c == 0) {
						item.capabilities = static_cast<std::uint8_t>(row.number(1));
						row.zeros(3);
						require(item.typeId != 0 &&
						            (item.capabilities == 1 || item.capabilities == 3),
						        ParseError::Endpoint);
					} else {
						const auto kind = ws_.types_[item.typeId].kind;
						require(kind == TypeKind::Void || kind == TypeKind::Struct,
						        ParseError::Endpoint);
						if (c == 2) {
							item.responseTypeId = static_cast<TypeId>(row.number(4));
							if (!require(item.responseTypeId < required_.types,
							             ParseError::TypeReference))
								break;
							const auto response = ws_.types_[item.responseTypeId].kind;
							require(response == TypeKind::Void || response == TypeKind::Struct,
							        ParseError::Endpoint);
						}
					}
					item.name = row.text();
					row.done();
					if (good())
						ws_.endpoints_[pos++] = item;
				}
				if (good())
					uniqueNames(ws_.endpoints_.subspan(begin, catalog.endpointCount),
					            [](const auto& a, const auto& b) {
						            return a.id < b.id;
					            });
			}
		}
		reader.done();
		return good();
	}

	ParseResult run() noexcept
	{
		if (!header() || !countStorage())
			return refused();
		if (required_.types > ws_.types_.size() || required_.members > ws_.members_.size() ||
		    required_.enumEntries > ws_.enums_.size() ||
		    required_.catalogs > ws_.catalogs_.size() ||
		    required_.endpoints > ws_.endpoints_.size())
			return refused(Status::Capacity);
		if (!types() || !typeNames() || !catalogs() || !endpoints())
			return refused();
		// One canonical encoding per model: without names and attributes the
		// descriptor is v3.0, so minor 1 must carry at least one of them.
		if (!require(minor_ == descriptor::binaryMinor || extended_, ParseError::Header))
			return refused();
		ws_.bytes_ = bytes_;
		ws_.fingerprint_ = fingerprint_;
		ws_.minor_ = minor_;
		ws_.counts_ = required_;
		ws_.published_ = true;
		RuntimeSchema schema;
		schema.owner_ = &ws_;
		schema.generation_ = ws_.generation_;
		return {Status::Ok, ParseError::None, required_, schema};
	}
};

} // namespace telemetry::runtime::detail

namespace telemetry::runtime {
inline ParseResult RuntimeSchema::parse(Input bytes, RuntimeSchemaWorkspace& workspace,
                                        ParseLimits limits) noexcept
{
	return detail::SchemaParser::parse(bytes, workspace, limits);
}
} // namespace telemetry::runtime
#endif // TELEMETRY_RUNTIME_DETAIL_SCHEMA_PARSER_HPP
