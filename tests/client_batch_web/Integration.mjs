// Generated user API against the actual persistent C++ Model/Encoded dispatcher. MIT.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {readFile} from 'node:fs/promises';
import {createInterface} from 'node:readline';
import {pathToFileURL} from 'node:url';

const [runtimePath, sdkPath, descriptorPath, capabilityPath, executable] = process.argv.slice(2);
const {DeviceClient, EncodedBackend, Status, Batch, BatchValueStatus : V} =
    await import(pathToFileURL(runtimePath).href);
const {control, matchesHeader} = await import(pathToFileURL(sdkPath).href);
const header = (await readFile(descriptorPath)).subarray(0, 64);
const capability = await readFile(capabilityPath);
const F = control.fields, C = control.commands, S = control.services;
let checks = 0, packets = 0;
const equal = (a, b) => {
	++checks;
	assert.deepEqual(a, b);
};
const truth = a => {
	++checks;
	assert(a);
};
const data = number => ({
	number,
	enabled : true,
	gains : [ 0, 65535, 17 ],
	mode : -123,
	large : 0xffffffffffffffffn,
	negative : -0x8000000000000000n,
	f32 : NaN,
	f64 : -0
});
const exact = (value, expected) => {
	equal(value.number, expected);
	equal(value.enabled, true);
	equal(value.gains, [ 0, 65535, 17 ]);
	equal(value.mode, -123);
	equal(value.large, 0xffffffffffffffffn);
	equal(value.negative, -0x8000000000000000n);
	truth(Number.isNaN(value.f32));
	truth(Object.is(value.f64, -0));
};
const child = spawn(executable, [], {stdio : [ 'pipe', 'pipe', 'pipe' ]});
let pending, stderr = '';
child.stderr.on('data', bytes => stderr += bytes);
createInterface({input : child.stdout}).on('line', line => {
	assert(pending, 'Unexpected native reply');
	const resolve = pending;
	pending = undefined;
	resolve(line);
});
const exited = new Promise((resolve, reject) => {
	child.once('error', reject);
	child.once('exit', code => {
		if (pending)
			reject(Error(`Native peer exited ${code}: ${stderr}`));
		else
			resolve(code);
	});
});
const native = command => new Promise(resolve => {
	assert(!pending, 'A second physical exchange bypassed the FIFO');
	pending = resolve;
	child.stdin.write(command + '\n');
});
const opcodes = [], sent = [], replies = [];
let mutate;
const backend = EncodedBackend.fromExchange(async packet => {
	++packets;
	opcodes.push(packet[6]);
	sent.push(packet.slice());
	const reply =
	    Uint8Array.from(Buffer.from(await native(Buffer.from(packet).toString('hex')), 'hex'));
	replies.push(reply.slice());
	return mutate ? mutate(reply) : reply;
}, {batch : true});
const client = new DeviceClient({backend, matchesHeader, timeoutMs : 5000, maxPending : 16});
const reset = async () => {
	equal(JSON.parse(await native('reset')), {});
};
const state = async () => JSON.parse(await native('state'));
const requestItems = bytes => {
	const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	const items = [];
	for (let i = 0, cursor = 34; i < dv.getUint16(32, true); ++i) {
		const length = dv.getUint16(cursor + 6, true);
		items.push([ dv.getUint32(cursor, true), bytes[cursor + 4], length ]);
		cursor += 8 + length;
	}
	return items;
};
try {
	truth(client.openSession(17, header));
	// Decoding into Number may quiet NaNs; raw payload is the bit-exact escape hatch.
	const initial = await client.batch(capability, Batch.items(Batch.read(F.Config)));
	equal(initial.valueStatus, V.Ok);
	exact(initial.items[0].value, 42);
	const raw = initial.items[0].payload;
	equal(new DataView(raw.buffer, raw.byteOffset).getUint32(29, true), 0xff812345);
	equal(new DataView(raw.buffer, raw.byteOffset).getBigUint64(33, true), 0x8000000000000000n);

	await reset();
	const written = data(1111), configured = data(37), echoed = data(99);
	// The entire ordinary usage path uses generated endpoints and typed DTOs, not bytes.
	const promise =
	    client.batch(capability, Batch.items(Batch.write(F.Config, written), Batch.read(F.Config),
	                                         Batch.command(C.Configure, configured),
	                                         Batch.read(F.Counter), Batch.service(S.Echo, echoed),
	                                         Batch.command(C.Accepted, configured),
	                                         Batch.service(S.Busy, echoed), Batch.read(F.Config)));
	written.number = 1;
	configured.number = 2;
	echoed.number = 3;
	const result = await promise;
	truth(result.succeeded);
	equal(result.status, Status.Ok);
	equal(result.value, undefined);
	equal(result.valueStatus, V.Ok);
	equal(result.items.length, 8);
	equal(result.items.map(i => i.id), [ 65536, 65536, 65536, 65537, 65536, 65538, 65539, 65536 ]);
	equal(result.items.map(i => i.operation), [
		'FieldWrite', 'FieldRead', 'Command', 'FieldRead', 'Service', 'Command', 'Service',
		'FieldRead'
	]);
	equal(result.items.map(i => i.valueStatus),
	      [ V.NoValue, V.Ok, V.NoValue, V.Ok, V.Ok, V.NoValue, V.NoValue, V.Ok ]);
	exact(result.items[1].value, 1000);
	equal(result.items[2].executed, true);
	equal(result.items[3].value, 37 ^ 0x55);
	exact(result.items[4].value, 99);
	equal(result.items[5].accepted, true);
	equal(result.items[5].executed, false);
	equal(result.items[6].endpointStatus, 3);
	equal(result.items[6].accepted, false);
	equal(result.items[6].succeeded, false);
	exact(result.items[7].value, 37 ^ 0x55);
	equal(requestItems(sent.at(-1)), [
		[ 65536, 1, 41 ], [ 65536, 4, 0 ], [ 65536, 2, 41 ], [ 65537, 4, 0 ], [ 65536, 3, 41 ],
		[ 65538, 2, 41 ], [ 65539, 3, 41 ], [ 65536, 4, 0 ]
	]);
	const actual = await state();
	equal(actual.trace, [
		[ 2, 1, 0, 1111 ], [ 1, 1, 0, 1000 ], [ 3, 1, 0, 37 ], [ 1, 1, 1, 112 ], [ 4, 1, 0, 99 ],
		[ 3, 1, 2, 37 ], [ 4, 1, 3, 99 ], [ 1, 1, 0, 112 ]
	]);
	equal(actual.decoy, [ 0, 0, 0, 0 ]);
	equal([ actual.reads, actual.writes, actual.commands, actual.services ], [ 3, 1, 2, 2 ]);

	await reset();
	const zero = await client.batch(
	    capability, Batch.items(Batch.command(C.Reset), Batch.command(C.EmptyArgument, {}),
	                            Batch.service(S.Ping), Batch.service(S.EmptyService),
	                            Batch.service(S.Consume, data(23)),
	                            Batch.service(S.EmptyRequest, {}), Batch.read(F.Empty)));
	equal(zero.valueStatus, V.Ok);
	equal(zero.items.map(i => i.valueStatus),
	      [ V.NoValue, V.NoValue, V.NoValue, V.Ok, V.NoValue, V.Ok, V.Ok ]);
	truth(zero.items[0].executed && zero.items[1].executed);
	equal(Object.keys(zero.items[3].value), []);
	equal(zero.items[3].payload.length, 0);
	equal(Object.keys(zero.items[6].value), []);
	exact(zero.items[5].value, 0);

	await reset();
	const errors = await client.batch(
	    capability, Batch.items(Batch.command(C.Reject, data(9)), Batch.read(F.Unavailable),
	                            Batch.service(S.Busy, data(10)), Batch.read(F.Counter)));
	truth(errors.succeeded);
	equal(errors.items[0].endpointStatus, 5);
	equal(errors.items[0].succeeded, false);
	truth(!errors.items[1].succeeded);
	equal(errors.items[2].endpointStatus, 3);
	truth(!errors.items[2].accepted);
	equal(errors.items[3].value, 42);
	equal((await state()).trace, [ [ 3, 1, 4, 9 ], [ 4, 1, 3, 10 ], [ 1, 1, 1, 42 ] ]);

	// Even a faulty handwritten encoder cannot execute an earlier item before server preflight.
	await reset();
	const invalidWrite = {
		...F.Config.write,
		encode : value => {
		    const bytes = F.Config.write.encode(value);
		    bytes[4] = 2;
		    return bytes;
		}
	};
	const refused = await client.batch(
	    capability, Batch.items(Batch.command(C.Reset), Batch.write(invalidWrite, data(9))));
	truth(!refused.succeeded);
	equal(refused.items, undefined);
	equal((await state()).trace, []);
	truth(!client.recoveryRequired);

	// Native getter sees the mutation only in FIFO order across batch boundaries.
	await reset();
	const queued = [
		client.read(F.Counter),
		client.batch(capability,
		             Batch.items(Batch.write(F.Config, data(1111)), Batch.read(F.Counter))),
		client.read(F.Counter)
	];
	const ordered = await Promise.all(queued);
	equal(ordered[0].value, 42);
	equal(ordered[1].items[1].value, 1000);
	equal(ordered[2].value, 1000);
	equal(opcodes.slice(-3), [ 4, 6, 4 ]);
	equal((await state()).trace,
	      [ [ 1, 1, 1, 42 ], [ 2, 1, 0, 1111 ], [ 1, 1, 1, 1000 ], [ 1, 1, 1, 1000 ] ]);

	for (const count of [1, 8, 16, 32]) {
		await reset();
		const response = await client.batch(
		    capability, Batch.items(...Array.from({length : count}, () => Batch.read(F.Counter))));
		equal(response.items.length, count);
		equal(response.items.map(i => i.value), Array(count).fill(42));
		equal((await state()).reads, count);
	}

	// A real successfully executed Write retains its outcome when the last DTO is corrupt.
	await reset();
	mutate = reply => {
		reply[reply.length - 1] = 2;
		return reply;
	};
	const bad =
	    await client.batch(capability, Batch.items(Batch.write(F.Config, data(77)),
	                                               Batch.read(F.Config), Batch.service(S.Flag)));
	mutate = undefined;
	equal(bad.status, Status.Ok);
	equal(bad.valueStatus, V.InvalidPayload);
	equal(bad.items.length, 3);
	truth(bad.items[0].succeeded);
	truth(bad.items.every(i => i.value === undefined));
	equal(bad.items[1].valueStatus, V.InvalidPayload);
	equal((await state()).writes, 1);
	truth(!client.recoveryRequired);
	// Retained response storage owns bytes, independent of later exchanges and user payload edits.
	equal(initial.items[0].value.number, 42);
	equal(result.items[1].value.number, 1000);
	result.items[1].payload.fill(0);
	equal(result.items[7].value.number, 112);
	equal(client.pendingCount, 0);
	client.close();
	truth(client.drained);
} finally {
	child.stdin.end();
	equal(await exited, 0);
	equal(stderr, '');
}
console.log(`Batch integration checks=${checks} packets=${packets} failures=0`);
