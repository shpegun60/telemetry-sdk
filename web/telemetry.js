/*
 * Validate structural telemetry v3.0/v3.1 descriptors and encode their exact payload types.
 *
 * One checked model supplies immutable shapes and private endpoint indexes to all
 * payload operations. Descriptor input is copied; decoded values own their data,
 * U64/S64 stay BigInt, and callers own framing, status envelopes and session policy.
 * v3.1 type names appear as type.name on Enum/Struct; attribute areas are validated
 * and skipped, so they never change the model.
 *
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */

export const TypeKind = Object.freeze({Void : 0, Scalar : 1, Enum : 2, Struct : 3, Array : 4});
// Resource ceilings constrain parser work; per-load overrides can only tighten them.
export const defaultLimits = Object.freeze({
	descriptorBytes : 4194304,
	typeCount : 4096,
	structMembers : 256,
	arrayElements : 65536,
	stringBytes : 4096,
	enumEntries : 65536,
	catalogCount : 65536,
	endpointCount : 65536,
	valueWireBytes : 1048576,
	typeDepth : 32,
	expandedValueNodes : 262144,
	// Group-tree ceilings: hosts are lenient (a producer's defaults are 8/64/256).
	groupDepth : 255,
	nameBytes : 4096,
	pathBytes : 4096,
});

// Public format refusal for malformed bytes or an unrepresentable caller value.
// Public methods:
// - constructor(): Explain format refusal.
export class TelemetryFormatError extends Error {
	constructor(reason)
	{
		super(reason);
		this.name = 'TelemetryFormatError';
	}
}

const require = (condition, reason) => {
	if (!condition)
		throw new TelemetryFormatError(reason);
};
const utf8 = new TextDecoder('utf-8', {fatal : true, ignoreBOM : true});
const schemas = new WeakMap();
const scalarWidths = [ 0, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8 ];
const signedCodes = new Set([ 3, 5, 7, 9 ]);
const u32Max = 0xffffffff;
const typeNamed = 1;            // v3.1 Type prefix typeFlags bit 0
const recordAttributes = 1;     // v3.1 record header flags bit 0
const attributeHeaderBytes = 5; // v3.1 attribute: u8 tag, u32 length
const headerGroupTree = 1;      // v3.1 descriptor header flags bit 0
const groupCategory = 0;        // v3.1 group record category
const pathCharacters = /[/.[\]\\]/;
const utf8Length = text => new TextEncoder().encode(text).length;

// Preserve a view's byte offset/length. The result borrows input unless the
// public operation explicitly snapshots it before retaining descriptor data.
function bytesOf(input)
{
	if (input instanceof ArrayBuffer)
		return new Uint8Array(input);
	if (ArrayBuffer.isView(input))
		return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
	throw new TelemetryFormatError('Expected an ArrayBuffer or byte view');
}

function limitsFor(overrides)
{
	const limits = {...defaultLimits};
	for (const [key, value] of Object.entries(overrides)) {
		require(Object.hasOwn(defaultLimits, key), 'Unknown resource ceiling');
		require(Number.isSafeInteger(value) && value >= 0 && value <= defaultLimits[key],
		        'Resource ceilings may only tighten the default profile');
		limits[key] = value;
	}
	return limits;
}

function freezeShape(value)
{
	if (value && typeof value === 'object') {
		for (const item of Object.values(value))
			freezeShape(item);
		Object.freeze(value);
	}
	return value;
}

// Hash once when loading a descriptor. The stored fingerprint bytes are zero
// during hashing, exactly as in the producer. No value/packet hash is invented.
export function descriptorFingerprint(input)
{
	const bytes = bytesOf(input);
	let hash = 0xcbf29ce484222325n;
	for (let i = 0; i < bytes.length; ++i) {
		const byte = i >= 16 && i < 24 ? 0 : bytes[i];
		hash = BigInt.asUintN(64, (hash ^ BigInt(byte)) * 0x100000001b3n);
	}
	return hash;
}

// A reader owns one bounded cursor into borrowed bytes. Child record readers
// cannot consume neighboring records; take() refuses before advancing on failure.
// Public methods:
// - constructor(): Borrow bounded bytes.
// - take(): Advance checked cursor.
// - u8(): Read byte value.
// - u16(): Read LE integer.
// - u32(): Read LE integer.
// - u64(): Read LE BigInt.
// - zeros(): Check reserved bytes.
// - text(): Decode bounded UTF8.
// - record(): Borrow bounded record.
// - done(): Check exact consumption or skip a validated attribute area.
class Reader {
	constructor(bytes, begin, end, limits, minor = 0)
	{
		this.bytes = bytes;
		this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
		this.pos = begin;
		this.end = end;
		this.limits = limits;
		this.minor = minor;
		this.attributes = false;
	}

