// Installed public Session contract, deterministic owner clocks and delayed fences. MIT.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {createHook} from 'node:async_hooks';
import {connectStatic, connectDynamic, Status} from 'telemetry-sdk';
import * as sessions from 'telemetry-sdk/session';
import {FakeClock, flush, u32Bytes} from './Trace.mjs';
import * as sdk from './generated/js/device.js';

const descriptor = new Uint8Array(await readFile(new URL('./descriptor.bin', import.meta.url)));
const header = descriptor.slice(0, 64);
const F = sdk.control, C = sdk.control, S = sdk.control;
let checks = 0;
const cases = [];
const equal = (actual, expected, label) => {
	++checks;
	assert.deepEqual(actual, expected, label);
};
const truth = (actual, label) => {
	++checks;
	assert(actual, label);
};
const deferred = () => {
	let resolve, reject;
	const promise = new Promise((yes, no) => {
		resolve = yes;
		reject = no;
	});
	return {promise, resolve, reject};
};
const dto = (number = 42) => ({
	number,
	enabled : true,
	gains : [ 0, 65535, 17 ],
	mode : -123,
	large : 0xffffffffffffffffn,
	negative : -0x8000000000000000n,
	f32 : NaN,
	f64 : -0
});

async function rejects(promise)
{
	let error;
	try {
		await promise;
	} catch (failure) {
		error = failure;
	}
	truth(error instanceof Error, 'connection failure rejects an Error');
	return error;
}

function wire(request)
{
	let payload = new Uint8Array(), endpointStatus = 0;
	if (request.operation === 'FieldRead') {
		payload = request.endpointId === F.Counter.id ? u32Bytes(42)
		          : request.endpointId === F.Empty.id ? new Uint8Array()
		          : request.endpointId === F.Bool.id  ? new Uint8Array([ 1 ])
		                                              : F.Config.write.encode(dto());
	} else if (request.operation === 'Command' && request.endpointId === C.Accepted.id) {
		endpointStatus = 1;
	} else if (request.operation === 'Service' && request.endpointId === S.Echo.id) {
		payload = new Uint8Array(request.payload);
	}
	return {dispatch : 0, endpointStatus, payload};
}

function harness(mode, settings = {})
{
	const clock = new FakeClock(), leases = [], opens = [], calls = [];
	let next = 17, active = 0, peak = 0, sends = 0;
	const plans = [];
	const profile = {
		async open(context) {
		    opens.push(context);
		    const supplied = plans.shift();
		    if (typeof supplied === 'function')
			    return supplied(context);
		    if (supplied)
			    return supplied;
		    return lease(next++, context.previous ? {supersedes : context.previous} : {});
		}
	};

	function lease(generation, options = {})
	{
		next = Math.max(next, generation + 1);
		const entry = {
			peer : options.peer ?? 'fixture-peer',
			session : generation,
			header : options.header ?? header.slice(),
			descriptor : options.descriptor ?? descriptor.slice(),
			readMany : options.readMany,
			batch : options.batch,
			supersedes : options.supersedes,
			closeCount : 0,
			confirms : [],
			locallyClosed : false
		};
		entry.backend = {
			supports() {
			    return true;
			},
			confirmSessionClosed(old) {
			    entry.confirms.push(old);
			    return options.confirm !== false;
			},
			execute(request, context) {
			    const done = deferred();
			    const call = {
				    request,
				    context,
				    done,
				    lease : entry,
				    handoffs : 0,
				    handoff() {
				        const accepted = context.handoff(request.operation !== 'FieldRead');
				        if (accepted) {
					        sends++;
					        this.handoffs++;
				        }
				        return accepted;
				    },
				    reply(value = wire(request)) {
				        done.resolve(value);
				    }
			    };
			    calls.push(call);
			    active++;
			    peak = Math.max(peak, active);
			    if (settings.autoHandoff !== false)
				    call.handoff();
			    if (settings.autoReply)
				    call.reply();
			    return done.promise.finally(() => active--);
			}
		};
		entry.close = () => {
			entry.closeCount++;
			return (options.closeFence?.promise ?? Promise.resolve()).then(() => {
				entry.locallyClosed = true;
				if (options.closeError)
					throw options.closeError;
			});
		};
		leases.push(entry);
		return entry;
	}

	const connect = () =>
	    mode === 'static'
	        ? connectStatic({sdk, transport : profile, clock, timeoutMs : 100, maxPending : 8})
	        : connectDynamic({transport : profile, clock, timeoutMs : 100, maxPending : 8});
	const read = (session, field = F.Counter) => session.read(mode === 'static' ? field : field.id);
	const write = (session, value = dto(90)) =>
	    session.write(mode === 'static' ? F.Config : F.Config.id, value);
	const preserving = (session, value, original, endpoint = F.Config, timeout) =>
	    session.writePreserving(mode === 'static' ? endpoint : endpoint.id, value, original,
	                            timeout);
	const command = (session, endpoint = C.Configure, value = dto()) =>
	    session.command(mode === 'static' ? endpoint : endpoint.id, value);
	const service = (session, endpoint = S.Echo, ...values) => session.service(
	    mode === 'static' ? endpoint : endpoint.id, values.length ? values[0] : dto());
	return {
		mode,
		clock,
		leases,
		opens,
		calls,
		profile,
		plans,
		lease,
		connect,
		read,
		write,
		preserving,
		command,
		service,
		get sends() {
		    return sends;
		},
		get peak() {
		    return peak;
		}
	};
}

