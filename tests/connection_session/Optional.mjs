// Public Session optional operations use the same real client and controlled owner clock. MIT.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {createHook} from 'node:async_hooks';
import {connectStatic, connectDynamic, Batch, Status} from 'telemetry-sdk';
import {FakeClock, flush, u32Bytes} from './Trace.mjs';
import * as sdk from './generated/js/device.js';

const descriptor = new Uint8Array(await readFile(new URL('./descriptor.bin', import.meta.url)));
const F = sdk.control, C = sdk.control;
const fingerprint = new DataView(descriptor.buffer).getBigUint64(16, true);
let checks = 0, cases = 0;
const equal = (a, b, label) => {
	++checks;
	assert.deepEqual(a, b, label);
};
const truth = (value, label) => {
	++checks;
	assert(value, label);
};
const test = async action => {
	await action();
	++cases;
};
const cap = (operation, session = 17) => {
	const out = new Uint8Array(24), dv = new DataView(out.buffer);
	out.set(operation === 'ReadMany' ? [ 84, 82, 77, 49, 1, 32, 1, 1 ]
	                                 : [ 84, 66, 77, 49, 1, 32, 1, 1 ]);
	dv.setUint16(8, operation === 'ReadMany' ? 130 : 8192, true);
	dv.setUint16(10, operation === 'ReadMany' ? 514 : 8192, true);
	dv.setUint32(12, session, true);
	dv.setBigUint64(16, fingerprint, true);
	return out;
};
const rmBody = (bad = false, unavailable = false) => {
	const out = new Uint8Array(2 + 8 + 4 + 8 + (unavailable ? 0 : 1)),
	      dv = new DataView(out.buffer);
	dv.setUint16(0, 2, true);
	dv.setUint32(2, F.Counter.id, true);
	dv.setUint16(8, 4, true);
	out.set(u32Bytes(42), 10);
	dv.setUint32(14, F.Bool.id, true);
	out[18] = unavailable ? 3 : 0;
	dv.setUint16(20, unavailable ? 0 : 1, true);
	if (!unavailable)
		out[22] = bad ? 2 : 1;
	return out;
};
const batchBody = (bad = false, malformed = false) => {
	const out = new Uint8Array(2 + 12 + 12 + 1), dv = new DataView(out.buffer);
	dv.setUint16(0, 2, true);
	dv.setUint32(2, C.Reset.id, true);
	out.set([ 2, 0, 1, 0 ], 6); // checked Accepted
	dv.setUint32(14, F.Bool.id, true);
	out.set([ 4, 0, 0, 0 ], 18);
	dv.setUint16(22, 1, true);
	out[26] = bad ? 2 : 1;
	if (malformed)
		out[25] = 1;
	return out;
};

async function fixture(mode, options = {})
{
	const clock = new FakeClock(), calls = [];
	let generation = 16, sends = 0, openCount = 0;
	const profile = {
		async open(context) {
		    ++openCount;
		    const session = ++generation;
		    return {
			    peer : 'optional-peer',
			    session,
			    header : descriptor.slice(0, 64),
			    descriptor,
			    backend : {
				    supports : operation => operation === 'ReadMany' ? options.readMany !== false
				                            : operation === 'Batch'  ? options.batch !== false
				                                                     : true,
				    confirmSessionClosed : () => true,
				    execute(request, context) {
			            return new Promise(resolve => calls.push({
				            request,
				            context,
				            resolve,
				            handoff() {
				                const ok = context.handoff(true);
				                if (ok)
					                ++sends;
				                return ok;
				            }
			            }));
				    }
			    },
			    supersedes : context.previous,
			    async close() {}
		    };
		}
	};
	const connect = mode === 'static' ? connectStatic : connectDynamic;
	const session = await connect({
		sdk,
		transport : profile,
		clock,
		timeoutMs : 100,
		maxPending : options.maxPending ?? 3,
		maxPayloadBytes : options.maxPayloadBytes ?? 4096
	});
	const fields = () => mode === 'static' ? [ F.Counter, F.Bool ] : [ F.Counter.id, F.Bool.id ];
	const recipe = () =>
	    mode === 'static' ? Batch.items(Batch.command(C.Reset), Batch.read(F.Bool)) : [
		    {operation : 'Command', id : C.Reset.id}, {operation : 'FieldRead', id : F.Bool.id}
	    ];
	const request = operation => operation === 'ReadMany'
	                                 ? session.readMany(cap(operation, session.session), fields())
	                                 : session.batch(cap(operation, session.session), recipe());
	const complete = async (body, index = calls.length - 1, prepare = true) => {
		await flush();
		const call = calls[index];
		truth(call);
		if (prepare)
			truth(call.handoff());
		call.resolve({dispatch : 0, endpointStatus : 0, payload : body});
		await flush();
	};
	return {
		mode,
		clock,
		session,
		calls,
		fields,
		recipe,
		request,
		complete,
		get sends() {
		    return sends;
		},
		get opens() {
		    return openCount;
		}
	};
}