	take(count)
	{
		require(count <= this.end - this.pos, 'Truncated record');
		const start = this.pos;
		this.pos += count;
		return start;
	}

	u8()
	{
		return this.view.getUint8(this.take(1));
	}

	u16()
	{
		return this.view.getUint16(this.take(2), true);
	}

	u32()
	{
		return this.view.getUint32(this.take(4), true);
	}

	u64()
	{
		return this.view.getBigUint64(this.take(8), true);
	}

	zeros(count)
	{
		const start = this.take(count);
		for (let i = 0; i < count; ++i)
			require(this.bytes[start + i] === 0, 'Nonzero reserved bytes');
	}

	text()
	{
		const count = this.u32();
		require(count > 0 && count <= this.limits.stringBytes, 'String budget');
		const start = this.take(count);
		const raw = this.bytes.subarray(start, start + count);
		require(!raw.includes(0), 'Embedded NUL');
		try {
			return utf8.decode(raw);
		} catch {
			throw new TelemetryFormatError('Invalid UTF-8');
		}
	}

	record(kind)
	{
		const offset = this.pos;
		require(this.u8() === kind, 'Record kind/order');
		require(this.u8() === 1, 'Record version');
		// v3.0 reserves the record flags; v3.1 defines bit 0 (attribute area) only.
		const flags = this.u16();
		require(flags === 0 || (this.minor >= 1 && flags === recordAttributes),
		        'Nonzero reserved bytes');
		const count = this.u32();
		const start = this.take(count);
		const row = new Reader(this.bytes, start, start + count, this.limits, this.minor);
		row.offset = offset;
		row.attributes = flags === recordAttributes;
		return row;
	}

	// Without an attribute area the payload ends exactly here. With one, it runs
	// to the record end as {u8 tag, u32 length, value} with tags strictly
	// ascending; each value describes the whole record and is skipped by its
	// length. Advisory/application tags are validated and skipped.
	done()
	{
		if (!this.attributes) {
			require(this.pos === this.end, 'Extra or missing payload bytes');
			return;
		}
		require(this.pos < this.end, 'Attribute flag without attribute');
		let previous = -1;
		while (this.pos < this.end) {
			require(this.end - this.pos >= attributeHeaderBytes, 'Attribute header cut short');
			const tag = this.u8();
			const length = this.u32();
			require(length <= this.end - this.pos, 'Attribute value past the record');
			this.pos += length;
			require(tag > previous, 'Attribute tag out of order or repeated');
			previous = tag;
			const category = attributeTagClass(tag);
			require(category !== 'reserved', 'Reserved attribute tag');
			// v3.1 defines no critical tag, so every critical tag is unknown here.
			require(category !== 'critical', 'Unknown critical attribute tag');
		}
	}
}

// v3.1 type names are [scope "::"] name: at most one "::", each part nonempty
// and without ':' or the path characters / . [ ] \\.
function typeNameShapeValid(name)
{
	const separator = name.indexOf('::');
	const parts =
	    separator < 0 ? [ name ] : [ name.slice(0, separator), name.slice(separator + 2) ];
	return parts.every(part => part.length > 0 && !/[:/.[\]\\]/.test(part));
}

// A group or endpoint name of a tree is a path segment: no / . [ ] or backslash.
const treeNameValid = name => !pathCharacters.test(name);

// A flat (v3.0) name in a path: a backslash before / . [ ] and backslash.
export function escapePathSegment(name)
{
	return name.replace(/[/.[\]\\]/g, c => '\\' + c);
}

// group/.../endpoint then .member and [i]: the segments up to the first
// unescaped '.' or '[', and the member suffix. undefined if malformed.
function splitPath(path)
{
	if (typeof path !== 'string')
		return undefined;
	const segments = [];
	let current = '', i = 0;
	for (; i < path.length; ++i) {
		const c = path[i];
		if (c === '\\') {
			if (i + 1 === path.length)
				return undefined;
			current += path[++i];
		} else if (c === '/') {
			if (current === '')
				return undefined;
			segments.push(current);
			current = '';
		} else if (c === '.' || c === '[') {
			break;
		} else if (c === ']') {
			return undefined;
		} else {
			current += c;
		}
	}
	if (current === '')
		return undefined;
	segments.push(current);
	return {segments, member : path.slice(i)};
}

// v3.1 u8 attribute tag ranges: SDK advisory, application, critical; 0x00, 0x80
// and 0xC0..0xFF are reserved.
function attributeTagClass(tag)
{
	if (tag >= 0x01 && tag <= 0x3f)
		return 'advisory';
	if (tag >= 0x40 && tag <= 0x7f)
		return 'application';
	if (tag >= 0x81 && tag <= 0xbf)
		return 'critical';
	return 'reserved';
}

