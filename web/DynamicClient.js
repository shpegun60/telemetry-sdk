/*
 * Headless runtime Descriptor sessions over the existing asynchronous client FIFO.
 *
 * The wrapper owns admission and opaque endpoint handles, not another execution
 * engine. Descriptor parsing and value codecs remain in telemetry.js. Results
 * own their bytes; operation outcomes survive revoked value interpretation.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
import {DeviceClient, Status, Batch} from '../lib/client/web/Client.js';
import {
	TypeKind,
	findType,
	findEndpoint,
	matchesDescriptorHeader,
	encodeValue,
	encodeValuePreserving,
	decodeValue
} from './telemetry.js';

export const ValueStatus = Object.freeze(
    {Ok : 'Ok', Stale : 'Stale', NoValue : 'NoValue', InvalidPayload : 'InvalidPayload'});
const ownBytes = input =>
    new Uint8Array(input instanceof ArrayBuffer
                       ? new Uint8Array(input)
                       : new Uint8Array(input.buffer, input.byteOffset, input.byteLength));
const categories = Object.freeze(
    {FieldRead : 'fields', FieldWrite : 'fields', Command : 'commands', Service : 'services'});
const noDecode = () => undefined;
const kindNames = Object.freeze([ 'Field', 'Command', 'Service' ]);

// The .member and [i] steps after an endpoint, checked against its value type.
// Throws the generated device's wording when a step does not exist.
function memberSteps(model, typeId, suffix, path)
{
	const steps = [];
	let type = model.types[typeId], rest = suffix;
	while (rest !== '') {
		const match = /^\.([^.[\]\\]+)|^\[(0|[1-9][0-9]*)\]/.exec(rest);
		let next;
		if (match && match[1] !== undefined && type.kind === TypeKind.Struct) {
			const member = type.members.find(item => item.name === match[1]);
			if (member) {
				steps.push(match[1]);
				next = model.types[member.typeId];
			}
		} else if (match && match[2] !== undefined && type.kind === TypeKind.Array &&
		           Number(match[2]) < type.elementCount) {
			steps.push(Number(match[2]));
			next = model.types[type.elementTypeId];
		}
		if (next === undefined)
			throw new Error(`No member at path "${path}"`);
		type = next;
		rest = rest.slice(match[0].length);
	}
	return steps;
}

// Public methods:
// - admit()/invalidate()/confirmQuiescence()/close(): Explicit session lifecycle.
// - findType(): Look up a v3.1 type name in the admitted schema.
// - bind()/bindReadMany(): Cache runtime contracts for one admission.
// - endpoint(): A handle by path, group/.../endpoint[.member][[i]], for one admission.
// - readField()/writeField()/command()/service()/readMany()/batch(): One core FIFO.
// - process(): Advance the same core for controlled owner clocks.
export class DynamicClient {
	#core;
	#token;
	#candidate;
	#closed = false;
	#quiescent = true;
	#admitting = false;
	#revision = 0;
	#projecting = 0;
	#producers = 0;
	#endpoints = new WeakMap();
	#readMany = new WeakMap();
	#cache = new Map();

	constructor(options)
	{
		// The caller supplies a backend/clock/capacities, never a static identity bypass.
		this.#core = new DeviceClient({
			backend : options.backend,
			clock : options.clock,
			maxPending : options.maxPending,
			maxPayloadBytes : options.maxPayloadBytes,
			timeoutMs : options.timeoutMs,
			matchesHeader : header =>
			    !!this.#candidate && matchesDescriptorHeader(this.#candidate.model, header)
		});
	}

	get session()
	{
		return this.#core.session;
	}

	get schema()
	{
		return this.#current() ? this.#token.model : undefined;
	}

	// The admitted schema's Enum/Struct with this unique v3.1 type name. It is
	// undefined without a current admission or when no type has the name.
	findType(name)
	{
		const model = this.schema;
		return model ? findType(model, name) : undefined;
	}

	get ready()
	{
		return this.#gate() === Status.Ok;
	}

	get pendingCount()
	{
		return this.#core.pendingCount;
	}

	get recoveryRequired()
	{
		return this.#core.recoveryRequired;
	}

	get recoveryGeneration()
	{
		return this.#core.recoveryGeneration;
	}

	get drained()
	{
		return this.#core.drained && this.#projecting === 0 && this.#producers === 0 &&
		       !this.#admitting;
	}

	admit(model, observedHeader, session)
	{
		if (this.#closed)
			return Status.Closed;
		if (this.#core.recoveryRequired)
			return Status.RecoveryRequired;
		if (this.#admitting || this.#producers || !this.#quiescent || this.#projecting ||
		    this.#core.pendingCount)
			return Status.Busy;
		this.#admitting = true;
		const revision = this.#revision;
		try {
			const header = ownBytes(observedHeader);
			if (!matchesDescriptorHeader(model, header))
				return Status.InvalidArgument;
			if (revision !== this.#revision || this.#closed)
				return this.#closed ? Status.Closed : Status.NotAdmitted;
			this.#candidate = {model};
			if (!this.#core.openSession(session, header))
				return Status.NotAdmitted;
			if (revision !== this.#revision || this.#closed) {
				this.#core.invalidateAdmission();
				return this.#closed ? Status.Closed : Status.NotAdmitted;
			}
			this.#token = {model, session, active : true};
			this.#cache.clear();
			this.#quiescent = false;
			return Status.Ok;
		} catch {
			return Status.InvalidArgument;
		} finally {
			this.#candidate = undefined;
			this.#admitting = false;
		}
	}

	invalidate()
	{
		++this.#revision;
		if (this.#token)
			this.#token.active = false;
		this.#cache.clear();
		this.#core.invalidateAdmission();
	}

	confirmQuiescence(proof)
	{
		try {
			if (this.#admitting || this.#producers || this.#current() || this.#projecting ||
			    this.#core.pendingCount || !proof)
				return false;
			const revision = this.#revision;
			const {session, txReleased, oldFramesExcluded, remoteSessionClosed} = proof;
			if (revision !== this.#revision || this.#producers || this.#current() ||
			    this.#projecting || this.#core.pendingCount || session !== this.#core.session ||
			    txReleased !== true || oldFramesExcluded !== true || remoteSessionClosed !== true)
				return false;
			if (!this.#core.confirmSessionClosed(session))
				return false;
			this.#quiescent = true;
			return true;
		} catch {
			return false;
		}
	}

	close()
	{
		this.#closed = true;
		++this.#revision;
		if (this.#token)
			this.#token.active = false;
		this.#cache.clear();
		this.#core.close();
	}

	process()
	{
		this.#core.process();
	}

	bind(operation, id)
	{
		const gate = this.#gate();
		if (gate !== Status.Ok)
			return Object.freeze({status : gate});
		if (typeof operation !== 'string' || !Object.hasOwn(categories, operation) ||
		    !Number.isInteger(id) || id < 0 || id > 0xffffffff)
			return Object.freeze({status : Status.InvalidArgument});
		const key = operation + ':' + id;
		if (this.#cache.has(key))
			return this.#cache.get(key);
		const token = this.#token, model = token.model;
		const row = model[categories[operation]].find(endpoint => endpoint.id === id);
		if (!row)
			return Object.freeze({status : Status.InvalidArgument});
		if (operation === 'FieldWrite' && !(row.capabilities & 2))
			return Object.freeze({status : Status.ReadOnly});
		const requestType = operation === 'FieldRead'    ? 0
		                    : operation === 'FieldWrite' ? row.typeId
		                                                 : row.requestTypeId;
		const responseType = operation === 'FieldRead' ? row.typeId
		                     : operation === 'Service' ? row.responseTypeId
		                                               : 0;
		const endpoint =
		    Object.freeze({operation, id, session : token.session, requestType, responseType});
		const binding = Object.freeze({
			fingerprint : model.fingerprint,
			operation,
			id,
			requestBytes : model.types[requestType].wireBytes,
			responseBytes : model.types[responseType].wireBytes,
			responseTypeId : responseType,
			encode : value => {
			    if (!this.#current(token))
				    throw new Error('Retired runtime binding');
			    return encodeValue(model, requestType, value);
			},
			decode : noDecode,
			decodeResponse : noDecode
		});
		this.#endpoints.set(endpoint, {token, endpoint, binding});
		const result = Object.freeze({status : Status.Ok, endpoint});
		this.#cache.set(key, result);
		return result;
	}

	readField(endpoint, timeoutMs)
	{
		return this.#produce(() => this.#submit('FieldRead', endpoint, undefined, timeoutMs));
	}

	writeField(endpoint, value, timeoutMs)
	{
		return this.#produce(() => this.#submit('FieldWrite', endpoint, value, timeoutMs));
	}

	writeFieldPreserving(endpoint, value, originalPayload, timeoutMs)
	{
		return this.#produce(
		    () => this.#submit('FieldWrite', endpoint, value, timeoutMs, originalPayload, true));
	}

	command(endpoint, value = undefined, timeoutMs)
	{
		return this.#produce(() => this.#submit('Command', endpoint, value, timeoutMs));
	}

	service(endpoint, value = undefined, timeoutMs)
	{
		return this.#produce(() => this.#submit('Service', endpoint, value, timeoutMs));
	}

	// A handle by path (plan section 15): group/.../endpoint, then .member and [i]
	// for part of a Field. It is resolved once against the admitted descriptor and
	// keeps working until another admission. read(), write(value) and call(value)
	// return the requests readField()/writeField()/command()/service() return; a
	// member's read() projects the value. A missing path, a wrong kind or a
	// read-only write throws, naming the path, as the generated device does.
	endpoint(path)
	{
		const token = this.#token;
		if (!this.#current(token))
			throw new Error(`No admitted descriptor to resolve path "${String(path)}"`);
		const found = typeof path === 'string' ? findEndpoint(token.model, path) : undefined;
		if (found === undefined)
			throw new Error(`No endpoint at path "${String(path)}"`);
		const category = kindNames[found.category], row = found.endpoint;
		if (found.member !== '' && found.category !== 0)
			throw new Error(
			    `"${path}" names a member of a ${category}: only a Field's members are readable`);
		const steps =
		    found.category === 0 ? memberSteps(token.model, row.typeId, found.member, path) : [];
		const member = steps.length !== 0;
		const usable = operation => {
			if (!this.#current(token))
				return Object.freeze({status : Status.NotAdmitted});
			return this.bind(operation, row.id);
		};
		const read = timeoutMs => {
			if (found.category !== 0)
				throw new Error(`"${path}" is a ${category}: read() takes a Field`);
			const bound = usable('FieldRead');
			if (bound.status !== Status.Ok)
				return this.#refused('FieldRead', bound.status);
			const pending = this.readField(bound.endpoint, timeoutMs);
			if (!member)
				return pending;
			return this.#map(pending, result => {
				if (result.valueStatus !== ValueStatus.Ok)
					return result;
				const value = steps.reduce((part, step) => part[step], result.value);
				return Object.freeze(
				    {operation : result.operation, valueStatus : ValueStatus.Ok, value});
			});
		};
		const write = (value, timeoutMs) => {
			if (found.category !== 0)
				throw new Error(`"${path}" is a ${category}: write() takes a Field`);
			if (member)
				throw new Error(`"${path}" is a member: write() takes a whole Field`);
			if (!(row.capabilities & 2))
				throw new Error(`"${path}" is a read-only Field`);
			const bound = usable('FieldWrite');
			if (bound.status !== Status.Ok)
				return this.#refused('FieldWrite', bound.status);
			return this.writeField(bound.endpoint, value, timeoutMs);
		};
		const call = (...args) => {
			if (found.category === 0)
				throw new Error(`"${path}" is a Field: call() takes a Command or Service`);
			const operation = found.category === 1 ? 'Command' : 'Service';
			const [value, timeoutMs] = row.requestTypeId === 0 ? [ undefined, args[0] ] : args;
			const bound = usable(operation);
			if (bound.status !== Status.Ok)
				return this.#refused(operation, bound.status);
			return operation === 'Command' ? this.command(bound.endpoint, value, timeoutMs)
			                               : this.service(bound.endpoint, value, timeoutMs);
		};
		return Object.freeze({id : row.id, path, category, member, read, write, call});
	}

	bindReadMany(capability, endpoints)
	{
		return this.#produce(() => this.#bindReadMany(capability, endpoints));
	}

	#bindReadMany(capability, endpoints)
	{
		const gate = this.#gate();
		if (gate !== Status.Ok)
			return Object.freeze({status : gate});
		const token = this.#token, entries = [], fields = [];
		let count;
		try {
			if (!Array.isArray(endpoints))
				return Object.freeze({status : Status.InvalidArgument});
			count = endpoints.length;
			if (!Number.isInteger(count) || count < 1 || count > 32)
				return Object.freeze({status : Status.InvalidArgument});
			for (let i = 0; i < count; ++i) {
				const entry = this.#endpoints.get(endpoints[i]);
				if (!entry || entry.token !== token || !this.#current(token))
					return Object.freeze(
					    {status : this.#gate() === Status.Ok ? Status.NotAdmitted : this.#gate()});
				if (entry.endpoint.operation !== 'FieldRead')
					return Object.freeze({status : Status.InvalidArgument});
				const type = token.model.types[entry.endpoint.responseType];
				const scalar = type.kind === TypeKind.Enum ? token.model.types[type.underlyingTypeId]
			    : type.kind === TypeKind.Scalar ? type : undefined;
				if (!scalar)
					return Object.freeze({status : Status.Unsupported});
				entries.push(entry);
				fields.push(Object.freeze({...entry.binding, readManyScalar : scalar.scalarCode}));
			}
		} catch {
			return Object.freeze(
			    {status : this.#gate() === Status.Ok ? Status.InvalidArgument : this.#gate()});
		}
		const bound = this.#core.bindReadMany(capability, fields);
		if (bound.status !== Status.Ok)
			return Object.freeze({status : bound.status});
		if (!this.#current(token))
			return Object.freeze({status : this.#staleGate()});
		const binding = Object.freeze({session : token.session, count});
		this.#readMany.set(binding, {token, entries, binding : bound.binding});
		return Object.freeze({status : Status.Ok, binding});
	}

	readMany(binding, timeoutMs)
	{
		return this.#produce(() => this.#readSelected(binding, timeoutMs));
	}

	#readSelected(binding, timeoutMs)
	{
		const gate = this.#gate();
		if (gate !== Status.Ok)
			return this.#refused('ReadMany', gate);
		const entry = this.#readMany.get(binding);
		if (!entry || !this.#current(entry.token))
			return this.#refused('ReadMany', Status.NotAdmitted);
		return this.#map(this.#core.readMany(entry.binding, timeoutMs), result => {
			// CodecError here means a checked envelope with unusable values. Keep its outcome.
			const operation =
			    result.requestId !== 0 && result.status === Status.CodecError
			        ? Object.freeze({
				          ...result,
				          status : Status.Ok,
				          succeeded : result.dispatch === 0 && result.endpointStatus === 0,
				          value : undefined
			          })
			        : Object.freeze({...result, value : undefined});
			if (!this.#current(entry.token))
				return Object.freeze(
				    {operation, valueStatus : ValueStatus.Stale, items : undefined});
			if (result.status === Status.CodecError)
				return Object.freeze(
				    {operation, valueStatus : ValueStatus.InvalidPayload, items : undefined});
			if (!result.succeeded || !result.value)
				return Object.freeze(
				    {operation, valueStatus : ValueStatus.NoValue, items : undefined});
			try {
				const items = result.value.map((item, index) => Object.freeze({
					id : item.id,
					status : item.status,
					payload : item.payload,
					valueStatus : item.status === 0 ? ValueStatus.Ok : ValueStatus.NoValue,
					value :
					    item.status === 0
					        ? decodeValue(entry.token.model,
					                      entry.entries[index].endpoint.responseType, item.payload)
					        : undefined
				}));
				return Object.freeze(
				    {operation, valueStatus : ValueStatus.Ok, items : Object.freeze(items)});
			} catch {
				return Object.freeze(
				    {operation, valueStatus : ValueStatus.InvalidPayload, items : undefined});
			}
		});
	}

	batch(capability, requests, timeoutMs)
	{
		return this.#produce(() => this.#batch(capability, requests, timeoutMs));
	}

	#batch(capability, requests, timeoutMs)
	{
		const gate = this.#gate();
		if (gate !== Status.Ok)
			return this.#refused('Batch', gate);
		const token = this.#token, specs = [];
		try {
			if (!Array.isArray(requests))
				return this.#refused('Batch', Status.InvalidArgument);
			const count = requests.length;
			if (!Number.isInteger(count) || count < 1 || count > 32)
				return this.#refused('Batch', Status.InvalidArgument);
			// Exactly the bounded indexed selection is frozen before any Codec/user getter runs.
			const selected = [];
			for (let i = 0; i < count; ++i) {
				const request = requests[i], entry = this.#endpoints.get(request.endpoint);
				if (!entry || entry.token !== token || !this.#current(token))
					return this.#refused('Batch', this.#gate() === Status.Ok ? Status.NotAdmitted
					                                                         : this.#gate());
				selected.push({entry, value : request.value});
			}
			if (!this.#current(token))
				return this.#refused('Batch', this.#staleGate());
			for (const {entry, value} of selected) {
				const {operation, responseType} = entry.endpoint;
				const decode = input => {
					if (!this.#current(token))
						throw new Error('Retired runtime binding');
					return decodeValue(token.model, responseType, input);
				};
				const contract = Object.freeze({...entry.binding, decode, decodeResponse : decode});
				specs.push(operation === 'FieldRead'    ? Batch.read(contract)
				           : operation === 'FieldWrite' ? Batch.write(contract, value)
				           : operation === 'Command'    ? Batch.command(contract, value)
				                                        : Batch.service(contract, value));
			}
		} catch {
			return this.#refused('Batch', this.#gate() === Status.Ok ? Status.InvalidArgument
			                                                         : this.#gate());
		}
		if (!this.#current(token))
			return this.#refused('Batch', this.#staleGate());
		return this.#map(this.#core.batch(capability, Batch.items(...specs), timeoutMs), result => {
			const operation = Object.freeze({...result, value : undefined, items : undefined});
			if (!this.#current(token)) {
				const items = result.items?.map(
				    item => Object.freeze(
				        {...item, valueStatus : ValueStatus.Stale, value : undefined}));
				return Object.freeze({
					operation,
					valueStatus : ValueStatus.Stale,
					items : items && Object.freeze(items)
				});
			}
			return Object.freeze(
			    {operation, valueStatus : result.valueStatus, items : result.items});
		});
	}

	#current(token = this.#token)
	{
		return !!token && token === this.#token && token.active && !this.#closed &&
		       token.session === this.#core.session;
	}

	#gate()
	{
		if (this.#closed)
			return Status.Closed;
		if (this.#core.recoveryRequired)
			return Status.RecoveryRequired;
		return this.#current() && this.#core.ready ? Status.Ok : Status.NotAdmitted;
	}

	#staleGate()
	{
		const status = this.#gate();
		return status === Status.Ok ? Status.NotAdmitted : status;
	}

	#produce(call)
	{
		++this.#producers;
		try {
			return call();
		} finally {
			--this.#producers;
		}
	}

	#refused(operation, status)
	{
		const result = Object.freeze({
			operation : Object.freeze({
				status,
				operation,
				session : this.session,
				requestId : 0,
				dispatch : 0,
				endpointStatus : 0,
				succeeded : false,
				accepted : false,
				executed : false,
				payload : new Uint8Array(0),
				value : undefined
			}),
			valueStatus : this.#current() ? ValueStatus.NoValue : ValueStatus.Stale,
			value : undefined
		});
		return Object.assign(Promise.resolve(result),
		                     {requestId : 0, session : this.session, cancel : () => false});
	}

	#map(pending, project)
	{
		if (pending.requestId)
			++this.#projecting;
		const mapped = pending.then(result => {
			try {
				return project(result);
			} finally {
				if (pending.requestId)
					--this.#projecting;
			}
		});
		return Object.assign(mapped, {
			requestId : pending.requestId,
			session : pending.session,
			cancel : () => pending.cancel()
		});
	}

	#submit(operation, endpoint, value, timeoutMs, originalPayload, preserving = false)
	{
		const gate = this.#gate();
		if (gate !== Status.Ok)
			return this.#refused(operation, gate);
		const entry = this.#endpoints.get(endpoint);
		if (!entry || !this.#current(entry.token))
			return this.#refused(operation, Status.NotAdmitted);
		if (entry.endpoint.operation !== operation)
			return this.#refused(operation, Status.InvalidArgument);
		const binding = preserving ? {
			...entry.binding,
			encode : request => {
			    if (!this.#current(entry.token))
				    throw new Error('Retired runtime binding');
			    return encodeValuePreserving(entry.token.model, entry.endpoint.requestType, request,
			                                 originalPayload);
			}
		}
		                           : entry.binding;
		const pending = operation === 'FieldRead'    ? this.#core.read(entry.binding, timeoutMs)
		                : operation === 'FieldWrite' ? this.#core.write(binding, value, timeoutMs)
		                : operation === 'Command'
		                    ? this.#core.command(entry.binding, value, timeoutMs)
		                    : this.#core.service(entry.binding, value, timeoutMs);
		return this.#map(pending, result => {
			if (!this.#current(entry.token))
				return Object.freeze(
				    {operation : result, valueStatus : ValueStatus.Stale, value : undefined});
			if (!result.succeeded || entry.endpoint.responseType === 0)
				return Object.freeze(
				    {operation : result, valueStatus : ValueStatus.NoValue, value : undefined});
			try {
				const value =
				    decodeValue(entry.token.model, entry.endpoint.responseType, result.payload);
				return Object.freeze({operation : result, valueStatus : ValueStatus.Ok, value});
			} catch {
				return Object.freeze({
					operation : result,
					valueStatus : ValueStatus.InvalidPayload,
					value : undefined
				});
			}
		});
	}
}