async function test(mode, name, body)
{
	const start = checks;
	await body();
	cases.push({mode, name, checks : checks - start});
}

function facts(result)
{
	truth(Object.isFrozen(result));
	for (const key of ['status', 'operation', 'session', 'requestId', 'dispatch', 'endpointStatus',
	                   'succeeded', 'accepted', 'executed', 'payload'])
		equal(result[key], result.rawResult[key], 'flat operation fact ' + key);
}

equal(connectStatic, sessions.connectStatic);
equal(connectDynamic, sessions.connectDynamic);

for (const mode of ['static', 'dynamic']) {
	await test(
	    mode, 'preserving writes own offset seed bytes and share the existing FIFO', async () => {
		    const h = harness(mode), session = await h.connect();
		    const source = F.Config.write.encode(dto(12)), expected = source.slice();
		    const bits = new DataView(source.buffer);
		    bits.setUint32(source.length - 12, 0xff812345, true);
		    bits.setBigUint64(source.length - 8, 0x8000000000000000n, true);
		    expected.set(source);
		    new DataView(expected.buffer).setUint32(0, 23, true);
		    const storage = new Uint8Array(source.length + 6);
		    storage.set(source, 3);
		    const seed = new DataView(storage.buffer, 3, source.length);
		    const value = F.Config.read.decode(source);
		    Object.defineProperty(value, 'number', {
			    get() {
			        storage.fill(0);
			        return 23;
			    }
		    });
		    let endpoint = F.Config;
		    if (mode === 'static')
			    endpoint = {
				    read : F.Config.read,
				    write : {
					    ...F.Config.write,
					    get encodePreserving() {
				            storage.fill(0);
				            return F.Config.write.encodePreserving;
					    }
				    }
			    };
		    const first = h.read(session), request = h.preserving(session, value, seed, endpoint);
		    storage.fill(255);
		    equal(request.requestId, 2);
		    equal(request.session, 17);
		    await flush();
		    equal(h.calls.length, 1);
		    h.calls[0].reply();
		    await first;
		    await flush();
		    equal(h.calls.length, 2);
		    equal(h.calls[1].request.operation, 'FieldWrite');
		    equal(h.calls[1].request.payload, expected);
		    h.calls[1].reply();
		    const result = await request;
		    equal(result.succeeded, true);
		    equal(result.valueStatus, 'NoValue');
		    facts(result);
		    equal(h.peak, 1);
		    await session.close();
	    });
	await test(
	    mode, 'preserving invalid seeds and binding policy refuse without canonical fallback',
	    async () => {
		    const h = harness(mode), session = await h.connect(),
		          seed = F.Config.write.encode(dto());
		    const refused = async (request, status) => {
			    equal(request.requestId, 0);
			    equal(request.session, 17);
			    equal(request.cancel(), false);
			    const result = await request;
			    equal(result.status, status);
			    equal(result.operation, 'FieldWrite');
			    equal(result.succeeded, false);
			    facts(result);
		    };
		    await refused(h.preserving(session, dto(), null), Status.InvalidArgument);
		    await refused(h.preserving(session, dto(), seed.subarray(1)), Status.CodecError);
		    const invalidBool = seed.slice();
		    invalidBool[4] = 2;
		    await refused(h.preserving(session, dto(), invalidBool), Status.CodecError);
		    await refused(h.preserving(session, dto(), seed, F.ReadOnly), Status.ReadOnly);
		    await refused(h.preserving(session, dto(), seed, F.Config, 0), Status.InvalidArgument);
		    if (mode === 'static') {
			    let ordinary = 0;
			    const unsupported = {
				    ...F.Config.write,
				    encode() {
				        ordinary++;
				        return seed;
				    },
				    encodePreserving : undefined
			    };
			    await refused(h.preserving(session, dto(), seed, unsupported), Status.Unsupported);
			    equal(ordinary, 0);
			    await refused(h.preserving(session, dto(), seed,
			                               {...F.Config.write, fingerprint : sdk.fingerprint ^ 1n}),
			                  Status.NotAdmitted);
			    await refused(
			        h.preserving(session, dto(), seed, {...F.Config.write, operation : 'Command'}),
			        Status.InvalidArgument);
		    } else {
			    await refused(session.writePreserving(NaN, dto(), seed), Status.InvalidArgument);
			    await refused(session.writePreserving(0xffffffff, dto(), seed),
			                  Status.InvalidArgument);
		    }
		    await flush();
		    equal(h.calls.length, 0);
		    await session.close();
		    const closed = await h.preserving(session, dto(), seed);
		    equal(closed.status, Status.Closed);
		    equal(closed.valueStatus, 'Stale');
	    });
	await test(mode, 'preserving requests retain queued and handed-off cancellation facts',
	           async () => {
		           const seed = F.Config.write.encode(dto());
		           const h = harness(mode), session = await h.connect(), first = h.read(session);
		           const queued = h.preserving(session, dto(), seed);
		           equal(queued.requestId, 2);
		           truth(queued.cancel());
		           const cancelled = await queued;
		           equal(cancelled.status, Status.Cancelled);
		           equal(cancelled.requestId, 2);
		           equal(cancelled.session, 17);
		           facts(cancelled);
		           equal(queued.cancel(), false);
		           await flush();
		           equal(h.calls.length, 1);
		           h.calls[0].reply();
		           await first;
		           await session.close();
		           const k = harness(mode), active = await k.connect();
		           const sent = k.preserving(active, dto(), seed);
		           await flush();
		           truth(sent.cancel());
		           const unknown = await sent;
		           equal(unknown.status, Status.OutcomeUnknown);
		           equal(unknown.requestId, sent.requestId);
		           equal(unknown.session, sent.session);
		           facts(unknown);
		           truth(active.recoveryRequired);
		           k.calls[0].reply();
		           await active.close();
		           equal(k.sends, 1);
	           });
	await test(mode, 'preserving getter revocation cannot submit into a retired admission',
	           async () => {
		           const h = harness(mode), session = await h.connect(),
		                 seed = F.Config.write.encode(dto());
		           const value = dto();
		           let disconnected;
		           Object.defineProperty(value, 'number', {
			           get() {
			               disconnected = session.disconnect();
			               return 42;
			           }
		           });
		           const request = h.preserving(session, value, seed), result = await request;
		           equal(request.requestId, 0);
		           equal(result.status, Status.NotAdmitted);
		           equal(result.valueStatus, 'Stale');
		           facts(result);
		           await disconnected;
		           await flush();
		           equal(h.calls.length, 0);
		           await session.reconnect();
		           const current = h.preserving(session, dto(), seed);
		           equal(current.session, 18);
		           await flush();
		           h.calls[0].reply();
		           equal((await current).succeeded, true);
		           await session.close();
	           });
	await test(mode, 'admission, generated values, all operations and one FIFO', async () => {
		const h = harness(mode), session = await h.connect();
		equal(session.state, 'ready');
		truth(session.ready);
		equal(session.session, 17);
		truth(Object.isFrozen(session));
		for (const internal
		         of ['core', 'forwarder', 'record', 'active', 'claim', 'closeLease', 'track'])
			equal(internal in session, false,
			      'No untracked execution/lifecycle escape hatch: ' + internal);
		equal(session.mode, mode);
		equal(mode === 'static' ? session.sdk : 'sdk' in session, mode === 'static' ? sdk : false);
		equal(session.fingerprint, sdk.fingerprint);
		equal(mode === 'dynamic' ? session.schema.fields.length : session.metadata,
		      mode === 'dynamic' ? 12 : undefined);
		const requests = [
			h.read(session), h.write(session), h.command(session, C.Accepted), h.service(session)
		];
		requests.forEach((request, index) => {
			equal(request.requestId, index + 1);
			equal(request.session, 17);
			equal(typeof request.cancel, 'function');
		});
		for (let index = 0; index < requests.length; index++) {
			await flush();
			equal(h.calls.length, index + 1);
			h.calls[index].reply();
			await requests[index];
		}
		const results = await Promise.all(requests);
		results.forEach(facts);
		equal(results[0].value, 42);
		equal(results[0].valueStatus, 'Ok');
		equal(results[1].valueStatus, 'NoValue');
		equal(results[1].succeeded, true);
		equal(results[2].accepted, true);
		equal(results[2].executed, false);
		equal(results[3].value.number, 42);
		equal(results[3].value.large, 0xffffffffffffffffn);
		truth(Number.isNaN(results[3].value.f32));
		truth(Object.is(results[3].value.f64, -0));
		equal(h.peak, 1);
		equal(h.sends, 4);
		await session.close();
		equal(session.state, 'closed');
	});
	await test(mode, 'empty aggregate is a value; void and codec failures remain distinct',
	           async () => {
		           const h = harness(mode), session = await h.connect();
		           for (const [endpoint, value, status] of [[ S.EmptyService, {}, 'Ok' ],
		                                                    [ S.Ping, undefined, 'NoValue' ]]) {
			           const request = h.service(session, endpoint, undefined);
			           await flush();
			           h.calls.at(-1).reply();
			           const result = await request;
			           facts(result);
			           if (value === undefined)
				           equal(result.value, undefined);
			           else {
				           truth(result.value !== undefined);
				           equal(Object.keys(result.value), []);
			           }
			           equal(result.valueStatus, status);
		           }
		           const request = h.read(session, F.Bool);
		           await flush();
		           h.calls.at(-1).reply(
		               {dispatch : 0, endpointStatus : 0, payload : new Uint8Array([ 2 ])});
		           const result = await request;
		           equal(result.valueStatus, 'InvalidPayload');
		           equal(result.value, undefined);
		           // Dynamic separates validated envelope facts from runtime value decoding.
		           equal(result.status, mode === 'static' ? Status.CodecError : Status.Ok);
		           facts(result);
		           await session.close();
	           });
	await test(mode, 'wrong identity or malformed Descriptor never executes', async () => {
		const h = harness(mode);
		const changed = (mode === 'static' ? header : descriptor).slice();
		if (mode === 'static')
			changed[40] ^= 1; // Same declared fingerprint, different exact header.
		else
			changed[0] = 0;
		h.plans.push(h.lease(17, mode === 'static' ? {header : changed} : {descriptor : changed}));
		await rejects(h.connect());
		equal(h.calls.length, 0);
		equal(h.sends, 0);
		equal(h.leases[0].closeCount, 1);
		truth(h.leases[0].locallyClosed);
	});
	await test(mode, 'capability discovery is copied data and grants no operation flags',
	           async () => {
		           const h = harness(mode), readMany = new Uint8Array([ 1, 2, 3 ]),
		                 batch = new Uint8Array([ 4, 5 ]);
		           h.plans.push(h.lease(17, {readMany, batch}));
		           const session = await h.connect();
		           readMany[0] = 9;
		           batch[0] = 9;
		           equal(session.capabilities.readMany, new Uint8Array([ 1, 2, 3 ]));
		           equal(session.capabilities.batch, new Uint8Array([ 4, 5 ]));
		           const copy = session.capabilities;
		           copy.readMany[0] = 8;
		           equal(session.capabilities.readMany[0], 1);
		           equal('readManySupported' in session, false);
		           equal('batchSupported' in session, false);
		           await session.disconnect();
		           equal(session.capabilities, undefined);
		           await session.close();
	           });
	await test(mode, 'queued disconnect cancels once without handoff', async () => {
		const h = harness(mode), session = await h.connect();
		const first = h.write(session), second = h.command(session);
		let completions = 0;
		first.then(() => completions++);
		second.then(() => completions++);
		const stopping = session.disconnect();
		truth(!session.ready);
		await stopping;
		await flush();
		equal((await first).status, Status.Cancelled);
		equal((await second).status, Status.Cancelled);
		equal(completions, 2);
		equal(h.sends, 0);
		equal(h.calls.length, 0);
		equal(session.state, 'disconnected');
		await session.close();
	});
	await test(mode, 'active pre-handoff cancellation cannot become uncertain', async () => {
		const h = harness(mode, {autoHandoff : false}), session = await h.connect();
		const request = h.write(session);
		await flush();
		equal(h.calls.length, 1);
		truth(request.cancel());
		equal((await request).status, Status.Cancelled);
		equal(h.calls[0].handoff(), false);
		equal(h.sends, 0);
		await session.close();
		h.calls[0].reply();
		await flush();
	});
	await test(mode, 'post-handoff mutation loss retains uncertainty and request identity',
	           async () => {
		           const h = harness(mode), session = await h.connect();
		           const request = h.command(session);
		           let completions = 0;
		           request.then(() => completions++);
		           await flush();
		           equal(h.sends, 1);
		           const requestId = request.requestId, generation = request.session;
		           await session.disconnect();
		           const result = await request;
		           facts(result);
		           equal(result.status, Status.OutcomeUnknown);
		           equal(result.requestId, requestId);
		           equal(result.session, generation);
		           equal(result.valueStatus, 'Stale');
		           truth(session.recoveryRequired);
		           equal(session.recoveryGeneration, 17);
		           h.calls[0].reply();
		           await flush();
		           equal(completions, 1);
		           equal(result.status, Status.OutcomeUnknown);
		           await session.close();
		           truth(session.recoveryRequired);
		           equal(session.recoveryGeneration, 17);
	           });
	await test(mode, 'checked outcomes survive subsequent loss', async () => {
		const h = harness(mode), session = await h.connect();
		const request = h.command(session, C.Accepted);
		await flush();
		h.calls[0].reply();
		const result = await request;
		equal(result.accepted, true);
		equal(result.executed, false);
		await session.disconnect();
		equal(result.accepted, true);
		equal(result.status, Status.Ok);
		equal(result.rawResult.endpointStatus, 1);
		await session.close();
	});
	await test(mode, 'checked read facts survive loss before wrapper projection', async () => {
		const h = harness(mode), session = await h.connect(), request = h.read(session);
		await flush();
		let scheduled = false, stopping;
		h.calls[0].reply({
			dispatch : 0,
			endpointStatus : 0,
			get payload() {
			    if (!scheduled) {
				    scheduled = true;
				    queueMicrotask(() => {
					    stopping = session.disconnect();
				    });
			    }
			    return u32Bytes(42);
			}
		});
		const result = await request;
		await stopping;
		truth(scheduled);
		equal(result.status, Status.Ok);
		equal(result.succeeded, true);
		equal(result.valueStatus, 'Stale');
		equal(result.value, undefined);
		equal(result.rawResult.payload, u32Bytes(42));
		facts(result);
		await session.close();
	});
	await test(
	    mode, 'microtask revocation before public resolution hides checked values', async () => {
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
		    let pendingChecked = 0, historicalCompleted = 0, pendingUnconfirmed = 0;
		    try {
			    // Observe actual Promise resolution, rather than inferring it from a .then
			    // callback that may itself run after disconnect. Sweeping these boundaries
			    // covers both Client -> DynamicClient and DynamicClient -> Session mapping.
			    for (let cut = 0; cut <= 12; cut++) {
				    const h = harness(mode), session = await h.connect(), request = h.read(session);
				    const publicId = ids.get(request);
				    truth(publicId !== undefined);
				    await flush();
				    h.calls[0].reply();
				    for (let step = 0; step < cut; step++)
					    await Promise.resolve();
				    const resolvedBeforeDisconnect = resolved.has(publicId);
				    const stopping = session.disconnect();
				    truth(!session.ready);
				    const result = await request;
				    await stopping;
				    equal(result.status, result.rawResult.status);
				    equal(result.succeeded, result.rawResult.succeeded);
				    equal(result.requestId, request.requestId);
				    equal(result.session, request.session);
				    if (!resolvedBeforeDisconnect) {
					    equal(result.valueStatus, 'Stale',
					          mode + ' pending public Promise at cut ' + cut);
					    equal(result.value, undefined,
					          mode + ' pending public Promise at cut ' + cut);
					    if (result.status === Status.Ok) {
						    pendingChecked++;
						    truth(result.succeeded);
						    equal(result.rawResult.payload, u32Bytes(42));
					    } else
						    pendingUnconfirmed++;
				    } else {
					    historicalCompleted++;
					    equal(result.status, Status.Ok);
					    equal(result.valueStatus, 'Ok');
					    equal(result.value, 42);
				    }
				    await session.close();
			    }
		    } finally {
			    hook.disable();
		    }
		    truth(pendingChecked > 0,
		          'Covers a checked operation while SessionRequest is still unresolved');
		    truth(historicalCompleted > 0, 'Covers already-resolved historical results');
		    truth(pendingUnconfirmed > 0, 'Covers loss before checked operation completion');
	    });
	await test(mode, 'queue refusal and old Dynamic row never bypass tracked numeric binding',
	           async () => {
		           const h = harness(mode), session = await h.connect();
		           const oldRow = mode === 'dynamic'
		                              ? session.schema.fields.find(row => row.id === F.Counter.id)
		                              : undefined;
		           const queued = Array.from({length : 8}, () => h.read(session));
		           const refused = h.read(session);
		           equal(refused.requestId, 0);
		           equal(refused.cancel(), false);
		           queued.forEach(request => truth(request.cancel()));
		           equal((await refused).status, Status.QueueFull);
		           await Promise.all(queued);
		           equal(h.sends, 0);
		           await session.reconnect();
		           if (mode === 'dynamic') {
			           truth(session.schema.fields.find(row => row.id === F.Counter.id) !== oldRow);
			           const stale = session.read(oldRow);
			           equal(stale.requestId, 0);
			           equal(stale.cancel(), false);
			           equal((await stale).status, Status.InvalidArgument);
			           equal(h.sends, 0);
		           }
		           await session.disconnect();
		           const disconnected = h.read(session);
		           equal(disconnected.requestId, 0);
		           equal(disconnected.cancel(), false);
		           truth((await disconnected).status !== Status.Ok);
		           await session.close();
	           });
	await test(mode, 'local close fence blocks candidate and old backend confirmation',
	           async () => {
		           const h = harness(mode), fence = deferred();
		           h.plans.push(h.lease(17, {closeFence : fence}));
		           const session = await h.connect(), request = h.write(session);
		           await flush();
		           const reconnecting = session.reconnect();
		           await flush();
		           equal(h.opens.length, 1);
		           equal(h.leases[0].locallyClosed, false);
		           equal(h.leases[0].confirms.length, 0);
		           truth(!session.ready);
		           equal((await request).status, Status.OutcomeUnknown);
		           fence.resolve();
		           await reconnecting;
		           equal(session.session, 18);
		           equal(h.leases[0].confirms, [ 17 ]);
		           equal(h.sends, 1);
		           await session.close();
	           });
	await test(mode, 'failed local close preserves obligation and blocks new open', async () => {
		const h = harness(mode);
		h.plans.push(h.lease(17, {closeError : new Error('local close failed')}));
		const session = await h.connect();
		await rejects(session.disconnect());
		truth(!session.ready);
		await rejects(session.reconnect());
		equal(h.opens.length, 1);
		await rejects(session.close());
		equal(session.state, 'closed');
	});
	await test(mode, 'local close alone cannot discharge missing remote proof', async () => {
		const h = harness(mode), session = await h.connect(), request = h.write(session);
		await flush();
		await session.disconnect();
		equal((await request).status, Status.OutcomeUnknown);
		h.plans.push(h.lease(18));
		await rejects(session.reconnect());
		truth(!session.ready);
		truth(session.recoveryRequired);
		equal(h.leases[1].closeCount, 1);
		equal(h.leases[0].confirms, []);
		equal(h.sends, 1);
		await session.close();
	});
	await test(mode, 'wrong peer, old generation and mismatched receipt fail closed', async () => {
		for (const options
		         of [{peer : 'other', supersedes : {peer : 'fixture-peer', session : 17}},
		             {supersedes : {peer : 'fixture-peer', session : 16}},
		             {supersedes : {peer : 'other', session : 17}},
		             {session : 17, supersedes : {peer : 'fixture-peer', session : 17}}]) {
			const h = harness(mode), session = await h.connect();
			await session.disconnect();
			h.plans.push(h.lease(options.session ?? 18, options));
			await rejects(session.reconnect());
			truth(!session.ready);
			equal(h.leases[0].confirms, []);
			equal(h.leases[1].closeCount, 1);
			equal(h.sends, 0);
			await session.close();
		}
	});
	await test(mode, 'exact fresh proof reuses core and does not replay old work', async () => {
		const h = harness(mode), session = await h.connect(), request = h.write(session);
		await flush();
		await session.disconnect();
		const old = await request;
		await session.reconnect();
		equal(session.session, 18);
		truth(session.ready);
		equal(h.opens[1].previous, {peer : 'fixture-peer', session : 17});
		equal(h.leases[0].confirms, [ 17 ]);
		equal(h.leases[1].confirms, []);
		equal(h.sends, 1);
		truth(!session.recoveryRequired);
		equal(old.status, Status.OutcomeUnknown);
		const fresh = h.read(session);
		await flush();
		equal(h.calls.length, 2);
		equal(fresh.session, 18);
		equal(fresh.requestId, 1);
		h.calls[1].reply();
		equal((await fresh).value, 42);
		equal(h.sends, 2);
		await session.close();
	});
	await test(mode, 'old confirmation refusal closes candidate and keeps recovery', async () => {
		const h = harness(mode);
		h.plans.push(h.lease(17, {confirm : false}));
		const session = await h.connect(), request = h.write(session);
		await flush();
		await session.disconnect();
		await request;
		await rejects(session.reconnect());
		equal(h.leases[0].confirms, [ 17 ]);
		equal(h.leases[1].closeCount, 1);
		truth(session.recoveryRequired);
		truth(!session.ready);
		await session.close();
	});
	await test(mode, 'late old bootstrap owns cleanup and cannot publish ready', async () => {
		const h = harness(mode), session = await h.connect();
		await session.disconnect();
		const opening = deferred();
		h.plans.push(() => opening.promise);
		const reconnecting = session.reconnect();
		const observed = reconnecting.catch(error => error);
		await flush();
		equal(h.opens.length, 2);
		const closing = session.close();
		await flush();
		truth(h.opens[1].signal.aborted);
		const late = h.lease(18, {supersedes : {peer : 'fixture-peer', session : 17}});
		opening.resolve(late);
		await observed;
		await closing;
		equal(session.state, 'closed');
		truth(!session.ready);
		equal(late.closeCount, 1);
		await rejects(session.reconnect());
	});
	await test(mode, 'old settlement cannot release a new invocation after proof', async () => {
		const h = harness(mode), session = await h.connect(), uncertain = h.command(session);
		await flush();
		await session.reconnect();
		equal((await uncertain).status, Status.OutcomeUnknown);
		// The first Backend promise deliberately never settled before exact external confirmation.
		const first = h.read(session), second = h.read(session);
		await flush();
		equal(h.calls.length, 2);
		h.calls[0].reply();
		await flush();
		equal(h.calls.length, 2);
		h.calls[1].reply();
		await first;
		await flush();
		equal(h.calls.length, 3);
		h.calls[2].reply();
		await second;
		equal(h.sends, 3);
		await session.close();
	});
	await test(mode, 'old disconnect callbacks do not revoke a fresh generation', async () => {
		const h = harness(mode), session = await h.connect();
		await session.reconnect();
		h.opens[0].onDisconnect(new Error('late old socket event'));
		await flush();
		truth(session.ready);
		equal(session.session, 18);
		h.opens[1].onDisconnect(new Error('current socket lost'));
		truth(!session.ready);
		await session.disconnect();
		truth(!session.ready);
		await session.close();
	});
	await test(mode, 'one profile is exclusive across owners and retains retirement', async () => {
		const h = harness(mode), first = await h.connect();
		await rejects(h.connect());
		equal(h.opens.length, 1);
		const request = h.write(first);
		await flush();
		await first.close();
		const old = await request;
		equal(old.status, Status.OutcomeUnknown);
		truth(first.recoveryRequired);
		h.plans.push(h.lease(18));
		await rejects(h.connect());
		equal(h.leases[1].closeCount, 1);
		h.plans.push(h.lease(19, {supersedes : {peer : 'fixture-peer', session : 17}}));
		const second = await h.connect();
		truth(second.ready);
		equal(second.session, 19);
		equal(h.leases[0].confirms, [ 17 ]);
		truth(first.recoveryRequired);
		equal(old.status, Status.OutcomeUnknown);
		equal(h.sends, 1);
		await second.close();
	});
	await test(mode, 'reentrant observers and request cancellation preserve identity once',
	           async () => {
		           const h = harness(mode), session = await h.connect();
		           let observed = 0, afterClosedReady = 0;
		           session.subscribe(() => {
			           throw new Error('observer failure is isolated');
		           });
		           const stop = session.subscribe(snapshot => {
			           truth(Object.isFrozen(snapshot));
			           observed++;
		           });
		           equal(observed, 1);
		           const request = h.read(session);
		           let completions = 0;
		           request.then(() => completions++);
		           const id = request.requestId, generation = request.session;
		           truth(request.cancel());
		           equal((await request).status, Status.Cancelled);
		           equal(request.cancel(), false);
		           equal(request.requestId, id);
		           equal(request.session, generation);
		           await session.reconnect();
		           equal(completions, 1);
		           truth(observed > 1);
		           stop();
		           let closed = false, closing;
		           session.subscribe(snapshot => {
			           if (snapshot.state === 'closed')
				           closed = true;
			           if (closed && snapshot.ready)
				           afterClosedReady++;
			           if (snapshot.ready && snapshot.session === 19)
				           closing = session.close();
		           });
		           await session.reconnect().catch(() => {
		                                           });
		           await closing;
		           equal(session.state, 'closed');
		           equal(afterClosedReady, 0);
		           await rejects(session.reconnect());
	           });
	await test(mode, 'explicit reentrant disconnect retires an outer reconnect attempt',
	           async () => {
		           const h = harness(mode), session = await h.connect();
		           let once = true, nested;
		           const stop = session.subscribe(snapshot => {
			           if (once && snapshot.state === 'disconnecting') {
				           once = false;
				           nested = session.disconnect();
			           }
		           });
		           await rejects(session.reconnect());
		           await nested;
		           equal(once, false);
		           truth(!session.ready);
		           equal(h.opens.length, 1);
		           stop();
		           await session.close();
	           });
	await test(mode, 'owner clock timeout preserves raw mutation outcome and terminal diagnostics',
	           async () => {
		           const h = harness(mode), session = await h.connect(), request = h.write(session);
		           await flush();
		           h.clock.advance(100);
		           await flush();
		           const result = await request;
		           equal(result.status, Status.OutcomeUnknown);
		           facts(result);
		           truth(session.recoveryRequired);
		           equal(result.requestId, request.requestId);
		           await session.close();
		           truth(session.recoveryRequired);
		           equal(session.state, 'closed');
	           });
}

console.log(JSON.stringify({cases, checks, failures : 0}));
console.log('connection-session checks=' + checks + ' cases=' + cases.length + ' failures=0');