// Only this parser creates a model accepted by the codecs. Arrays and rows
// are frozen; the lookup Maps are private, so callers cannot alter references
// or resource bounds after validation. Input byte storage is not retained.
export function parseDescriptor(input, overrides = {})
{
	const limits = limitsFor(overrides);
	const source = bytesOf(input);
	require(source.length >= 64 && source.length <= limits.descriptorBytes, 'Descriptor budget');
	const bytes = source.slice();
	const header = new Reader(bytes, 0, 64, limits);
	require(header.u32() === 0x33534454, 'Descriptor magic'); // TDS3, LE
	require(header.u16() === 3, 'Descriptor version');
	const minor = header.u16(); // 0: v3.0; 1: v3.1 type names and attribute areas
	require(minor === 0 || minor === 1, 'Descriptor version');
	require(header.u16() === 64, 'Header size/flags');
	// v3.0 reserves the header flags; v3.1 defines bit 0, GroupTree.
	const headerFlags = header.u16();
	require(headerFlags === 0 || (minor >= 1 && headerFlags === headerGroupTree),
	        'Header size/flags');
	const tree = headerFlags === headerGroupTree;
	const total = header.u32();
	const fingerprint = header.u64();
	const typeCount = header.u32();
	const catalogsCount = [], endpointsCount = [];
	for (let i = 0; i < 3; ++i) {
		catalogsCount.push(header.u32());
		endpointsCount.push(header.u32());
	}
	const typesOffset = header.u32();
	const catalogsOffset = header.u32();
	const endpointsOffset = header.u32();
	const sum = values => values.reduce((a, b) => a + b, 0);
	require(total === bytes.length, 'Total length');
	require(typeCount >= 12 && typeCount <= limits.typeCount, 'Type budget');
	// In a tree every family's catalog count is the group count G.
	require(!tree ||
	            (catalogsCount[0] === catalogsCount[1] && catalogsCount[1] === catalogsCount[2]),
	        'Group tree catalog counts differ');
	const records = tree ? catalogsCount[0] : sum(catalogsCount);
	require(records <= limits.catalogCount, 'Catalog budget');
	require(sum(endpointsCount) <= limits.endpointCount, 'Endpoint budget');
	require(catalogsCount.every(count => count <= 65536), 'Group index capacity');
	require(typesOffset === 64 && typesOffset <= catalogsOffset &&
	            catalogsOffset <= endpointsOffset && endpointsOffset <= total,
	        'Section offsets');
	require(typeCount * 20 <= catalogsOffset - typesOffset, 'Type count/bytes');
	require(records * 29 <= endpointsOffset - catalogsOffset, 'Catalog count/bytes');
	require(sum(endpointsCount) * 21 <= total - endpointsOffset, 'Endpoint count/bytes');
	require(fingerprint === descriptorFingerprint(bytes), 'Descriptor fingerprint');

	const types = [];
	const typeNames = new Set();
	let extended = false; // minor 1 must carry a type name or an attribute area
	let enumCount = 0;
	const reference = (id, current) => {
		require(id > 0 && id < current, 'Backward non-Void type reference');
		return types[id];
	};
	let reader = new Reader(bytes, typesOffset, catalogsOffset, limits, minor);
	for (let i = 0; i < typeCount; ++i) {
		const row = reader.record(1);
		extended ||= row.attributes;
		require(row.u32() === i, 'Positional type ID');
		const kind = row.u8();
		const typeFlags = row.u8();
		require(typeFlags === 0 || (minor >= 1 && typeFlags === typeNamed),
		        'Nonzero reserved bytes');
		row.zeros(2);
		const wireBytes = row.u32();
		require(wireBytes <= limits.valueWireBytes, 'Value wire budget');
		require((i === 0 && kind === 0) || (i > 0 && i < 12 && kind === 1) ||
		            (i >= 12 && kind >= 2 && kind <= 4),
		        'Type kind/builtin position');
		require(!row.attributes || kind >= 2, 'Attributes on Void/Scalar type record');
		const type = {id : i, kind, wireBytes, depth : 0, nodes : 0, offset : row.offset};
		if (typeFlags === typeNamed) {
			require(kind === TypeKind.Enum || kind === TypeKind.Struct,
			        'Type name on a type other than Enum/Struct');
			type.name = row.text();
			require(typeNameShapeValid(type.name), 'Type name shape');
			require(type.name.split('::').every(part => utf8Length(part) <= limits.nameBytes) &&
			            utf8Length(type.name) <= limits.pathBytes,
			        'Name budget');
			require(!typeNames.has(type.name), 'Duplicate type name');
			typeNames.add(type.name);
			extended = true;
		}
		let expectedBytes = 0;
		if (kind === TypeKind.Scalar) {
			type.scalarCode = row.u8();
			row.zeros(3);
			require(type.scalarCode === i, 'Builtin scalar code');
			expectedBytes = scalarWidths[i];
			type.nodes = 1;
		} else if (kind === TypeKind.Enum) {
			type.underlyingTypeId = row.u32();
			const base = reference(type.underlyingTypeId, i);
			require(base.kind === 1 && base.scalarCode >= 2 && base.scalarCode <= 9,
			        'Enum underlying integer');
			const count = row.u32();
			enumCount += count;
			require(enumCount <= limits.enumEntries, 'Enum budget');
			expectedBytes = base.wireBytes;
			require(count * (expectedBytes + 5) <= row.end - row.pos, 'Enum count/bytes');
			type.entries = [];
			for (let j = 0; j < count; ++j) {
				const code = readScalar(row.view, row.take(expectedBytes), base.scalarCode);
				require(j === 0 || type.entries[j - 1].code < code, 'Enum code order/duplicate');
				type.entries.push({code, name : row.text()});
			}
			type.nodes = 1;
		} else if (kind === TypeKind.Struct) {
			const count = row.u32();
			require(count <= limits.structMembers, 'Struct member budget');
			require(count * 9 <= row.end - row.pos, 'Member count/bytes');
			type.members = [];
			type.depth = type.nodes = 1;
			const names = new Set();
			for (let j = 0; j < count; ++j) {
				const typeId = row.u32();
				const child = reference(typeId, i);
				const name = row.text();
				require(!tree || utf8Length(name) <= limits.nameBytes, 'Name budget');
				require(!names.has(name), 'Duplicate member name');
				names.add(name);
				type.members.push({typeId, name});
				expectedBytes += child.wireBytes;
				type.nodes += child.nodes;
				type.depth = Math.max(type.depth, child.depth + 1);
			}
		} else if (kind === TypeKind.Array) {
			type.elementTypeId = row.u32();
			const child = reference(type.elementTypeId, i);
			type.elementCount = row.u32();
			require(type.elementCount <= limits.arrayElements, 'Array element budget');
			expectedBytes = child.wireBytes * type.elementCount;
			type.depth = child.depth + 1;
			type.nodes = 1 + child.nodes * type.elementCount;
		}
		require(wireBytes === expectedBytes, 'Wire size mismatch');
		require(type.depth <= limits.typeDepth, 'Type depth budget');
		require(type.nodes <= limits.expandedValueNodes, 'Expanded value node budget');
		row.done();
		types.push(type);
	}
	reader.done();

	const catalogs = [ [], [], [] ];
	reader = new Reader(bytes, catalogsOffset, endpointsOffset, limits, minor);
	const groups = [];
	const firsts = [ 0, 0, 0 ];
	for (let index = 0; tree && index < catalogsCount[0]; ++index) {
		// One group of the shared tree, in pre-order.
		const row = reader.record(2);
		extended ||= row.attributes;
		require(row.u8() === groupCategory, 'Catalog category/order');
		row.zeros(1);
		const depth = row.u16();
		const counts = [ row.u32(), row.u32(), row.u32() ];
		const name = row.text();
		require(depth < limits.groupDepth, 'Group depth budget');
		require(depth <= (index === 0 ? 0 : groups[index - 1].depth + 1), 'Group depth outline');
		require(treeNameValid(name), 'Tree name character');
		let parent;
		if (depth > 0) {
			parent = index - 1;
			while (groups[parent].depth >= depth)
				parent = groups[parent].parent;
		}
		const path = parent === undefined ? name : groups[parent].path + '/' + name;
		require(utf8Length(name) <= limits.nameBytes && utf8Length(path) <= limits.pathBytes,
		        'Name budget');
		groups.push({
			index,
			name,
			depth,
			parent,
			path,
			fieldCount : counts[0],
			commandCount : counts[1],
			serviceCount : counts[2],
			firstField : firsts[0],
			firstCommand : firsts[1],
			firstService : firsts[2],
			offset : row.offset
		});
		for (let category = 0; category < 3; ++category) {
			require(counts[category] <= 65536 &&
			            counts[category] <= endpointsCount[category] - firsts[category],
			        'Catalog entry count');
			catalogs[category].push({
				group : index,
				first : firsts[category],
				count : counts[category],
				name,
				offset : row.offset
			});
			firsts[category] += counts[category];
		}
		row.done();
	}
	if (tree)
		require(firsts.every((first, category) => first === endpointsCount[category]),
		        'Catalog total entries');
	for (let category = 0; !tree && category < 3; ++category) {
		let first = 0;
		const names = new Set();
		for (let group = 0; group < catalogsCount[category]; ++group) {
			const row = reader.record(2);
			extended ||= row.attributes;
			require(row.u8() === category + 1, 'Catalog category/order');
			row.zeros(3);
			require(row.u32() === group && row.u32() === first, 'Catalog positional range');
			const count = row.u32();
			require(count <= 65536 && count <= endpointsCount[category] - first,
			        'Catalog entry count');
			const name = row.text();
			require(!names.has(name), 'Duplicate catalog name');
			names.add(name);
			catalogs[category].push({group, first, count, name, offset : row.offset});
			first += count;
			row.done();
		}
		require(first === endpointsCount[category], 'Catalog total entries');
	}
	reader.done();

	const endpoints = [ [], [], [] ];
	reader = new Reader(bytes, endpointsOffset, total, limits, minor);
	for (let category = 0; category < 3; ++category) {
		for (const catalog of catalogs[category]) {
			const names = new Set();
			for (let local = 0; local < catalog.count; ++local) {
				const row = reader.record(category + 3);
				extended ||= row.attributes;
				const id = row.u32();
				require(id === catalog.group * 65536 + local, 'Endpoint positional ID');
				const typeId = row.u32();
				require(typeId < types.length, 'Endpoint type reference');
				const endpoint = {id, offset : row.offset};
				if (category === 0) {
					endpoint.typeId = typeId;
					require(typeId !== 0, 'Void Field');
					endpoint.capabilities = row.u8();
					require([ 1, 3 ].includes(endpoint.capabilities), 'Field capabilities');
					row.zeros(3);
				} else {
					endpoint.requestTypeId = typeId;
					require([ 0, 3 ].includes(types[typeId].kind), 'Request must be Void/Struct');
					if (category === 2) {
						endpoint.responseTypeId = row.u32();
						require(endpoint.responseTypeId < types.length &&
						            [ 0, 3 ].includes(types[endpoint.responseTypeId].kind),
						        'Response must be Void/Struct');
					}
				}
				endpoint.name = row.text();
				require(!names.has(endpoint.name), 'Duplicate endpoint name');
				if (tree) {
					require(treeNameValid(endpoint.name), 'Tree name character');
					const path = groups[catalog.group].path + '/' + endpoint.name;
					require(utf8Length(endpoint.name) <= limits.nameBytes &&
					            utf8Length(path) <= limits.pathBytes,
					        'Name budget');
				}
				names.add(endpoint.name);
				row.done(); // Only a flagged, skippable attribute area may follow the name.
				endpoints[category].push(endpoint);
			}
		}
	}
	reader.done();
	if (tree) {
		// Names are unique among everything one group holds, like a folder:
		// its subgroups and the endpoints of all three families.
		const held = new Map();
		const hold = (key, name) => {
			const names = held.get(key) ?? new Set();
			require(!names.has(name), 'Duplicate sibling name');
			names.add(name);
			held.set(key, names);
		};
		for (const group of groups)
			hold(group.parent ?? -1, group.name);
		for (const rows of endpoints)
			for (const endpoint of rows)
				hold(Math.floor(endpoint.id / 65536), endpoint.name);
	}
	// One canonical encoding per model: without names, attributes and a tree a
	// descriptor is v3.0, so minor 1 must carry at least one of them.
	require(minor === 0 || extended || tree,
	        'Descriptor version: minor 1 without a type name or attribute');
	const model = freezeShape({
		fingerprint,
		byteLength : total,
		minor,
		groupTree : tree,
		groups : tree ? groups : undefined,
		types,
		fieldCatalogs : catalogs[0],
		commandCatalogs : catalogs[1],
		serviceCatalogs : catalogs[2],
		fields : endpoints[0],
		commands : endpoints[1],
		services : endpoints[2],
	});
	schemas.set(model, {
		endpoints : endpoints.map(rows => new Map(rows.map(row => [row.id, row]))),
		header : bytes.slice(0, 64)
	});
	return model;
}

