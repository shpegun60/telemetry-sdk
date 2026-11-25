/* Generated web codecs -> native Encoded API and Resource/RPC mailbox.
 * Usage: node WebIntegration.mjs GENERATED_MODULE NATIVE_FIXTURE_DIR BRIDGE [CLIENT]
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawn, spawnSync} from 'node:child_process';
import readline from 'node:readline';
import {pathToFileURL} from 'node:url';
import * as oracle from '../../web/telemetry.js';
import {rpcExchange, rpcInvoke, RpcError} from '../../examples/codegen_web/transport.mjs';

const [modulePath, fixtureDirectory, bridgePath, clientPath] = process.argv.slice(2);
assert.ok(modulePath && fixtureDirectory && bridgePath, 'MODULE FIXTURES BRIDGE required');
const api = await import(pathToFileURL(path.resolve(modulePath)));
const read = name => new Uint8Array(fs.readFileSync(path.join(fixtureDirectory, name + '.bin')));
const model = oracle.parseDescriptor(read('descriptor'));
let checks = 0;
const check = condition => {
	assert.ok(condition);
	++checks;
};
const equal = (left, right) => {
	assert.deepEqual(left, right);
	++checks;
};
const reject = async (fn, predicate = RpcError) => {
	await assert.rejects(fn, predicate);
	++checks;
};
const configField = api.device.Config, inspect = api.device.Inspect;
const config = configField.read.decode(read('config'));
const request = {
	config,
	multiplier : 4
};
check(api.matchesHeader(read('descriptor').subarray(0, 64)));
check(api.fingerprint === model.fingerprint);

for (const [name, typeId] of [
         [ 'config', configField.valueTypeId ], [ 'request', inspect.requestTypeId ],
         [ 'response', inspect.responseTypeId ], [ 'scalars', api.edge.Scalars.valueTypeId ],
         [ 'empty', api.edge.Good.responseTypeId ]]) {
	const codec = api.codecs['Type_' + typeId], bytes = read(name);
	const value = codec.decode(bytes);
	equal(value, oracle.decodeValue(model, typeId, bytes));
	equal(codec.encode(value), bytes);
	equal(oracle.encodeValue(model, typeId, value), bytes);
}
equal(inspect.encode(request), read('request'));
equal(inspect.decodeResponse(read('response')).config, config);
check(config.serial === 0xffffffffffffffffn && config.offset === -0x8000000000000000n);
check(Object.is(config.gain, -0) && config.mode === -2);
const scalars = api.edge.Scalars.read.decode(read('scalars'));
check(scalars.u64 === 0xffffffffffffffffn && scalars.i64 === -0x8000000000000000n);
check(Object.is(scalars.f32, -0) && scalars.f64 === Infinity);

let encodedSequence = 0;

function encoded(operation, id, payload)
{
	const stem = path.join(fixtureDirectory, 'web-integration-' + (++encodedSequence));
	fs.writeFileSync(stem + '.input', payload);
	const process = spawnSync(
	    bridgePath, [ 'encoded', operation, String(id), stem + '.input', stem + '.output' ],
	    {encoding : 'utf8', timeout : 5000});
	assert.equal(process.status, 0, process.stderr);
	const status = process.stdout.trim().split(/\s+/).map(Number);
	check(status.length === 4 && status.every(Number.isInteger));
	const output = new Uint8Array(fs.readFileSync(stem + '.output'));
	check(output.length === status[2]);
	return {status, output};
}

let outcome = encoded('read', configField.id, new Uint8Array(0));
equal(outcome.status, [ 0, 0, read('config').length, 0 ]);
equal(configField.read.decode(outcome.output), config);
outcome = encoded('read', api.edge.Scalars.id, new Uint8Array(0));
equal(outcome.status, [ 0, 0, read('scalars').length, 0 ]);
equal(api.edge.Scalars.read.decode(outcome.output), scalars);
for (const [operation, id, payload, expected] of [
         [ 'write', configField.id, configField.write.encode(config), [ 0, 0, 0, 1 ] ],
         [ 'command', api.device.Apply.id, api.device.Apply.encode(config), [ 0, 0, 0, 1 ] ],
         [ 'command', api.device.Reset.id, api.device.Reset.encode(undefined), [ 0, 0, 0, 1 ] ],
         [ 'command', api.edge.Accept.id, api.edge.Accept.encode({}), [ 0, 1, 0, 1 ] ],
         [ 'service', api.edge.Good.id, api.edge.Good.encode(undefined), [ 0, 0, 0, 1 ] ],
         [ 'service', api.edge.Fail.id, api.edge.Fail.encode(undefined), [ 0, 3, 0, 1 ] ],
         [ 'service', api.edge.Consume.id, api.edge.Consume.encode({}), [ 0, 0, 0, 1 ] ],
         [ 'service', api.device.Ping.id, api.device.Ping.encode(undefined), [ 0, 0, 0, 1 ] ],
         [ 'service', api.device.Offline.id, api.device.Offline.encode(request), [ 9, 0, 0, 0 ] ],
         [ 'write', api.edge.Scalars.id, read('scalars'), [ 0, 2, 0, 0 ] ],
         [ 'read', 0xffffffff, new Uint8Array(0), [ 4, 0, 0, 0 ] ],
         [ 'service', inspect.id, read('request').subarray(1), [ 5, 0, 0, 0 ] ],
]) {
	outcome = encoded(operation, id, payload);
	equal(outcome.status, expected);
	check(outcome.output.length === 0);
}
outcome = encoded('service', inspect.id, inspect.encode(request));
equal(outcome.status, [ 0, 0, read('response').length, 1 ]);
equal(inspect.decodeResponse(outcome.output), inspect.decodeResponse(read('response')));
for (const [multiplier, status] of [[ 0, 1 ], [ 1, 3 ], [ 2, 2 ], [ 3, 4 ]]) {
	outcome = encoded('service', inspect.id, inspect.encode({config, multiplier}));
	equal(outcome.status, [ 0, status, 0, 1 ]);
}

const child = spawn(bridgePath, [ 'resource' ], {stdio : [ 'pipe', 'pipe', 'pipe' ]});
const lines = readline.createInterface({input : child.stdout});
let pending = null, stderr = '', exited = false;
child.stderr.on('data', data => {
	stderr += data.toString();
});
const childClosed = new Promise(resolve => child.on('close', code => {
	exited = true;
	resolve(code);
}));

function failPending(error)
{
	if (pending) {
		clearTimeout(pending.timer);
		pending.reject(error);
		pending = null;
	}
}

child.on('error', failPending);
child.on('close', code => failPending(new Error(`Bridge exited ${code}: ${stderr}`)));
lines.on('line', line => {
	if (!pending)
		return;
	const current = pending;
	pending = null;
	clearTimeout(current.timer);
	if (line.length % 2 || !/^[0-9a-f]*$/i.test(line))
		current.reject(new Error('Invalid Bridge hex reply'));
	else
		current.resolve(new Uint8Array(Buffer.from(line, 'hex')));
});
let continuationWrites = 0, continuationReads = 0;
const transport = packet => new Promise((resolve, reject) => {
	assert.equal(pending, null, 'Serialize mailbox operations');
	if (exited) {
		reject(new Error('Bridge already exited'));
		return;
	}
	const cursor =
	    new DataView(packet.buffer, packet.byteOffset, packet.byteLength).getBigUint64(5, true);
	if (cursor !== 0n && packet[0] === 4)
		++continuationWrites;
	if (cursor !== 0n && packet[0] === 3)
		++continuationReads;
	pending = {
		resolve,
		reject,
		timer : setTimeout(() => failPending(new Error('Bridge packet timeout')), 5000)
	};
	child.stdin.write(Buffer.from(packet).toString('hex') + '\n');
});
let transactionId = 0;
const options = extra => ({transactionId : ++transactionId, ...extra});
try {
	if (clientPath) {
		const example = await import(pathToFileURL(path.resolve(clientPath)));
		const result = await example.run(transport, read('descriptor').subarray(0, 64));
		check(result.config.gain === 1.5 && result.accepted && result.checksum === 263168);
		transactionId = 5; // The fresh-session example uses IDs1..5 explicitly.
		await rpcInvoke(transport, api.device.Reset, undefined, options());
	}
	let result = await rpcInvoke(transport, configField.read, undefined, options());
	equal(result.value, config);
	const changed = {...config, serial : 12345678901234567890n, gain : 1.5, enabled : false};
	result = await rpcInvoke(transport, configField.write, changed, options());
	check(result.endpointStatus === 0 && result.payload.length === 0 && result.value === undefined);
	result = await rpcInvoke(transport, configField.read, undefined, options());
	equal(result.value, Object.assign(Object.create(null), changed));
	await rpcInvoke(transport, api.device.Apply, config, options());
	result = await rpcInvoke(transport, inspect, request, options());
	equal(result.value, inspect.decodeResponse(read('response')));
	check(result.payload.length === read('response').length);
	result = await rpcInvoke(transport, api.edge.Accept, {}, options());
	check(result.accepted && result.endpointStatus === 1 && result.value === undefined);
	await rpcInvoke(transport, api.device.Reset, undefined, options());
	result = await rpcInvoke(transport, api.edge.Good, undefined, options());
	equal(result.value, Object.create(null));
	result = await rpcInvoke(transport, api.edge.Consume, {}, options());
	check(result.value === undefined && result.payload.length === 0);
	result = await rpcInvoke(transport, api.device.Ping, undefined, options());
	check(result.value === undefined && result.payload.length === 0);
	let failedDecodeCalls = 0;
	const fail = {
		...api.edge.Fail,
		decodeResponse : () => {
		    ++failedDecodeCalls;
		    return {};
		}
	};
	await reject(() => rpcInvoke(transport, fail, undefined, options()),
	             error => error instanceof RpcError && error.endpointStatus === 3);
	check(failedDecodeCalls === 0);
	const offline = {
		...api.device.Offline,
		decodeResponse : () => {
		    ++failedDecodeCalls;
		    return {};
		}
	};
	await reject(() => rpcInvoke(transport, offline, request, options()),
	             error => error instanceof RpcError && error.dispatchStatus === 9);
	check(failedDecodeCalls === 0);
	for (const [multiplier, status] of [[ 0, 1 ], [ 1, 3 ], [ 2, 2 ], [ 3, 4 ]])
		await reject(() => rpcInvoke(transport, inspect, {config, multiplier}, options()),
		             error => error instanceof RpcError && error.endpointStatus === status);
	await reject(
	    () => rpcExchange(transport, 'FieldWrite', api.edge.Scalars.id, read('scalars'), options()),
	    error => error instanceof RpcError && error.endpointStatus === 2);
	await reject(() =>
	                 rpcExchange(transport, 'FieldRead', 0xffffffff, new Uint8Array(0), options()),
	             error => error instanceof RpcError && error.dispatchStatus === 4);
	await reject(
	    () => rpcExchange(transport, 'Service', inspect.id, read('request').subarray(1), options()),
	    error => error instanceof RpcError && error.dispatchStatus === 5);
	await reject(() => rpcInvoke(transport, configField.read, undefined, options({fileIndex : 77})),
	             error => error instanceof RpcError && error.resourceStatus === 1);
	await reject(() => rpcInvoke(transport, inspect, request, options({maxPackets : 1})));
	await reject(
	    () => rpcInvoke(transport, configField.read, undefined, options({maxReplyBytes : 32})));
	await reject(
	    () => rpcInvoke(transport, configField.read, undefined, options({fragmentBytes : 23})));
	await reject(() => rpcExchange(transport, 'FieldRead', 0, Uint8Array.of(1), options()));

	// Valid Pending snapshot: zero statuses do not establish completion.
	let injectedPending = false;
	const delayed = async packet => {
		const reply = await transport(packet);
		if (packet[0] !== 3 || injectedPending)
			return reply;
		injectedPending = true;
		const result = reply.slice(0, 44), data = new DataView(result.buffer);
		data.setBigUint64(1, 0n, true);
		result[9] = 1;
		data.setUint16(10, 32, true);
		data.setUint32(12 + 16, 0, true);
		result[12 + 21] = 2;
		result[12 + 22] = 0;
		result[12 + 23] = 0;
		return result;
	};
	result = await rpcInvoke(delayed, configField.read, undefined, options());
	check(injectedPending);
	equal(result.value, config);

	// Corrupt actual native replies at each envelope boundary; all fail closed.
	const cases = [
		[
			4,
			reply => {
		        reply[1] ^= 1;
			}
		],
		[
			4,
			reply => {
		        reply[9] ^= 1;
			}
		],
		[
			4,
			reply => {
		        reply[13] = 2;
			}
		],
		[
			3,
			reply => {
		        reply[12] ^= 1;
			}
		],
		[
			3,
			reply => {
		        reply[12 + 4] ^= 1;
			}
		],
		[
			3,
			reply => {
		        reply[12 + 8] ^= 1;
			}
		],
		[
			3,
			reply => {
		        reply[12 + 12] ^= 1;
			}
		],
		[
			3,
			reply => {
		        reply[12 + 20] = 2;
			}
		],
		[
			3,
			reply => {
		        reply[12 + 21] = 0;
			}
		],
		[
			3,
			reply => {
		        reply[12 + 22] = 255;
			}
		],
		[
			3,
			reply => {
		        reply[12 + 23] = 255;
			}
		],
		[
			3,
			reply => {
		        reply[12 + 24] ^= 1;
			}
		],
		[
			3,
			reply => {
		        reply[12 + 28] ^= 1;
			}
		],
		[
			3,
			reply => {
		        reply[9] = 2;
			}
		],
		[
			3,
			reply => {
		        reply[10] ^= 1;
			}
		],
	];
	for (const [operation, mutate] of cases) {
		let injected = false;
		await reject(() => rpcInvoke(async packet => {
			             const reply = await transport(packet);
			             if (packet[0] === operation && !injected) {
				             injected = true;
				             mutate(reply);
			             }
			             return reply;
		             }, configField.read, undefined, options()));
		check(injected);
	}
	let alteredContinuation = false;
	await reject(() => rpcInvoke(async packet => {
		             const reply = await transport(packet);
		             if (packet[0] === 3 &&
		                 new DataView(packet.buffer, packet.byteOffset).getBigUint64(5, true) !==
		                     0n) {
			             alteredContinuation = true;
			             reply[1] ^= 1;
		             }
		             return reply;
	             }, configField.read, undefined, options()));
	check(alteredContinuation && continuationWrites > 0 && continuationReads > 0);
} finally {
	child.stdin.end();
	const timeout = setTimeout(() => child.kill(), 5000);
	const exit = await childClosed;
	clearTimeout(timeout);
	lines.close();
	assert.equal(exit, 0, stderr);
}
console.log(`generated web native Encoded/RPC: ${checks} checks passed`);
