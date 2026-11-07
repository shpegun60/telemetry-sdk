// Deterministic optional ReadMany admission, validation, ownership and lifecycle. MIT.
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
import {FakeClock, flush, u32Bytes, u32Value, bindings, invoke} from '../client_web/Trace.mjs';

const R = await import(pathToFileURL(process.argv[2]).href);
const {DeviceClient, EncodedBackend, RetryingEncodedBackend, RpcBackend, Status, ReadManyStatus} =
    R;
let checks = 0;
const equal = (a, b) => {
	++checks;
	assert.deepEqual(a, b);
};
const truth = a => {
	++checks;
	assert(a);
};
const header = new Uint8Array(64);
const capability = (session = 1, fingerprint = 0n) => {
	const bytes = new Uint8Array(24), data = new DataView(bytes.buffer);
	data.setUint32(0, 0x314d5254, true);
	bytes.set([ 1, 32, 1, 1 ], 4);
	data.setUint16(8, 130, true);
	data.setUint16(10, 514, true);
	data.setUint32(12, session, true);
	data.setBigUint64(16, fingerprint, true);
	return bytes;
};
const field = (id = 7, scalar = 6, decode = u32Value) => ({
	fingerprint : 0n,
	operation : 'FieldRead',
	id,
	requestBytes : 0,
	responseBytes : [ 0, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8 ][scalar],
	readManyScalar : scalar,
	decode
});
// Planning uses the same SDK parser as admission, including offset byte views.
{
	const source = capability(17, 0xfedcba9876543210n);
	const copy = new Uint8Array(31);
	copy.set(source, 3);
	const info = R.inspectReadManyCapability(copy.subarray(3, 27));
	equal(info, {
		itemLimit : 32,
		requestLimit : 130,
		replyLimit : 514,
		session : 17,
		fingerprint : 0xfedcba9876543210n
	});
	truth(Object.isFrozen(info));
	source.fill(0);
	equal(info.session, 17);
	for (const bad of [new Uint8Array(23), new Uint8Array(25), null, {}, 1])
		equal(R.inspectReadManyCapability(bad), undefined);
	for (const [offset, value] of [[ 0, 0 ], [ 4, 0 ], [ 5, 0 ], [ 5, 33 ], [ 6, 0 ], [ 7, 0 ],
	                               [ 8, 5 ], [ 8, 131 ], [ 10, 9 ], [ 12, 0 ]]) {
		const bad = capability();
		bad[offset] = value;
		equal(R.inspectReadManyCapability(bad), undefined);
	}
	const small = capability();
	small[5] = 7;
	new DataView(small.buffer).setUint16(8, 14, true);
	new DataView(small.buffer).setUint16(10, 34, true);
	equal(R.inspectReadManyCapability(small),
	      {itemLimit : 7, requestLimit : 14, replyLimit : 34, session : 1, fingerprint : 0n});
}
const reply = items => {
	const output = new Uint8Array(2 + items.reduce((size, i) => size + 8 + (i[2]?.length ?? 0), 0));
	const data = new DataView(output.buffer);
	data.setUint16(0, items.length, true);
	let cursor = 2;
	for (const [id, status, payload = new Uint8Array(0)] of items) {
		data.setUint32(cursor, id, true);
		output[cursor + 4] = status;
		data.setUint16(cursor + 6, payload.length, true);
		output.set(payload, cursor + 8);
		cursor += 8 + payload.length;
	}
	return output;
};
const frameReply =
    (sent, body,
     echo = new DataView(sent.buffer, sent.byteOffset, sent.byteLength).getUint32(24, true)) => {
	    const frame = new Uint8Array(32 + body.length);
	    frame.set(sent.subarray(0, 32));
	    frame[5] = 1;
	    const data = new DataView(frame.buffer);
	    data.setUint32(20, body.length, true);
	    data.setUint32(24, echo, true);
	    frame.set(body, 32);
	    return frame;
    };