// Type names are unique in a descriptor, so a name is a key. An unnamed type
// (and every array, scalar and Void) has no name and is never found.
export function findType(model, name)
{
	require(schemas.has(model), 'Expected a validated descriptor model');
	if (typeof name !== 'string' || name.length === 0)
		return undefined;
	return model.types.find(type => type.name === name);
}

// A group of a GroupTree descriptor by its path, group/.../group, or
// undefined. A flat (v3.0) descriptor has no shared groups.
export function findGroup(model, path)
{
	require(schemas.has(model), 'Expected a validated descriptor model');
	const split = splitPath(path);
	if (!model.groupTree || split === undefined || split.member !== '')
		return undefined;
	let parent, found;
	for (const segment of split.segments) {
		found = model.groups.find(group => group.parent === parent && group.name === segment);
		if (found === undefined)
			return undefined;
		parent = found.index;
	}
	return found;
}

const familyRows = model => [model.fields, model.commands, model.services];
const familyCatalogs = model => [model.fieldCatalogs, model.commandCatalogs, model.serviceCatalogs];

// The endpoint at group/.../endpoint, then an optional .member and [i] suffix:
// {category (0 Field, 1 Command, 2 Service), endpoint, member}, or undefined.
// In a tree the path alone is unique. In a flat descriptor the group is one
// segment with / . [ ] and backslash escaped, and a path that names endpoints
// of two families is ambiguous (undefined).
export function findEndpoint(model, path)
{
	require(schemas.has(model), 'Expected a validated descriptor model');
	const split = splitPath(path);
	if (split === undefined || split.segments.length < 2)
		return undefined;
	const {segments, member} = split;
	const leaf = segments[segments.length - 1];
	const matches = [];
	if (model.groupTree) {
		const group = findGroup(model, segments.slice(0, -1).map(escapePathSegment).join('/'));
		if (group === undefined)
			return undefined;
		familyCatalogs(model).forEach((catalogs, category) => {
			const catalog = catalogs[group.index];
			for (const endpoint of familyRows(model)[category].slice(catalog.first,
			                                                         catalog.first + catalog.count))
				if (endpoint.name === leaf)
					matches.push({category, endpoint, member});
		});
	} else if (segments.length === 2) {
		familyCatalogs(model).forEach((catalogs, category) => {
			for (const catalog of catalogs) {
				if (catalog.name !== segments[0])
					continue;
				for (const endpoint of familyRows(model)[category].slice(
				         catalog.first, catalog.first + catalog.count))
					if (endpoint.name === leaf)
						matches.push({category, endpoint, member});
			}
		});
	}
	return matches.length === 1 ? matches[0] : undefined;
}

