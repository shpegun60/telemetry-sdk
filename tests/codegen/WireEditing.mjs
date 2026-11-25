/* Opt-in generated codecs preserve original IEEE bytes while editing another leaf.
 * Usage: node WireEditing.mjs MODULE DESCRIPTOR_HEX METADATA BASELINE [ORACLE_JS]
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const [modulePath, descriptorPath, metadataPath, baselinePath, oraclePath] = process.argv.slice(2);
assert.ok(modulePath && descriptorPath && metadataPath && baselinePath,
          'Four input paths required');
const load = file => import(pathToFileURL(path.resolve(file)));
const generated = await load(modulePath), baseline = await load(baselinePath);
const {structuralMetadata} = await load(metadataPath);
const oracle = await load(oraclePath ?? path.join(path.dirname(fileURLToPath(import.meta.url)),
                                                  '../../web/telemetry.js'));
const descriptor =
    new Uint8Array(Buffer.from(fs.readFileSync(descriptorPath, 'utf8').trim(), 'hex'));
const model = oracle.parseDescriptor(descriptor);
let checks = 0;
const equal = (actual, expected) => {
	assert.deepEqual(actual, expected);
	++checks;
};
const check = value => {
	assert.ok(value);
	++checks;
};
const reject = action => {
	assert.throws(action, generated.TelemetryCodecError);
	++checks;
};
const codec = id => generated.codecs['Type_' + id];
equal(generated.fingerprint, model.fingerprint);
equal(structuralMetadata.fingerprint, '0x' + model.fingerprint.toString(16).padStart(16, '0'));
for (const [name, value] of Object.entries(generated.codecs)) {
	check(typeof value.encodePreserving === 'function');
	check(!Object.hasOwn(baseline.codecs[name], 'encodePreserving'));
}

function sample(type)
{
	switch (type.kind) {
		case 0:
			return undefined;
		case 1:
			return type.scalarCode === 1                   ? true
			       : type.scalarCode === 8                 ? 0xffffffffffffffffn
			       : type.scalarCode === 9                 ? -0x8000000000000000n
			       : type.scalarCode >= 10                 ? 1.25
			       : [ 3, 5, 7 ].includes(type.scalarCode) ? -3
			                                               : 3;
		case 2:
			return sample(model.types[type.underlyingTypeId]);
		case 3: {
			const value = Object.create(null);
			for (const member of type.members)
				value[member.name] = sample(model.types[member.typeId]);
			return value;
		}
		case 4:
			return Array.from({length : type.elementCount},
			                  () => sample(model.types[type.elementTypeId]));
	}
}

const vectors = [
	[ 10, [ 0x7fc12345n, 0xffc54321n, 0x7f812345n, 0xff854321n, 0n, 0x80000000n ] ],
	[
		11,
		[
			0x7ff8123456789abcn, 0xfff8abcdef123456n, 0x7ff0123456789abcn, 0xfff0abcdef123456n, 0n,
			0x8000000000000000n
		]
	]
];

function setBits(bytes, offset, code, bits)
{
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	if (code === 10)
		view.setUint32(offset, Number(bits), true);
	else
		view.setBigUint64(offset, bits, true);
}

for (const [code, bitsList] of vectors) {
	const type = model.types.find(row => row.kind === 1 && row.scalarCode === code);
	check(!!type && !!codec(type.id));
	for (const bits of bitsList) {
		const seed = new Uint8Array(type.wireBytes);
		setBits(seed, 0, code, bits);
		const value = codec(type.id).decode(seed);
		equal(codec(type.id).encodePreserving(value, seed), seed);
		equal(codec(type.id).encodePreserving(value, seed.buffer), seed);
		const padded = new Uint8Array(type.wireBytes + 9).fill(0x5a);
		padded.set(seed, 3);
		equal(codec(type.id).encodePreserving(value, padded.subarray(3, 3 + seed.length)), seed);
		equal(codec(type.id).encodePreserving(value, new DataView(padded.buffer, 3, seed.length)),
		      seed);
		const buffer = Buffer.alloc(seed.length + 7, 0x5a);
		buffer.set(seed, 2);
		equal(codec(type.id).encodePreserving(value, buffer.subarray(2, 2 + seed.length)), seed);
		equal(codec(type.id).encodePreserving(1.25, seed),
		      oracle.encodeValue(model, type.id, 1.25));
		const result = codec(type.id).encodePreserving(value, seed);
		check(result.buffer !== seed.buffer);
		const original = new Uint8Array(seed);
		seed.fill(0x6b);
		equal(result, original);
		reject(() => codec(type.id).encodePreserving(value, new Uint8Array(type.wireBytes - 1)));
		reject(() => codec(type.id).encodePreserving(value, new Uint8Array(type.wireBytes + 1)));
		if (Number.isNaN(value))
			equal(codec(type.id).encodePreserving(NaN, original), original);
		else {
			const opposite = Object.is(value, -0) ? 0 : -0;
			equal(codec(type.id).encodePreserving(opposite, original),
			      codec(type.id).encode(opposite));
		}
	}
	const finite = codec(type.id).encode(1.25);
	equal(codec(type.id).encodePreserving(NaN, finite), codec(type.id).encode(NaN));
	reject(() => codec(type.id).encodePreserving('NaN', finite));
}

// Every reachable generated type uses the ordinary validation/writer contract.
for (const name of Object.keys(generated.codecs)) {
	const id = Number(name.slice(5)), type = model.types[id], value = sample(type);
	const seed = oracle.encodeValue(model, id, value);
	equal(codec(id).encodePreserving(value, seed), seed);
	equal(codec(id).decode(seed), oracle.decodeValue(model, id, seed));
	reject(() => codec(id).encodePreserving(value, new Uint8Array(seed.length + 1)));
}
const bool = model.types.find(row => row.kind === 1 && row.scalarCode === 1);
reject(() => codec(bool.id).encodePreserving(true, new Uint8Array([ 2 ])));
reject(() => codec(bool.id).encodePreserving(2, new Uint8Array([ 1 ])));
reject(() => codec(bool.id).encodePreserving(true, 'not bytes'));

function leaves(id, path = [], offset = 0)
{
	const type = model.types[id];
	if (type.kind === 1 || type.kind === 2)
		return [ {type, path, offset} ];
	if (type.kind === 3) {
		const result = [];
		for (const member of type.members) {
			result.push(...leaves(member.typeId, [...path, member.name ], offset));
			offset += model.types[member.typeId].wireBytes;
		}
		return result;
	}
	if (type.kind === 4)
		return Array
		    .from({length : type.elementCount},
		          (_, index) => leaves(type.elementTypeId, [...path, index ],
		                               offset + index * model.types[type.elementTypeId].wireBytes))
		    .flat();
	return [];
}

function put(value, path, replacement)
{
	for (const key of path.slice(0, -1))
		value = value[key];
	value[path.at(-1)] = replacement;
}

// The actual native Request has nested Config with F32/F64 and independent fields.
const nested = model.types.find(
    type => type.kind === 3 && type.members.some(member => model.types[member.typeId].kind === 3) &&
            leaves(type.id).some(leaf => leaf.type.scalarCode === 10) &&
            leaves(type.id).some(leaf => leaf.type.scalarCode === 11));
check(!!nested && !!codec(nested.id));
const nestedLeaves = leaves(nested.id),
      floats = nestedLeaves.filter(leaf => leaf.type.scalarCode >= 10);
const changed = nestedLeaves.find(leaf => leaf.type.kind === 1 && leaf.type.scalarCode >= 2 &&
                                          leaf.type.scalarCode <= 9);
const flag = nestedLeaves.find(leaf => leaf.type.scalarCode === 1);
check(!!changed && !!flag && floats.length >= 2);
for (let pattern = 0; pattern < 4; ++pattern) {
	const seed = oracle.encodeValue(model, nested.id, sample(nested));
	for (const leaf of floats) {
		const bits = vectors.find(([ code ]) => code === leaf.type.scalarCode)[1][pattern];
		setBits(seed, leaf.offset, leaf.type.scalarCode, bits);
	}
	const value = codec(nested.id).decode(seed);
	const replacement = [ 8, 9 ].includes(changed.type.scalarCode) ? 7n : 7;
	put(value, changed.path, replacement);
	const expected = new Uint8Array(seed);
	expected.set(oracle.encodeValue(model, changed.type.id, replacement), changed.offset);
	equal(codec(nested.id).encodePreserving(value, seed), expected);
	equal(oracle.encodeValuePreserving(model, nested.id, value, seed), expected);
	const metadata = structuralMetadata.types.find(type => type.id === nested.id);
	for (const member of metadata.members)
		equal(member.wireOffset,
		      nested.members.slice(0, metadata.members.indexOf(member))
		          .reduce((sum, prior) => sum + model.types[prior.typeId].wireBytes, 0));
	const bad = new Uint8Array(seed);
	bad[flag.offset] = 2;
	reject(() => codec(nested.id).encodePreserving(value, bad));
	const mutable = new Uint8Array(seed), getterValue = codec(nested.id).decode(seed);
	put(getterValue, changed.path, replacement);
	const first = nested.members[0].name, saved = getterValue[first];
	Object.defineProperty(getterValue, first, {
		enumerable : true,
		get() {
		    mutable.fill(0);
		    return saved;
		}
	});
	equal(codec(nested.id).encodePreserving(getterValue, mutable), expected);
	check(mutable.every(byte => byte === 0));
}

// Generated writable metadata exposes the same opt-in encoder, without a client dependency.
let writable = 0;
for (const endpoint of structuralMetadata.endpoints.filter(row => row.category === 1 &&
                                                                  row.capabilities === 3)) {
	const field = generated[endpoint.groupSymbol][endpoint.symbol];
	const old = baseline[endpoint.groupSymbol][endpoint.symbol];
	equal(field.write.encodePreserving, codec(endpoint.valueType).encodePreserving);
	check(!Object.hasOwn(old.write, 'encodePreserving'));
	const value = sample(model.types[endpoint.valueType]), seed = field.write.encode(value);
	equal(field.write.encodePreserving(value, seed), seed);
	++writable;
}
check(writable > 0);
console.log(`generated web wire editing: ${checks} checks passed`);