function fixture(options = {})
{
	const clock = new FakeClock(), started = [];
	let supported = true;
	const backend = {
		supports : () => supported,
		execute : (request, context) =>
		    new Promise(resolve => started.push({request, context, resolve})),
	};
	const client = new DeviceClient(
	    {backend, clock, matchesHeader : () => true, timeoutMs : 100, maxPending : 4, ...options});
	truth(client.openSession(1, header));
	return {client, clock, backend, started, disable : () => supported = false};
}

const bound = (f, fields = [ field() ]) => {
	const result = f.client.bindReadMany(capability(), fields);
	equal(result.status, Status.Ok);
	truth(Object.isFrozen(result.binding));
	return result.binding;
};
const complete = async (f, payload, prepare = true) => {
	await flush();
	const record = f.started.at(-1);
	truth(record);
	if (prepare)
		truth(record.context.handoff(true));
	record.resolve({dispatch : 0, endpointStatus : 0, payload});
	await flush();
};

equal(Status.Unsupported, 18);
equal(ReadManyStatus.Unavailable, 3);
{
	const f = fixture(), fields = [ field() ];
	let iterators = 0;
	fields[Symbol.iterator] = function*() {
		++iterators;
	};
	const b = bound(f, fields);
	equal(iterators, 0);
	equal(b.count, 1);
	const p = f.client.readMany(b);
	await complete(f, reply([ [ 7, 0, u32Bytes(42) ] ]));
	equal((await p).value[0].value, 42);
}
{
	const f = fixture(), fields = [ field() ];
	const initial = fields[0];
	Object.defineProperty(fields, '0', {
		get() {
		    fields.length = 1000000;
		    return initial;
		}
	});
	const b = bound(f, fields);
	equal(b.count, 1);
	equal(f.client.pendingCount, 0);
}
{
	const f = fixture(), fields = [ field(), field(8) ], initial = fields[0];
	Object.defineProperty(fields, '0', {
		get() {
		    fields.length = 1;
		    return initial;
		}
	});
	equal(f.client.bindReadMany(capability(), fields).status, Status.InvalidArgument);
	equal(f.client.pendingCount, 0);
}
for (const length of [0.5, '1', Infinity, NaN]) {
	const f = fixture();
	const fields = new Proxy([ field() ], {
		get(target, key) {
		    return key === 'length' ? length : Reflect.get(target, key);
		}
	});
	equal(f.client.bindReadMany(capability(), fields).status, Status.InvalidArgument);
	equal(f.started.length, 0);
}
{
	const f = fixture();
	f.backend.supports = () => Promise.reject(Error('invalid async support hook'));
	equal(f.client.bindReadMany(capability(), [ field() ]).status, Status.Unsupported);
	await new Promise(resolve => setImmediate(resolve));
	equal(f.started.length, 0);
}
{
	const f = fixture(), fields = [ field(7), field(8) ];
	const b = bound(f, fields);
	fields[0].id = 99;
	fields[0].decode = () => {
		throw Error('mutable source codec');
	};
	fields.reverse();
	const p = f.client.readMany(b);
	await flush();
	equal(f.started[0].request.operation, 'ReadMany');
	equal(f.started[0].request.endpointId, 0);
	equal(f.started[0].request.responseBytes, 26);
	equal([...f.started[0].request.payload ], [ 2, 0, 7, 0, 0, 0, 8, 0, 0, 0 ]);
	const body = reply([ [ 7, 0, u32Bytes(42) ], [ 8, 3 ] ]);
	await complete(f, body);
	const result = await p;
	equal(result.status, Status.Ok);
	truth(result.succeeded);
	equal(result.value.map(i => i.status), [ 0, 3 ]);
	equal(result.value[0].value, 42);
	equal(result.value[1].value, undefined);
	truth(Object.isFrozen(result.value));
	body.fill(255);
	equal(u32Value(result.value[0].payload), 42);
	equal(result.payload[0], 2);
	result.value[0].payload.fill(0);
	equal(u32Value(result.payload.subarray(10, 14)), 42);
	equal(f.client.pendingCount, 0);
	equal(f.client.snapshotBytes, 0);
}
// Default/legacy backends do not silently enable an optional wire operation.
for (const backend of [new EncodedBackend(() => {
                                          }),
                       new RetryingEncodedBackend(() => {
                                                  }),
                       new RpcBackend(() => {
	                       throw Error('no Resource WRITE');
                       }),
                       {
	                       execute : () => {
	                           throw Error('legacy execute');
	                       }
                       }]) {
	const f = fixture({backend});
	equal(f.client.bindReadMany(capability(), [ field() ]).status, Status.Unsupported);
	equal(f.client.pendingCount, 0);
	f.client.close();
	equal(f.client.bindReadMany(new Uint8Array(), []).status, Status.Closed);
}
for (const mutate of [b => b.subarray(1),
                     b => {
	                     b[0] = 0;
	                     return b;
                     },
                     b => {
	                     b[4] = 2;
	                     return b;
                     },
                     b => {
	                     b[6] = 0;
	                     return b;
                     },
                     b => {
	                     b[7] = 0;
	                     return b;
                     },
                     b => {
	                     b[5] = 0;
	                     return b;
                     },
                     b => {
	                     b[5] = 33;
	                     return b;
                     },
                     b => {
	                     b[8] = 5;
	                     return b;
                     },
                     b => {
	                     b[8] = 131;
	                     return b;
                     },
                     b => {
	                     b[10] = 9;
	                     b[11] = 0;
	                     return b;
                     },
                     b => {
	                     b[10] = 3;
	                     return b;
                     },
                     b => {
	                     b[12] = 0;
	                     return b;
                     }]) {
	const f = fixture();
	equal(f.client.bindReadMany(mutate(capability()), [ field() ]).status, Status.InvalidArgument);
	equal(f.started.length, 0);
}
{
	const f = fixture();
	equal(f.client.bindReadMany(capability(2), [ field() ]).status, Status.NotAdmitted);
	equal(f.client.bindReadMany(capability(1, 3n), [ field() ]).status, Status.NotAdmitted);
	for (const selected
	         of [[], Array.from({length : 33}, (_, i) => field(i)), [ field(), field() ]])
		equal(f.client.bindReadMany(capability(), selected).status, Status.InvalidArgument);
	for (const scalar of [undefined, 0, 12, 2.5])
		equal(f.client.bindReadMany(capability(), [ {...field(), readManyScalar : scalar} ]).status,
		      Status.Unsupported);
	equal(f.client.bindReadMany(capability(), [ {...field(), responseBytes : 2} ]).status,
	      Status.Unsupported);
	equal(f.client.bindReadMany(capability(), [ {...field(), fingerprint : 5n} ]).status,
	      Status.NotAdmitted);
	const cap = capability();
	new DataView(cap.buffer).setUint16(10, 10, true);
	equal(f.client.bindReadMany(cap, [ field() ]).status, Status.BufferTooSmall);
	cap.set(capability());
	new DataView(cap.buffer).setUint16(8, 6, true);
	equal(f.client.bindReadMany(cap, [ field(1), field(2) ]).status, Status.BufferTooSmall);
	equal((await f.client.readMany({session : 1, fingerprint : 0n, count : 1})).status,
	      Status.NotAdmitted);
	const b = bound(f);
	const other = fixture();
	equal((await other.client.readMany(b)).status, Status.NotAdmitted);
	f.client.resetSession();
	equal((await f.client.readMany(b)).status, Status.NotAdmitted);
	truth(f.client.confirmSessionClosed(1));
	truth(f.client.openSession(2, header));
	equal((await f.client.readMany(b)).status, Status.NotAdmitted);
}
// Every body failure occurs before the first decoder, including a malformed last scalar.
for (const mutate of [b => b.subarray(0, 1),
                     b => {
	                     b[0] = 1;
	                     return b;
                     },
                     b => {
	                     b[2] = 99;
	                     return b;
                     },
                     b => {
	                     b[6] = 5;
	                     return b;
                     },
                     b => {
	                     b[7] = 1;
	                     return b;
                     },
                     b => {
	                     b[8] = 2;
	                     return b;
                     },
                     b => {
	                     b[6] = 3;
	                     return b;
                     },
                     b => {
	                     b[b.length - 1] = 2;
	                     return b;
                     },
                     b => b.subarray(0, b.length - 1),
                     b => {
	                     const out = new Uint8Array(b.length + 1);
	                     out.set(b);
	                     return out;
                     }]) {
	const f = fixture();
	let decodes = 0;
	const b = bound(f, [
		field(7, 6,
		      x => {
		          ++decodes;
		          return u32Value(x);
		      }),
		field(8, 1,
		      x => {
		          ++decodes;
		          return x[0] !== 0;
		      })
	]);
	const p = f.client.readMany(b);
	const body = mutate(reply([ [ 7, 0, u32Bytes(42) ], [ 8, 0, new Uint8Array([ 1 ]) ] ]));
	const envelopeFailure = body.length < 2 || body.length > 23;
	await complete(f, body);
	equal((await p).status, envelopeFailure ? Status.MalformedReply : Status.CodecError);
	equal(decodes, 0);
	equal(f.client.recoveryRequired, envelopeFailure);
	equal(f.client.bindReadMany(capability(), [ field() ]).status,
	      envelopeFailure ? Status.RecoveryRequired : Status.Ok);
	f.client.close();
	equal((await f.client.readMany(b)).status, Status.Closed);
}
for (const status of [1, 2, 3, 4]) {
	const f = fixture(), b = bound(f), p = f.client.readMany(b);
	await complete(f, reply([ [ 7, status ] ]));
	const result = await p;
	equal(result.status, Status.Ok);
	equal(result.value[0].status, status);
	equal(result.value[0].value, undefined);
	truth(!f.client.recoveryRequired);
}
// Deterministic queued/offered/prepared boundaries retain the existing recovery contract.
for (const phase of ['queued', 'offered', 'prepared']) {
	const f = fixture(), b = bound(f), p = f.client.readMany(b);
	let completed = 0;
	p.then(() => ++completed);
	if (phase !== 'queued')
		await flush();
	if (phase === 'prepared')
		truth(f.started[0].context.handoff(true));
	f.clock.advance(100);
	await flush();
	equal((await p).status, Status.Timeout);
	equal(f.client.recoveryRequired, phase === 'prepared');
	if (phase === 'prepared')
		equal((await f.client.readMany(b)).status, Status.RecoveryRequired);
	equal(completed, 1);
	if (f.started.length) {
		f.started[0].resolve(
		    {dispatch : 0, endpointStatus : 0, payload : reply([ [ 7, 0, u32Bytes(99) ] ])});
		await flush();
		equal(completed, 1);
	}
}
{
	const f = fixture(), b = bound(f), p = f.client.readMany(b);
	await flush();
	truth(f.started[0].context.handoff(true));
	truth(p.cancel());
	equal((await p).status, Status.Cancelled);
	truth(f.client.recoveryRequired);
	equal(p.cancel(), false);
}
{
	const f = fixture();
	const b = bound(f, [
		field(7, 6,
		      () => {
		          throw Error('decode');
		      }),
		field(8)
	]);
	const p = f.client.readMany(b);
	await complete(f, reply([ [ 7, 0, u32Bytes(1) ], [ 8, 0, u32Bytes(2) ] ]));
	const result = await p;
	equal(result.status, Status.CodecError);
	equal(result.value, undefined);
	equal(result.dispatch, 0);
	truth(!f.client.recoveryRequired);
}
for (const action of ['close', 'reset', 'readmit']) {
	const f = fixture();
	let later = 0;
	let body;
	const b = bound(f, [
		field(7, 6,
		      () => {
		          body.fill(255); // User codec can reuse a custom backend's original bytes.
		          if (action === 'close')
			          f.client.close();
		          else {
			          f.client.resetSession();
			          if (action === 'readmit') {
				          truth(f.client.confirmSessionClosed(1));
				          truth(f.client.openSession(2, header));
			          }
		          }
		          return 42;
		      }),
		field(8, 6, () => ++later)
	]);
	const p = f.client.readMany(b);
	let completions = 0;
	p.then(() => ++completions);
	body = reply([ [ 7, 0, u32Bytes(1) ], [ 8, 0, u32Bytes(2) ] ]);
	const original = body.slice();
	await complete(f, body);
	const result = await p;
	equal(result.status, Status.CodecError);
	equal(result.value, undefined);
	equal(result.payload, original);
	equal(result.dispatch, 0);
	equal(result.endpointStatus, 0);
	equal(result.session, 1);
	equal(later, 0);
	equal(completions, 1);
	equal(f.client.pendingCount, 0);
	truth(!f.client.recoveryRequired);
	equal(f.client.ready, action === 'readmit');
	if (action === 'close')
		truth(f.client.drained);
}
{
	const f = fixture();
	let body;
	const b = bound(f, [
		field(7, 6,
		      input => {
		          body.fill(255);
		          return u32Value(input);
		      }),
		field(8)
	]);
	const p = f.client.readMany(b);
	body = reply([ [ 7, 0, u32Bytes(1) ], [ 8, 0, u32Bytes(2) ] ]);
	const original = body.slice();
	await complete(f, body);
	const result = await p;
	equal(result.status, Status.Ok);
	equal(result.value.map(item => item.value), [ 1, 2 ]);
	equal(result.payload, original);
}
{
	const f = fixture(), b = bound(f);
	const p = f.client.readMany(b);
	f.disable();
	await flush();
	equal((await p).status, Status.Unsupported);
	equal(f.started.length, 0);
}
{
	const f = fixture({maxPending : 1}), b = bound(f), first = f.client.readMany(b);
	const rejected = f.client.readMany(b);
	equal(rejected.requestId, 0);
	equal((await rejected).status, Status.QueueFull);
	equal(f.client.pendingCount, 1);
	await complete(f, reply([ [ 7, 0, u32Bytes(5) ] ]));
	equal((await first).value[0].value, 5);
	const next = f.client.readMany(b);
	await complete(f, reply([ [ 7, 0, u32Bytes(6) ] ]));
	equal((await next).value[0].value, 6);
}
{
	const f = fixture(), read = field();
	Object.defineProperty(read, 'readManyScalar', {
		get() {
		    f.client.resetSession();
		    return 6;
		}
	});
	equal(f.client.bindReadMany(capability(), [ read ]).status, Status.NotAdmitted);
	equal(f.client.pendingCount, 0);
	equal(f.started.length, 0);
}
{
	const f = fixture(), b = bound(f), p = f.client.readMany(b);
	await flush();
	f.disable();
	equal(f.started[0].context.handoff(true), false);
	truth(!f.client.recoveryRequired);
	truth(p.cancel());
	equal((await p).status, Status.Cancelled);
}
{
	let calls = 0;
	try {
		await R.rpcExchange(() => {
			++calls;
		}, 'ReadMany', 0, new Uint8Array(6), {transactionId : 1});
		assert.fail('RPC admitted optional operation');
	} catch (error) {
		equal(error.clientStatus, Status.Unsupported);
	}
	equal(calls, 0);
}
// Actual Encoded header correlation keeps the exact budget echo, not the shorter actual body.
// A clock callback cannot migrate a submission into a newly admitted session.
for (const operation of [1, 2, 3, 4, 5]) {
	const f = fixture(), b = operation === 5 ? bound(f) : undefined;
	let armed = true;
	f.clock.now = () => {
		if (armed) {
			armed = false;
			f.client.resetSession();
			truth(f.client.confirmSessionClosed(1));
			truth(f.client.openSession(2, header));
		}
		return 0;
	};
	const p = operation === 5 ? f.client.readMany(b) : invoke(f.client, bindings()[operation], 42);
	equal(p.requestId, 0);
	equal((await p).status, Status.NotAdmitted);
	await flush();
	equal(f.started.length, 0);
	equal(f.client.pendingCount, 0);
	truth(f.client.ready);
}
// A clock callback at the handoff boundary can close; permission must be revoked immediately.
for (const operation of [1, 2, 3, 4, 5]) {
	const f = fixture(), b = operation === 5 ? bound(f) : undefined;
	const p = operation === 5 ? f.client.readMany(b) : invoke(f.client, bindings()[operation], 42);
	await flush();
	let armed = true, completions = 0;
	p.then(() => ++completions);
	f.clock.now = () => {
		if (armed) {
			armed = false;
			f.client.close();
		}
		return 0;
	};
	equal(f.started[0].context.handoff(true), false);
	equal(f.started[0].context.signal.aborted, true);
	equal((await p).status, Status.Cancelled);
	truth(!f.client.recoveryRequired);
	f.started[0].resolve({dispatch : 0, endpointStatus : 0, payload : new Uint8Array(0)});
	await flush();
	equal(completions, 1);
	equal(f.started.length, 1);
	truth(f.client.drained);
}
{
	const f = fixture();
	let armed = false, pending;
	f.backend.supports = () => {
		if (armed) {
			armed = false;
			truth(pending.cancel());
		}
		return true;
	};
	const b = bound(f);
	pending = f.client.readMany(b);
	armed = true;
	await flush();
	equal((await pending).status, Status.Cancelled);
	equal(f.started.length, 0);
	const next = f.client.read(field(9));
	await flush();
	equal(f.started.length, 1);
	equal(f.started[0].request.operation, 'FieldRead');
	await complete(f, u32Bytes(99));
	equal((await next).value, 99);
	equal(f.client.pendingCount, 0);
	truth(!f.client.recoveryRequired);
}
for (const echo of [14, 13]) {
	const clock = new FakeClock();
	let sent;
	const backend = new EncodedBackend(packet => sent = packet.slice(), {readMany : true});
	const f = fixture({backend, clock});
	const b = bound(f), p = f.client.readMany(b);
	await flush();
	const frame = frameReply(sent, reply([ [ 7, 3 ] ]), echo);
	equal(backend.receive(frame), echo === 14);
	await flush();
	equal((await p).status, echo === 14 ? Status.Ok : Status.MalformedReply);
	equal(f.client.recoveryRequired, echo !== 14);
}
// The selected retry policy repeats one immutable identity, not a new logical request.
{
	const clock = new FakeClock(), sent = [];
	const backend =
	    new RetryingEncodedBackend(packet => sent.push(packet.slice()),
	                               {readMany : true, replyTimeoutMs : 10, maxRetries : 1});
	const f = fixture({backend, clock}), b = bound(f), p = f.client.readMany(b);
	await flush();
	equal(sent.length, 1);
	clock.advance(10);
	await flush();
	equal(sent.length, 2);
	equal(sent[0], sent[1]);
	truth(backend.receive(frameReply(sent[1], reply([ [ 7, 0, u32Bytes(55) ] ]))));
	await flush();
	equal((await p).value[0].value, 55);
	clock.advance(500);
	await flush();
	equal(sent.length, 2);
	truth(!f.client.recoveryRequired);
}
// A checked result does not release the physical lease or allow a second send.
{
	const clock = new FakeClock(), sent = [];
	let release;
	const backend = new EncodedBackend(packet => {
		sent.push(packet.slice());
		if (sent.length === 1)
			return new Promise(resolve => release = resolve);
	}, {readMany : true});
	const f = fixture({backend, clock}), b = bound(f);
	const first = f.client.readMany(b), second = f.client.readMany(b, 200);
	await flush();
	truth(backend.receive(frameReply(sent[0], reply([ [ 7, 0, u32Bytes(1) ] ]))));
	await flush();
	equal((await first).value[0].value, 1);
	equal(sent.length, 1);
	clock.advance(110);
	await flush();
	equal(sent.length, 1);
	release();
	await flush();
	equal(sent.length, 2);
	truth(backend.receive(frameReply(sent[1], reply([ [ 7, 0, u32Bytes(2) ] ]))));
	await flush();
	equal((await second).value[0].value, 2);
	truth(!f.client.recoveryRequired);
}
console.log(`ReadMany web checks=${checks} failures=0`);