// The path of one endpoint of a family (0 Field, 1 Command, 2 Service):
// group/.../endpoint in a tree, escaped group/endpoint in a flat descriptor.
export function endpointPath(model, category, endpoint)
{
	require(schemas.has(model), 'Expected a validated descriptor model');
	const group = Math.floor(endpoint.id / 65536);
	if (model.groupTree)
		return model.groups[group].path + '/' + endpoint.name;
	return escapePathSegment(familyCatalogs(model)[category][group].name) + '/' +
	       escapePathSegment(endpoint.name);
}

// Compare the full admitted header with the parser-owned bytes. A fingerprint
// alone is not the runtime admission contract; arbitrary lookalike models fail.
export function matchesDescriptorHeader(model, input)
{
	const schema = schemas.get(model);
	if (!schema)
		return false;
	try {
		const observed = bytesOf(input);
		return observed.length === 64 && schema.header.every((byte, i) => observed[i] === byte);
	} catch {
		return false;
	}
}

function typeOf(model, id)
{
	require(schemas.has(model), 'Expected a validated descriptor model');
	require(Number.isInteger(id) && id >= 0 && id < model.types.length, 'Unknown type ID');
	return model.types[id];
}

function endpointOf(model, category, id)
{
	require(schemas.has(model), 'Expected a validated descriptor model');
	require(Number.isInteger(id) && id >= 0 && id <= u32Max, 'Invalid packed u32 ID');
	const endpoint = schemas.get(model).endpoints[category].get(id);
	require(endpoint !== undefined, 'Unknown endpoint ID');
	return endpoint;
}

