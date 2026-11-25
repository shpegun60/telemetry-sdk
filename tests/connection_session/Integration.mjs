// Packed SDK Session -> existing EncodedBackend -> unchanged native SDK Model. MIT.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createInterface} from 'node:readline';
import {readFile} from 'node:fs/promises';
import {connectStatic, connectDynamic, EncodedBackend, Status, Batch} from 'telemetry-sdk';
import * as sdk from './generated/js/device.js';

const [executable] = process.argv.slice(2);
const descriptor = new Uint8Array(await readFile(new URL('./descriptor.bin', import.meta.url)));
let checks = 0, packets = 0;
const equal = (actual, expected) => {
	checks++;
	assert.deepEqual(actual, expected);
};
const truth = value => {
	checks++;
	assert(value);
};
const dto = number => ({
	number,
	enabled : true,
	gains : [ 0, 65535, 17 ],
	mode : -123,
	large : 0xffffffffffffffffn,
	negative : -0x8000000000000000n,
	f32 : NaN,
	f64 : -0
});
const outcomes = [];
const capability = kind => {
	const bytes = new Uint8Array(24), view = new DataView(bytes.buffer);
	bytes.set(kind === 'ReadMany' ? [ 84, 82, 77, 49, 1, 32, 1, 1 ]
	                              : [ 84, 66, 77, 49, 1, 32, 1, 1 ]);
	view.setUint16(8, kind === 'ReadMany' ? 130 : 8192, true);
	view.setUint16(10, kind === 'ReadMany' ? 514 : 8192, true);
	view.setUint32(12, 17, true);
	view.setBigUint64(16, new DataView(descriptor.buffer).getBigUint64(16, true), true);
	return bytes;
};
for (const mode of ['static', 'dynamic']) {
	const child = spawn(executable, [], {stdio : [ 'pipe', 'pipe', 'pipe' ]});
	let pending, errors = '', active = 0, peak = 0;
	const exit = new Promise((resolve, reject) => {
		child.once('error', reject);
		child.once('exit', code => {
			if (pending)
				pending.reject(new Error('Native peer ended: ' + code + ' ' + errors));
			resolve(code);
		});
	});
	child.stderr.on('data', bytes => errors += bytes);
	createInterface({input : child.stdout}).on('line', line => {
		assert(pending, 'No unsolicited native response');
		const {resolve} = pending;
		pending = undefined;
		resolve(line);
	});
	const native = command => new Promise((resolve, reject) => {
		assert(!pending, 'Second physical exchange bypassed existing client FIFO');
		pending = {resolve, reject};
		child.stdin.write(command + '\n');
	});
	const backend = EncodedBackend.fromExchange(async packet => {
		active++;
		peak = Math.max(peak, active);
		packets++;
		try {
			return new Uint8Array(
			    Buffer.from(await native(Buffer.from(packet).toString('hex')), 'hex'));
		} finally {
			active--;
		}
	}, {readMany : true, batch : true});
	const profile = {
		async open() {
		    return {
			    peer : 'native-fixture',
			    session : 17,
			    header : descriptor.slice(0, 64),
			    descriptor,
			    readMany : capability('ReadMany'),
			    batch : capability('Batch'),
			    backend,
			    async close() {
			        child.stdin.end();
			        equal(await exit, 0);
			    }
		    };
		}
	};
	const session = mode === 'static'
	                    ? await connectStatic({sdk, transport : profile, timeoutMs : 5000})
	                    : await connectDynamic({transport : profile, timeoutMs : 5000});
	const endpoint = object => mode === 'static' ? object : object.id;
	const F = sdk.control, C = sdk.control, S = sdk.control;
	try {
		const results = [];
		results.push(await session.read(endpoint(F.Config)));
		equal(results[0].value.number, 42);
		equal(results[0].value.large, 0xffffffffffffffffn);
		equal(results[0].value.negative, -0x8000000000000000n);
		truth(Number.isNaN(results[0].value.f32));
		truth(Object.is(results[0].value.f64, -0));
		results.push(await session.write(endpoint(F.Config), dto(1111)));
		equal(results[1].succeeded, true);
		equal(results[1].valueStatus, 'NoValue');
		results.push(await session.read(endpoint(F.Config)));
		equal(results[2].value.number, 1000);
		results.push(await session.command(endpoint(C.Configure), dto(37)));
		equal(results[3].executed, true);
		results.push(await session.service(endpoint(S.Echo), dto(99)));
		equal(results[4].value.number, 99);
		results.push(await session.command(endpoint(C.Accepted), dto(10)));
		equal(results[5].accepted, true);
		equal(results[5].executed, false);
		results.push(await session.service(endpoint(S.Busy), dto(99)));
		equal(results[6].status, Status.Ok);
		equal(results[6].succeeded, false);
		equal(results[6].endpointStatus, 3);
		equal(results[6].valueStatus, 'NoValue');
		results.push(await session.service(endpoint(S.EmptyService), undefined));
		truth(results[7].value !== undefined);
		equal(Object.keys(results[7].value), []);
		equal(results[7].valueStatus, 'Ok');
		results.push(await session.service(endpoint(S.Ping), undefined));
		equal(results[8].value, undefined);
		equal(results[8].valueStatus, 'NoValue');
		results.push(await session.read(endpoint(F.Empty)));
		truth(results[9].value !== undefined);
		equal(Object.keys(results[9].value), []);
		equal(results[9].valueStatus, 'Ok');
		for (const [index, result] of results.entries()) {
			equal(result.requestId, index + 1);
			equal(result.session, 17);
			for (const key of ['status', 'operation', 'session', 'requestId', 'dispatch',
			                   'endpointStatus', 'succeeded', 'accepted', 'executed', 'payload'])
				equal(result[key], result.rawResult[key]);
		}
		equal(peak, 1);
		const state = JSON.parse(await native('state'));
		equal(state.reads, 3);
		equal(state.writes, 1);
		equal(state.commands, 2);
		equal(state.services, 4);
		equal(state.decoy, [ 0, 0, 0, 0 ]);
		// Native round trips keep both signs of quiet/signaling NaNs and signed zero.
		// Seed and edit through the supported Session API, then compare actual read bytes.
		const patterns = [
			[ 0x7f812345, 0x7ff0000000001234n ], [ 0xff812345, 0xfff0000000001234n ],
			[ 0x7fcabcde, 0x7ff8123456789abcn ], [ 0xffcabcde, 0xfff8123456789abcn ],
			[ 0x00000000, 0x0000000000000000n ], [ 0x80000000, 0x8000000000000000n ]
		];
		for (const [index, [ f32, f64 ]] of patterns.entries()) {
			const original = F.Config.write.encode(dto(100 + index));
			const data = new DataView(original.buffer);
			data.setUint32(original.length - 12, f32, true);
			data.setBigUint64(original.length - 8, f64, true);
			const seed = session.writePreserving(endpoint(F.Config), F.Config.read.decode(original),
			                                     original);
			equal(seed.session, 17);
			equal((await seed).succeeded, true);
			const before = await session.read(endpoint(F.Config));
			equal(before.payload, original);
			equal(before.valueStatus, 'Ok');
			const expected = original.slice();
			new DataView(expected.buffer).setUint32(0, 200 + index, true);
			const callerSeed = before.payload.slice(), edited = {...before.value};
			Object.defineProperty(edited, 'number', {
				get() {
				    callerSeed.fill(0);
				    return 200 + index;
				}
			});
			const writing = session.writePreserving(endpoint(F.Config), edited, callerSeed);
			callerSeed.fill(255);
			const applied = await writing;
			equal(applied.succeeded, true);
			equal(applied.valueStatus, 'NoValue');
			equal(applied.requestId, writing.requestId);
			equal(applied.rawResult.status, Status.Ok);
			const after = await session.read(endpoint(F.Config));
			equal(after.payload, expected);
			const actual = new DataView(after.payload.buffer, after.payload.byteOffset,
			                            after.payload.byteLength);
			equal(actual.getUint32(after.payload.length - 12, true), f32);
			equal(actual.getBigUint64(after.payload.length - 8, true), f64);
			equal(after.value.number, 200 + index);
		}
		equal(peak, 1);
		const preserved = JSON.parse(await native('state'));
		equal(preserved.reads, state.reads + 12);
		equal(preserved.writes, state.writes + 12);
		equal(preserved.commands, state.commands);
		equal(preserved.services, state.services);
		const watch = session.readMany(session.capabilities.readMany,
		                               mode === 'static' ? [ F.Counter, F.Bool ]
		                                                 : [ F.Counter.id, F.Bool.id ]);
		const recipe = mode === 'static'
		                   ? Batch.items(Batch.read(F.Counter), Batch.command(C.Accepted, dto(19)),
		                                 Batch.service(S.Busy, dto(20)),
		                                 Batch.service(S.Echo, dto(21)), Batch.read(F.Counter))
		                   : [
			                     {operation : 'FieldRead', id : F.Counter.id},
			                     {operation : 'Command', id : C.Accepted.id, value : dto(19)},
			                     {operation : 'Service', id : S.Busy.id, value : dto(20)},
			                     {operation : 'Service', id : S.Echo.id, value : dto(21)},
			                     {operation : 'FieldRead', id : F.Counter.id}
		                     ];
		const mixed = session.batch(session.capabilities.batch, recipe);
		truth(watch.requestId > 0);
		equal(mixed.requestId, watch.requestId + 1);
		const watchResult = await watch, mixedResult = await mixed;
		equal(watchResult.valueStatus, 'Ok');
		equal(watchResult.items.map(row => row.value), [ preserved.number, true ]);
		equal(itemFacts(mixedResult), [ true, false, 3, 'NoValue', 21, preserved.number ]);
		equal(mixedResult.succeeded, true);
		equal(mixedResult.items[2].succeeded, false);
		const optionalState = JSON.parse(await native('state'));
		equal(optionalState.reads, preserved.reads + 4);
		equal(optionalState.commands, preserved.commands + 1);
		equal(optionalState.services, preserved.services + 2);
		equal(optionalState.writes, preserved.writes);
		equal(peak, 1);
		outcomes.push(results.map(result => ({
			                          status : result.status,
			                          operation : result.operation,
			                          dispatch : result.dispatch,
			                          endpointStatus : result.endpointStatus,
			                          succeeded : result.succeeded,
			                          accepted : result.accepted,
			                          executed : result.executed,
			                          valueStatus : result.valueStatus,
			                          payload : Buffer.from(result.payload).toString('hex')
		                          })));
	} finally {
		await session.close();
	}
}

function itemFacts(result)
{
	return [
		result.items[1].accepted, result.items[1].executed, result.items[2].endpointStatus,
		result.items[2].valueStatus, result.items[3].value.number, result.items[4].value
	];
}

equal(outcomes[0], outcomes[1]);
console.log('connection-session-native checks=' + checks + ' packets=' + packets + ' failures=0');
