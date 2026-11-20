/* Execute one generated JS or TS-compiled-JS module against the independent
 * existing browser codec. Usage: node Check.mjs MODULE DESCRIPTOR_HEX [ORACLE_JS]
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath, pathToFileURL} from 'node:url';

const [modulePath, descriptorPath, oraclePath] = process.argv.slice(2);
assert.ok(modulePath && descriptorPath, 'MODULE and DESCRIPTOR_HEX required');
const generated = await import(pathToFileURL(path.resolve(modulePath)));
const oracle = await import(
    pathToFileURL(path.resolve(oraclePath ?? path.join(path.dirname(fileURLToPath(import.meta.url)),
                                                       '../../web/telemetry.js'))));
const bytes =
    new Uint8Array(Buffer.from(fs.readFileSync(descriptorPath, 'utf8').replace(/\s/g, ''), 'hex'));
const model = oracle.parseDescriptor(bytes);
let checks = 0;
const check = value => {
	assert.ok(value);
	++checks;
};
const equal = (a, b) => {
	assert.deepEqual(a, b);
	++checks;
};
const reject = fn => {
	assert.throws(fn, generated.TelemetryCodecError);
	++checks;
};
const codec = id => generated.codecs['Type_' + id];
const reachable = new Set();

function visit(id)
{
	if (reachable.has(id))
		return;
	reachable.add(id);
	const type = model.types[id];
	if (type.kind === 2)
		visit(type.underlyingTypeId);
	if (type.kind === 3)
		for (const member of type.members)
			visit(member.typeId);
	if (type.kind === 4)
		visit(type.elementTypeId);
}

visit(0);
for (const field of model.fields)
	visit(field.typeId);
for (const command of model.commands)
	visit(command.requestTypeId);
for (const service of model.services) {
	visit(service.requestTypeId);
	visit(service.responseTypeId);
}
equal(Object.keys(generated.codecs).sort(), [...reachable ].map(id => 'Type_' + id).sort());
equal(Object.keys(generated.enums).sort(),
      [...reachable ].filter(id => model.types[id].kind === 2).map(id => 'Type_' + id).sort());

function sample(type, seed = 0)
{
	switch (type.kind) {
		case 0:
			return undefined;
		case 1:
			if (type.scalarCode === 1)
				return seed % 2 === 0;
			if (type.scalarCode === 8)
				return BigInt(seed + 42);
			if (type.scalarCode === 9)
				return -BigInt(seed + 42);
			if (type.scalarCode >= 10)
				return seed ? -0 : 1.25;
			return seed + 3;
		case 2:
			return sample(model.types[type.underlyingTypeId], seed);
		case 3: {
			const result = Object.create(null);
			for (const member of type.members)
				result[member.name] = sample(model.types[member.typeId], seed);
			return result;
		}
		case 4:
			return Array.from({length : type.elementCount},
			                  () => sample(model.types[type.elementTypeId], seed));
	}
}

function roundTrip(type, value)
{
	const encoded = codec(type.id).encode(value);
	const reference = oracle.encodeValue(model, type.id, value);
	equal(encoded, reference);
	equal(codec(type.id).decode(encoded), oracle.decodeValue(model, type.id, reference));
	const wrapped = new Uint8Array(encoded.length + 8).fill(0x5a);
	const sliced = wrapped.subarray(3, 3 + encoded.length);
	codec(type.id).encodeInto(value, sliced);
	equal(sliced, encoded);
	check(wrapped.subarray(0, 3).every(x => x === 0x5a) &&
	      wrapped.subarray(3 + encoded.length).every(x => x === 0x5a));
	const view = new DataView(wrapped.buffer, 3, encoded.length);
	equal(codec(type.id).decode(view), oracle.decodeValue(model, type.id, encoded));
	codec(type.id).encodeInto(value, view);
	equal(sliced, encoded);
	equal(codec(type.id).decode(sliced), oracle.decodeValue(model, type.id, encoded));
	if (encoded.length % 2 === 0) {
		const typedBacking = new Uint8Array(encoded.length + 8).fill(0x5a);
		const typed = new Uint16Array(typedBacking.buffer, 4, encoded.length / 2);
		codec(type.id).encodeInto(value, typed);
		equal(typedBacking.subarray(4, 4 + encoded.length), encoded);
		equal(codec(type.id).decode(typed), oracle.decodeValue(model, type.id, encoded));
		check(typedBacking.subarray(0, 4).every(x => x === 0x5a) &&
		      typedBacking.subarray(4 + encoded.length).every(x => x === 0x5a));
	}
	if (type.kind === 3)
		check(Object.getPrototypeOf(codec(type.id).decode(sliced)) === null);
	if (type.kind === 3 || type.kind === 4) {
		const independent = codec(type.id).decode(sliced);
		sliced.fill(0);
		equal(independent, oracle.decodeValue(model, type.id, encoded));
	}
}

check(generated.fingerprint === model.fingerprint);
check(generated.descriptorSha256 === crypto.createHash('sha256').update(bytes).digest('hex'));
// The generated SDK carries its own descriptor's minor: 1 when it names a type.
equal(generated.wireVersion, {major : 3, minor : bytes[6] | bytes[7] << 8});
equal(
    generated.Operation,
    {FieldRead : 'FieldRead', FieldWrite : 'FieldWrite', Command : 'Command', Service : 'Service'});
check(Object.isFrozen(generated.wireVersion) && Object.isFrozen(generated.Operation));
generated.assertCompatibleFingerprint(model.fingerprint);
reject(() => generated.assertCompatibleFingerprint(model.fingerprint ^ 1n));
reject(() => generated.assertCompatibleFingerprint(Number(model.fingerprint)));
check(generated.matchesHeader(bytes.subarray(0, 64)));
const paddedHeader = new Uint8Array(70);
paddedHeader.set(bytes.subarray(0, 64), 2);
check(generated.matchesHeader(new DataView(paddedHeader.buffer, 2, 64)));
for (let length = 0; length < 64; ++length)
	check(!generated.matchesHeader(bytes.subarray(0, length)));
check(!generated.matchesHeader(new Uint8Array(65)));
for (let offset = 0; offset < 64; ++offset) {
	const altered = bytes.slice(0, 64);
	altered[offset] ^= 1;
	check(!generated.matchesHeader(altered));
}
for (const type of model.types.filter(type => codec(type.id))) {
	check(Object.isFrozen(codec(type.id)) && codec(type.id).wireBytes === type.wireBytes);
	for (let seed = 0; seed < 3; ++seed)
		roundTrip(type, sample(type, seed));
	reject(() => codec(type.id).decode(new Uint8Array(type.wireBytes + 1)));
	reject(() => codec(type.id).encodeInto(sample(type), new Uint8Array(type.wireBytes + 1)));
	if (type.wireBytes)
		reject(() => codec(type.id).decode(new Uint8Array(type.wireBytes - 1)));
	if (type.kind === 2) {
		check(Object.isFrozen(generated.enums['Type_' + type.id]));
		equal(Object.values(generated.enums['Type_' + type.id]),
		      type.entries.map(entry => entry.code));
	}
	if (type.kind === 3) {
		const valid = sample(type);
		reject(() => codec(type.id).encode(null));
		reject(() => codec(type.id).encode([]));
		const extra = Object.assign(Object.create(null), valid, {extraUnexpectedMember : 1});
		reject(() => codec(type.id).encode(extra));
		if (type.members.length) {
			const missing = Object.assign(Object.create(null), valid);
			delete missing[type.members[0].name];
			reject(() => codec(type.id).encode(missing));
			reject(() => codec(type.id).encode(Object.create(valid)));
		}
	}
	if (type.kind === 4) {
		reject(() => codec(type.id).encode(new Uint8Array(type.elementCount)));
		reject(() => codec(type.id).encode(Array(type.elementCount + 1).fill(0)));
	}
	// Read each field/index once. A second getter read must never bypass the
	// scalar representation checks by changing a validated value before writing.
	if ((type.kind === 3 && type.members.length) || (type.kind === 4 && type.elementCount)) {
		const expected = codec(type.id).encode(sample(type));
		for (const into of [false, true]) {
			const value = sample(type), key = type.kind === 3 ? type.members[0].name : '0';
			const original = value[key];
			let calls = 0;
			Object.defineProperty(value, key, {
				enumerable : true,
				get : () => ++calls === 1 ? original : undefined,
			});
			const output = into ? new Uint8Array(type.wireBytes) : codec(type.id).encode(value);
			if (into)
				codec(type.id).encodeInto(value, output);
			equal(output, expected);
			check(calls === 1);
		}
	}
}

for (const code of [2, 3, 4, 5, 6, 7, 8, 9]) {
	if (!codec(code))
		continue;
	const type = model.types[code], signed = [ 3, 5, 7, 9 ].includes(code);
	const bits = type.wireBytes * 8, big = bits === 64;
	const lo = signed ? -(1n << BigInt(bits - 1)) : 0n;
	const hi = (1n << BigInt(signed ? bits - 1 : bits)) - 1n;
	const convert = x => big ? x : Number(x);
	for (const value of [lo, hi, 0n, 1n])
		roundTrip(type, convert(value));
	for (const value of [lo - 1n, hi + 1n])
		reject(() => codec(code).encode(convert(value)));
	for (const value of [NaN, Infinity, 1.5, true, '1', null, undefined, big ? 1 : 1n]) {
		const output = new Uint8Array(type.wireBytes).fill(0x5a);
		reject(() => codec(code).encodeInto(value, output));
		check(output.every(x => x === 0x5a));
	}
}
if (codec(1)) {
	for (const value of [true, false])
		roundTrip(model.types[1], value);
	for (const value of [0, 1, null, undefined, 'true'])
		reject(() => codec(1).encode(value));
	for (const value of [2, 127, 255])
		reject(() => codec(1).decode(Uint8Array.of(value)));
}
reject(() => codec(0).encode(null));
if (codec(10))
	reject(() => codec(10).encode(Number.MAX_VALUE));
for (const id of [10, 11]) {
	if (!codec(id))
		continue;
	for (const value of [-0, 0, 1.25, -2.5, NaN, Infinity, -Infinity]) {
		roundTrip(model.types[id], value);
		check(Object.is(codec(id).decode(codec(id).encode(value)), value));
	}
	for (const value of ['1', 1n, null, undefined, true])
		reject(() => codec(id).encode(value));
}
for (const [id, patterns] of [
         [ 10, [ '00000080', '01000000', '0000807f', '0100c07f', 'ffff7fff' ] ], [
	         11, [ '0000000000000080', '0100000000000000', '000000000000f07f', '010000000000f87f' ]
         ]]) {
	if (!codec(id))
		continue;
	for (const pattern of patterns) {
		const raw = new Uint8Array(Buffer.from(pattern, 'hex'));
		const value = codec(id).decode(raw), expected = oracle.decodeValue(model, id, raw);
		check(Object.is(value, expected));
		if (!Number.isNaN(value))
			equal(codec(id).encode(value), raw);
		else
			check(Number.isNaN(codec(id).decode(codec(id).encode(value))));
	}
}
// Unknown enum representations stay open numbers/BigInts.
for (const type of model.types.filter(t => t.kind === 2 && codec(t.id))) {
	const base = model.types[type.underlyingTypeId];
	roundTrip(type, [ 8, 9 ].includes(base.scalarCode) ? 42n : 42);
}

// v3.1 type names: types.scope.Name is the codec (enum dictionary) of the type
// the JS decoder names scope::Name; no named type is missing or extra.
let namedCount = 0;
for (const type of model.types.filter(type => type.name !== undefined && codec(type.id))) {
	++namedCount;
	const parts = type.name.split('::');
	const holder = parts.length === 2 ? generated.types?.[parts[0]] : generated.types;
	const source = type.kind === 2 ? generated.enums : generated.codecs;
	check(holder !== undefined && Object.isFrozen(holder) &&
	      holder[parts[parts.length - 1]] === source['Type_' + type.id]);
}
// A codec or an enum dictionary is one type; any other object is a scope of them.
const isTypeLeaf = value => Object.hasOwn(value, 'wireBytes') ||
                            Object.values(value).every(x => typeof x === 'number' || typeof x === 'bigint');
if (namedCount === 0)
	check(generated.types === undefined);
else
	equal(Object.values(generated.types)
	          .reduce((total, child) => total + (isTypeLeaf(child) ? 1 : Object.keys(child).length), 0),
	      namedCount);

// A group tree nests group objects: subgroups and endpoints side by side, the
// family objects kept. Each endpoint carries its path, which the JS decoder resolves.
const familyNames = [ 'fields', 'commands', 'services' ];
const isGroup = value => value && typeof value === 'object' && value.types && value.fields &&
                         value.commands && value.services;
let treeGroups = 0;
function visitTree(value, depth)
{
	++treeGroups;
	const family = new Set(familyNames.flatMap(name => Object.values(value[name])));
	let direct = 0, prefix;
	for (const [key, child] of Object.entries(value)) {
		if (key === 'types' || familyNames.includes(key))
			continue;
		if (isGroup(child)) {
			visitTree(child, depth + 1);
			continue;
		}
		++direct;
		check(family.has(child));
	}
	check(direct === family.size);
	for (const endpoint of family) {
		const segments = endpoint.path.split('/');
		check(segments.length === depth + 2);
		const group = segments.slice(0, -1).join('/');
		check(prefix === undefined || prefix === group);
		prefix = group;
		const found = oracle.findEndpoint(model, endpoint.path);
		check(found !== undefined && found.endpoint.id === endpoint.id &&
		      found.category === endpoint.category - 1 && found.member === '');
		equal(oracle.endpointPath(model, endpoint.category - 1, found.endpoint), endpoint.path);
	}
}

let fieldCount = 0, commandCount = 0, serviceCount = 0;
const groupObjects = Object.values(generated).filter(isGroup);
if (model.groupTree) {
	for (const value of [...groupObjects ])
		visitTree(value, 0);
	for (let i = 0; i < groupObjects.length; ++i)
		for (const [key, child] of Object.entries(groupObjects[i]))
			if (key !== 'types' && isGroup(child))
				groupObjects.push(child);
	check(treeGroups === model.groups.length && groupObjects.length === model.groups.length);
}
for (const value of groupObjects) {
	check(Object.isFrozen(value) && Object.isFrozen(value.types));
	for (const alias of Object.values(value.types))
		check(alias === undefined || Object.isFrozen(alias));
	for (const category of ['fields', 'commands', 'services']) {
		check(Object.isFrozen(value[category]));
		for (const endpoint of Object.values(value[category])) {
			check(Object.isFrozen(endpoint));
			if (category === 'fields') {
				++fieldCount;
				check(endpoint.category === 1 && endpoint.read.operation === 'FieldRead' &&
				      Object.isFrozen(endpoint.read));
				check((endpoint.capabilities === 3) === Object.hasOwn(endpoint, 'write'));
				if (endpoint.write)
					check(endpoint.write.operation === 'FieldWrite' &&
					      Object.isFrozen(endpoint.write));
				equal(endpoint.read.decode(codec(endpoint.valueTypeId)
				                               .encode(sample(model.types[endpoint.valueTypeId]))),
				      sample(model.types[endpoint.valueTypeId]));
			} else {
				if (category === 'commands')
					++commandCount;
				else
					++serviceCount;
				check(endpoint.operation === (category === 'commands' ? 'Command' : 'Service'));
				equal(endpoint.encode(sample(model.types[endpoint.requestTypeId])),
				      codec(endpoint.requestTypeId)
				          .encode(sample(model.types[endpoint.requestTypeId])));
				if (category === 'services')
					equal(endpoint.decodeResponse(
					          codec(endpoint.responseTypeId)
					              .encode(sample(model.types[endpoint.responseTypeId]))),
					      sample(model.types[endpoint.responseTypeId]));
			}
		}
	}
}
equal([ fieldCount, commandCount, serviceCount ],
      [ model.fields.length, model.commands.length, model.services.length ]);
console.log(`generated web ${path.basename(path.dirname(modulePath))}: ${checks} checks passed`);