function readScalar(view, offset, code)
{
	switch (code) {
		case 1: {
			const raw = view.getUint8(offset);
			require(raw <= 1, 'Invalid bool representation');
			return raw !== 0;
		}
		case 2:
			return view.getUint8(offset);
		case 3:
			return view.getInt8(offset);
		case 4:
			return view.getUint16(offset, true);
		case 5:
			return view.getInt16(offset, true);
		case 6:
			return view.getUint32(offset, true);
		case 7:
			return view.getInt32(offset, true);
		case 8:
			return view.getBigUint64(offset, true);
		case 9:
			return view.getBigInt64(offset, true);
		case 10:
			return view.getFloat32(offset, true);
		case 11:
			return view.getFloat64(offset, true);
		default:
			throw new TelemetryFormatError('Unknown scalar code');
	}
}

function writeScalar(view, offset, code, value, original)
{
	// Fixed-size scalar representations are canonical except Bool. Validate the
	// old leaf using the same reader before copying any of its floating bits.
	const previous = original ? readScalar(original, offset, code) : undefined;
	if (code === 1) {
		require(typeof value === 'boolean', 'Bool requires a boolean');
		view.setUint8(offset, value ? 1 : 0);
	} else if (code === 8 || code === 9) {
		require(typeof value === 'bigint', 'U64/S64 requires BigInt');
		const signed = code === 9;
		const minimum = signed ? -(1n << 63n) : 0n;
		const maximum = signed ? (1n << 63n) - 1n : (1n << 64n) - 1n;
		require(value >= minimum && value <= maximum, 'Integer representation range');
		if (signed)
			view.setBigInt64(offset, value, true);
		else
			view.setBigUint64(offset, value, true);
	} else if (code === 10 || code === 11) {
		require(typeof value === 'number', 'Float requires a Number');
		require(code !== 10 || !Number.isFinite(value) || Number.isFinite(Math.fround(value)),
		        'F32 representation overflow');
		if (code === 10)
			view.setFloat32(offset, value, true);
		else
			view.setFloat64(offset, value, true);
	} else {
		const bits = scalarWidths[code] * 8;
		const signed = signedCodes.has(code);
		const minimum = signed ? -(2 ** (bits - 1)) : 0;
		const maximum = signed ? 2 ** (bits - 1) - 1 : 2 ** bits - 1;
		require(typeof value === 'number' && Number.isInteger(value) && value >= minimum &&
		            value <= maximum,
		        'Integer representation range');
		const setters = {
			2 : 'setUint8',
			3 : 'setInt8',
			4 : 'setUint16',
			5 : 'setInt16',
			6 : 'setUint32',
			7 : 'setInt32'
		};
		view[setters[code]](offset, value, true);
	}
	if (original && (code === 10 || code === 11) && Number.isNaN(previous) && Number.isNaN(value))
		for (let i = 0; i < scalarWidths[code]; ++i)
			view.setUint8(offset + i, original.getUint8(offset + i));
}