for (const mode of ['static', 'dynamic']) {
	await test(async () => {
		const f = await fixture(mode), rm = f.request('ReadMany'), batch = f.request('Batch');
		const read = f.session.read(mode === 'static' ? F.Counter : F.Counter.id);
		equal([ rm.requestId, batch.requestId, read.requestId ], [ 1, 2, 3 ]);
		const full = f.request('ReadMany');
		equal(full.requestId, 0);
		equal((await full).status, Status.QueueFull);
		await flush();
		equal(f.calls.length, 1);
		await f.complete(rmBody());
		const one = await rm;
		equal(one.items.map(x => x.value), [ 42, true ]);
		equal(one.valueStatus, 'Ok');
		equal(one.rawResult.status, Status.Ok);
		truth(Object.isFrozen(one));
		truth(Object.isFrozen(one.items));
		truth(one.items.every(Object.isFrozen));
		const payload = one.payload.slice(), second = one.items[1].payload.slice();
		one.items[0].payload.fill(0);
		equal(one.payload, payload);
		equal(one.items[1].payload, second);
		equal(f.calls.length, 2);
		await f.complete(batchBody());
		const two = await batch;
		equal(two.items[0].accepted, true);
		equal(two.items[0].executed, false);
		equal(two.items[0].valueStatus, 'NoValue');
		equal(two.items[1].value, true);
		equal(two.valueStatus, 'Ok');
		equal(two.value, undefined);
		equal(two.items[0].requestId, batch.requestId);
		equal(two.items[0].session, 17);
		truth(Object.isFrozen(two.items));
		truth(two.items.every(Object.isFrozen));
		equal(f.calls.length, 3);
		await f.complete(u32Bytes(7));
		equal((await read).value, 7);
		equal(f.sends, 3);
		await f.session.close();
	});
	for (const operation of ['ReadMany', 'Batch']) {
		await test(async () => {
			const f =
			    await fixture(mode, {[operation === 'ReadMany' ? 'readMany' : 'batch'] : false});
			equal((await f.request(operation)).status, Status.Unsupported);
			equal(f.calls.length, 0);
			const peer = f.request(operation === 'ReadMany' ? 'Batch' : 'ReadMany');
			await flush();
			equal(f.calls.length, 1);
			truth(peer.cancel());
			await peer;
			await f.session.close();
		});
		await test(async () => {
			const f = await fixture(mode);
			const submit = record => operation === 'ReadMany'
			                             ? f.session.readMany(record, f.fields())
			                             : f.session.batch(record, f.recipe());
			for (const record of [new Uint8Array(0), cap(operation, 16), cap(operation, 0),
			                      cap(operation === 'ReadMany' ? 'Batch' : 'ReadMany')]) {
				const pending = submit(record);
				equal(pending.requestId, 0);
				truth((await pending).status !== Status.Ok);
			}
			equal(f.calls.length, 0);
			await f.session.reconnect();
			equal(f.session.session, 18);
			equal((await submit(cap(operation, 17))).status, Status.NotAdmitted);
			await f.session.close();
			equal((await f.request(operation)).status, Status.Closed);
		});
		for (const phase of ['queued', 'offered', 'prepared'])
			await test(async () => {
				const f = await fixture(mode), pending = f.request(operation);
				if (phase !== 'queued') {
					await flush();
					equal(f.calls.length, 1);
				}
				if (phase === 'prepared')
					truth(f.calls[0].handoff());
				truth(pending.cancel());
				const result = await pending;
				equal(result.status,
				      phase === 'prepared'
				          ? operation === 'Batch' ? Status.OutcomeUnknown : Status.Cancelled
				          : Status.Cancelled);
				equal(result.value, undefined);
				equal(result.items, undefined);
				equal(result.requestId, pending.requestId);
				equal(result.session, pending.session);
				equal(f.session.recoveryRequired, phase === 'prepared');
				await f.session.close();
				equal(f.sends, phase === 'prepared' ? 1 : 0);
			});
		await test(async () => {
			const f = await fixture(mode), pending = f.request(operation);
			await flush();
			truth(f.calls[0].handoff());
			f.clock.advance(100);
			await flush();
			const result = await pending;
			equal(result.status, operation === 'ReadMany' ? Status.Timeout : Status.OutcomeUnknown);
			equal(result.items, undefined);
			truth(f.session.recoveryRequired);
			f.calls[0].resolve({
				dispatch : 0,
				endpointStatus : 0,
				payload : operation === 'ReadMany' ? rmBody() : batchBody()
			});
			await f.session.reconnect();
			equal(f.sends, 1);
			equal(f.calls.length, 1);
			await f.session.close();
		});
		await test(async () => {
			const f = await fixture(mode), pending = f.request(operation);
			await flush();
			await f.complete(operation === 'ReadMany' ? rmBody(true) : batchBody(true));
			const result = await pending;
			equal(result.valueStatus, 'InvalidPayload');
			equal(result.items?.every(row => row.value === undefined) ?? true, true);
			if (operation === 'ReadMany') {
				equal(result.items, undefined);
				equal(result.rawResult.status, mode === 'static' ? Status.CodecError : Status.Ok);
			} else {
				equal(result.items[0].accepted, true);
				equal(result.items[0].valueStatus, 'NoValue');
			}
			await f.session.close();
		});
		await test(async () => {
			const f = await fixture(mode);
			const original = cap(operation),
			      selected = operation === 'ReadMany' ? f.fields() : [...f.recipe() ];
			const pending = operation === 'ReadMany' ? f.session.readMany(original, selected)
			                                         : f.session.batch(original, selected);
			original.fill(0);
			selected.length = 0;
			await flush();
			equal(f.calls.length, 1);
			const before = f.calls[0].request.payload.slice();
			await f.complete(operation === 'ReadMany' ? rmBody(false, true) : batchBody());
			const result = await pending;
			equal(result.valueStatus, 'Ok');
			equal(result.items.length, 2);
			if (operation === 'ReadMany') {
				equal(result.items[1].status, 3);
				equal(result.items[1].valueStatus, 'NoValue');
			}
			equal(f.calls[0].request.payload, before);
			await f.session.close();
		});
		await test(async () => {
			const f = await fixture(mode),
			      selected = operation === 'ReadMany' ? f.fields() : [...f.recipe() ];
			let stopping;
			Object.defineProperty(selected, 0, {
				get() {
				    stopping = f.session.disconnect();
				    return operation === 'ReadMany' ? f.fields()[0] : f.recipe()[0];
				}
			});
			const pending = operation === 'ReadMany' ? f.session.readMany(cap(operation), selected)
			                                         : f.session.batch(cap(operation), selected);
			equal(pending.requestId, 0);
			equal((await pending).status, Status.NotAdmitted);
			await stopping;
			equal(f.calls.length, 0);
			await f.session.close();
		});
		await test(async () => {
			const f = await fixture(mode),
			      source = operation === 'ReadMany' ? f.fields() : [...f.recipe() ];
			let lengthReads = 0;
			const selected = new Proxy(source, {
				get(target, key) {
				    if (key === 'length') {
					    if (++lengthReads !== 1)
						    throw new Error('length must be captured exactly once');
					    return 2;
				    }
				    if (key === Symbol.iterator)
					    throw new Error('bounded indexed selection only');
				    return Reflect.get(target, key);
				}
			});
			const pending = operation === 'ReadMany' ? f.session.readMany(cap(operation), selected)
			                                         : f.session.batch(cap(operation), selected);
			truth(pending.requestId > 0,
			      'one captured array length admits a valid bounded selection');
			equal(lengthReads, 1);
			await flush();
			await f.complete(operation === 'ReadMany' ? rmBody() : batchBody());
			equal((await pending).valueStatus, 'Ok');
			await f.session.close();
		});
		await test(async () => {
			const f = await fixture(mode);
			for (const count of [0, -1, 1.5, 33, 1e9, NaN, Infinity]) {
				let lengthReads = 0, indices = 0;
				const selected = new Proxy([], {
					get(_target, key) {
					    if (key === 'length') {
						    ++lengthReads;
						    return count;
					    }
					    ++indices;
					    throw new Error('invalid count must refuse before index access');
					}
				});
				const pending = operation === 'ReadMany'
				                    ? f.session.readMany(cap(operation), selected)
				                    : f.session.batch(cap(operation), selected);
				equal((await pending).status, Status.InvalidArgument);
				equal(pending.requestId, 0);
				equal(lengthReads, 1);
				equal(indices, 0);
			}
			equal(f.calls.length, 0);
			await f.session.close();
		});
		await test(async () => {
			for (const terminal of [false, true]) {
				const f = await fixture(mode);
				let stopping, lengthReads = 0;
				const selected = new Proxy([], {
					get(_target, key) {
					    assert.equal(key, 'length');
					    ++lengthReads;
					    stopping = terminal ? f.session.close() : f.session.disconnect();
					    return 0;
					}
				});
				const pending = operation === 'ReadMany'
				                    ? f.session.readMany(cap(operation), selected)
				                    : f.session.batch(cap(operation), selected);
				equal((await pending).status, terminal ? Status.Closed : Status.NotAdmitted);
				equal(lengthReads, 1);
				equal(pending.requestId, 0);
				await stopping;
				equal(f.calls.length, 0);
				await f.session.close();
			}
		});
		await test(async () => {
			const ids = new WeakMap(), resolved = new Set();
			const hook = createHook({
				             init(id, type, _trigger, resource) {
				                 if (type === 'PROMISE')
					                 ids.set(resource, id);
				             },
				             promiseResolve(id) {
				                 resolved.add(id);
				             }
			             }).enable();
			let pendingChecked = 0, historical = 0;
			try {
				for (let cut = 0; cut <= 14; ++cut) {
					const f = await fixture(mode), pending = f.request(operation),
					      id = ids.get(pending);
					truth(id !== undefined);
					await flush();
					truth(f.calls[0].handoff());
					f.calls[0].resolve({
						dispatch : 0,
						endpointStatus : 0,
						payload : operation === 'ReadMany' ? rmBody() : batchBody()
					});
					for (let i = 0; i < cut; ++i)
						await Promise.resolve();
					const done = resolved.has(id), stopping = f.session.disconnect(),
					      result = await pending;
					await stopping;
					if (!done) {
						equal(result.valueStatus, 'Stale');
						truth(!result.items || result.items.every(row => row.value === undefined));
						if (result.status === Status.Ok) {
							++pendingChecked;
							truth(result.rawResult.succeeded);
						}
					} else {
						++historical;
						equal(result.valueStatus, 'Ok');
						truth(result.items);
					}
					if (result.items && operation === 'Batch') {
						equal(result.items[0].accepted, true);
						equal(result.items[0].executed, false);
					}
					equal(f.sends, 1);
					await f.session.close();
				}
			} finally {
				hook.disable();
			}
			truth(pendingChecked > 0);
			truth(historical > 0);
		});
	}
	await test(async () => {
		const f = await fixture(mode);
		for (const ids of [[], [...f.fields(), f.fields()[0] ], Array(33).fill(f.fields()[0])]) {
			equal((await f.session.readMany(cap('ReadMany'), ids)).status, Status.InvalidArgument);
		}
		equal((await f.session.readMany(cap('ReadMany'),
		                                mode === 'static' ? [ F.Config ] : [ F.Config.id ]))
		          .status,
		      Status.Unsupported);
		equal(f.calls.length, 0);
		await f.session.close();
	});
	await test(async () => {
		const f = await fixture(mode), pending = f.request('Batch');
		await flush();
		await f.complete(batchBody(false, true));
		const result = await pending;
		equal(result.valueStatus, 'InvalidPayload');
		equal(result.items, undefined);
		await f.session.close();
	});
}
console.log('connection-session-optional checks=' + checks + ' cases=' + cases + ' failures=0');