// Recursion follows only validated backward type references, whose depth and
// expanded node counts were bounded before any native value is constructed.
function decodeAt(model, type, view, offset)
{
	switch (type.kind) {
		case TypeKind.Void:
			return undefined;
		case TypeKind.Scalar:
			return readScalar(view, offset, type.scalarCode);
		case TypeKind.Enum:
			return readScalar(view, offset, model.types[type.underlyingTypeId].scalarCode);
		case TypeKind.Struct: {
			// Even names such as __proto__/constructor remain ordinary members.
			const value = Object.create(null);
			for (const member of type.members) {
				const child = model.types[member.typeId];
				value[member.name] = decodeAt(model, child, view, offset);
				offset += child.wireBytes;
			}
			return value;
		}
		case TypeKind.Array: {
			const child = model.types[type.elementTypeId];
			const value = new Array(type.elementCount);
			for (let i = 0; i < type.elementCount; ++i) {
				value[i] = decodeAt(model, child, view, offset);
				offset += child.wireBytes;
			}
			return value;
		}
	}
}

function encodeAt(model, type, view, offset, value, original)
{
	switch (type.kind) {
		case TypeKind.Void:
			require(value === undefined, 'Void has no value');
			break;
		case TypeKind.Scalar:
			writeScalar(view, offset, type.scalarCode, value, original);
			break;
		case TypeKind.Enum:
			writeScalar(view, offset, model.types[type.underlyingTypeId].scalarCode, value,
			            original);
			break;
		case TypeKind.Struct:
			require(value !== null && typeof value === 'object' && !Array.isArray(value),
			        'Struct requires an object');
			require(Object.keys(value).length === type.members.length, 'Exact struct member count');
			for (const member of type.members) {
				require(Object.hasOwn(value, member.name), 'Missing struct member');
				const child = model.types[member.typeId];
				encodeAt(model, child, view, offset, value[member.name], original);
				offset += child.wireBytes;
			}
			break;
		case TypeKind.Array: {
			require(Array.isArray(value) && value.length === type.elementCount,
			        'Exact array length');
			const child = model.types[type.elementTypeId];
			for (let i = 0; i < type.elementCount; ++i) {
				encodeAt(model, child, view, offset, value[i], original);
				offset += child.wireBytes;
			}
			break;
		}
	}
}

// Allocate and return a complete canonical payload. Exact members/array length
// and scalar representation are checked; application meaning remains external.
export function encodeValue(model, typeId, value)
{
	const type = typeOf(model, typeId);
	const bytes = new Uint8Array(type.wireBytes);
	encodeAt(model, type, new DataView(bytes.buffer), 0, value);
	return bytes;
}

// Encode an edited DTO while retaining an existing NaN's exact wire bits when
// its new logical value is still NaN. The seed is snapshotted before DTO getters
// run, checked for exact length and canonical scalar representations, and never
// retained. Changing a NaN to a finite value uses the normal scalar writer.
export function encodeValuePreserving(model, typeId, value, originalPayload)
{
	const type = typeOf(model, typeId);
	const source = bytesOf(originalPayload);
	require(source.length === type.wireBytes, 'Exact original value payload length');
	const original = new Uint8Array(source);
	const bytes = new Uint8Array(type.wireBytes);
	encodeAt(model, type, new DataView(bytes.buffer), 0, value, new DataView(original.buffer));
	return bytes;
}

// Decode exactly one payload into independent JS values. U64/S64 remain BigInt,
// unknown representable enum codes remain codes, and Structs have no prototype.
export function decodeValue(model, typeId, input)
{
	const type = typeOf(model, typeId);
	const bytes = bytesOf(input);
	require(bytes.length === type.wireBytes, 'Exact value payload length');
	return decodeAt(model, type, new DataView(bytes.buffer, bytes.byteOffset, bytes.length), 0);
}

// Check the declared writable capability before producing Field payload bytes.
export const encodeFieldWrite = (model, id, value) => {
	const field = endpointOf(model, 0, id);
	require(field.capabilities === 3, 'Read-only Field');
	return encodeValue(model, field.typeId, value);
};
// Convenience APIs resolve positional endpoint IDs through private validated maps.
// An omitted request is valid only for Void; payloads contain no envelope.
export const decodeFieldValue = (model, id, input) =>
    decodeValue(model, endpointOf(model, 0, id).typeId, input);

export const encodeCommandRequest = (model, id, value = undefined) =>
    encodeValue(model, endpointOf(model, 1, id).requestTypeId, value);

export const encodeServiceRequest = (model, id, value = undefined) =>
    encodeValue(model, endpointOf(model, 2, id).requestTypeId, value);

export const decodeServiceResponse = (model, id, input) =>
    decodeValue(model, endpointOf(model, 2, id).responseTypeId, input);

// Validate the complete values file against this descriptor identity and shape.
// Unavailable tokens must carry zero payload bytes and do not become fake values.
// Each decoded Field is independent; no cross-Field sample instant is inferred.
export function decodeValues(model, input)
{
	typeOf(model, 0);
	const bytes = bytesOf(input);
	require(bytes.length >= 24, 'Short values header');
	const header = new Reader(bytes, 0, bytes.length, defaultLimits);
	require(header.u32() === 0x334c5654, 'Values magic'); // TVL3
	require(header.u16() === 3 && header.u16() === 0, 'Values version');
	require(header.u32() === model.fields.length, 'Values count');
	require(header.u32() === bytes.length, 'Values file length');
	require(header.u64() === model.fingerprint, 'Values descriptor fingerprint');
	const expected =
	    24 + model.fields.reduce((sum, field) => sum + 1 + model.types[field.typeId].wireBytes, 0);
	require(bytes.length === expected, 'Values token lengths');
	const result = [];
	for (const field of model.fields) {
		const status = header.u8();
		const width = model.types[field.typeId].wireBytes;
		require(status === 0 || status === 1, 'Unknown value status');
		let value;
		if (status === 1)
			header.zeros(width);
		else {
			const start = header.take(width);
			value = decodeValue(model, field.typeId, bytes.subarray(start, start + width));
		}
		result.push({id : field.id, status, value});
	}
	header.done();
	return result;
}

// A small client cache, not a telemetry session. Loading always validates new
// bytes; a rare equal-hash/different-byte pair is refused rather than aliased.
// Public methods:
// - constructor(): Set cache capacity.
// - load(): Validate cached descriptor.
// - clear(): Discard cached models.
// - size getter: Count cached models.
// - capacity getter: Report cache limit.
export class DescriptorCache {
	#entries = new Map();
	#capacity;

	constructor(capacity = 4)
	{
		require(Number.isInteger(capacity) && capacity > 0 && capacity <= 256, 'Cache capacity');
		this.#capacity = capacity;
	}

	// Retain a private descriptor byte copy and immutable model. A cache hit
	// is reused only after full validation and byte-for-byte identity checks.
	load(input, limits = {})
	{
		const source = bytesOf(input);
		const maximum = limitsFor(limits).descriptorBytes;
		require(source.length >= 64 && source.length <= maximum, 'Descriptor budget');
		// One stable snapshot also covers a caller using shared byte storage.
		const bytes = source.slice();
		const model = parseDescriptor(bytes, limits);
		const previous = this.#entries.get(model.fingerprint);
		if (previous) {
			require(bytes.length === previous.bytes.length &&
			            bytes.every((byte, i) => byte === previous.bytes[i]),
			        'Fingerprint collision');
			this.#entries.delete(model.fingerprint);
			this.#entries.set(model.fingerprint, previous);
			return previous.model;
		}
		if (this.#entries.size === this.capacity)
			this.#entries.delete(this.#entries.keys().next().value);
		this.#entries.set(model.fingerprint, {model, bytes});
		return model;
	}

	clear()
	{
		this.#entries.clear();
	}

	get size()
	{
		return this.#entries.size;
	}

	get capacity()
	{
		return this.#capacity;
	}
}
