/* One-owner asynchronous FIFO client. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
export const Status = Object.freeze({
	Ok : 0,
	QueueFull : 1,
	PayloadFull : 2,
	NotAdmitted : 3,
	ReadOnly : 4,
	Closed : 5,
	Cancelled : 6,
	Timeout : 7,
	SessionLost : 8,
	OutcomeUnknown : 9,
	MalformedReply : 10,
	TransportError : 11,
	CodecError : 12,
	BufferTooSmall : 13,
	InvalidArgument : 14,
	Busy : 15,
	RecoveryRequired : 16,
	IdsExhausted : 17,
	Unsupported : 18,
} as const);
export type ClientStatus = typeof Status[keyof typeof Status];
export type Operation = 'FieldWrite'|'Command'|'Service'|'FieldRead'|'ReadMany'|'Batch';
export const Operations = Object.freeze(
    {FieldWrite : 1, Command : 2, Service : 3, FieldRead : 4, ReadMany : 5, Batch : 6} as const);
export type ByteInput = ArrayBuffer|ArrayBufferView;
export interface ReadManyCapability {
	readonly itemLimit: number;
	readonly requestLimit: number;
	readonly replyLimit: number;
	readonly session: number;
	readonly fingerprint: bigint;
}

// Pure inspection for host planning. This never admits a session or bypasses
// bindReadMany(), which still checks identity, field types and byte budgets.
export function inspectReadManyCapability(input: ByteInput): ReadManyCapability|undefined
{
	try {
		const record = new Uint8Array(bytes(input));
		if (record.length !== 24)
			return undefined;
		const cap = new DataView(record.buffer);
		const itemLimit = record[5], requestLimit = cap.getUint16(8, true),
		      replyLimit = cap.getUint16(10, true), session = cap.getUint32(12, true);
		if (cap.getUint32(0, true) !== 0x314d5254 || record[4] !== 1 || record[6] !== 1 ||
		    record[7] !== 1 || itemLimit < 1 || itemLimit > 32 || requestLimit < 6 ||
		    requestLimit > 130 || replyLimit < 10 || replyLimit > 514 || session === 0)
			return undefined;
		return Object.freeze({
			itemLimit,
			requestLimit,
			replyLimit,
			session,
			fingerprint : cap.getBigUint64(16, true)
		});
	} catch {
		return undefined;
	}
}

export interface ReadBinding<T> {
	readonly fingerprint: bigint;
	readonly operation: 'FieldRead';
	readonly id: number;
	readonly requestBytes: number;
	readonly responseBytes: number;
	// Explicit scalar/open-enum underlying ScalarCode; absent on legacy/aggregate bindings.
	readonly readManyScalar?: ReadManyScalar;
	decode(input: ByteInput): T;
}
export type ReadManyScalar = 1|2|3|4|5|6|7|8|9|10|11;
export interface ReadManyReadBinding<T> extends ReadBinding<T> {
	readonly readManyScalar: ReadManyScalar;
}
export type ReadManyField<T> = ReadManyReadBinding<T>|{readonly read : ReadManyReadBinding<T>};
export const ReadManyStatus = Object.freeze(
    {Ok : 0, NotFound : 1, UnsupportedType : 2, Unavailable : 3, ReadError : 4} as const);
export type ReadManyItem<T> = {
	readonly id: number; readonly status : 0; readonly value : T; readonly payload : Uint8Array
}|
{
	readonly id: number;
	readonly status: 1|2|3|4;
	readonly value?: never;
	readonly payload: Uint8Array
};
type ReadManyValue<F> = F extends {readonly read : ReadBinding<infer T>}
                                      ? T
                                      : F extends ReadBinding<infer T>? T : never;
export type ReadManyValues<F extends readonly ReadManyField<unknown>[]> = {
	readonly[K in keyof F]: ReadManyItem<ReadManyValue<F[K]>>;
};
declare const readManyTypes: unique symbol;
export interface ReadManyBinding<T extends readonly unknown[]> {
	readonly session: number;
	readonly fingerprint: bigint;
	readonly count: number;
	readonly[readManyTypes]: T;
}
export type ReadManyBindingResult<T extends readonly unknown[]> = {
	readonly status: typeof Status.Ok; readonly binding : ReadManyBinding<T>
}|
{
	readonly status: Exclude<ClientStatus, typeof Status.Ok>;
	readonly binding?: never
};
interface ReadManyEntry {
	readonly id: number;
	readonly size: number;
	readonly scalar: ReadManyScalar;
	readonly decode: (input: ByteInput) => unknown;
}
interface ReadManySnapshot {
	readonly session: number;
	readonly fingerprint: bigint;
	readonly payload: Uint8Array;
	readonly responseBytes: number;
	readonly entries: readonly ReadManyEntry[];
}
export interface WriteBinding<T> {
	readonly fingerprint: bigint;
	readonly operation: 'FieldWrite';
	readonly id: number;
	readonly requestBytes: number;
	readonly responseBytes: number;
	encode(value: T): Uint8Array;
}
export interface CommandBinding<T> {
	readonly fingerprint: bigint;
	readonly operation: 'Command';
	readonly id: number;
	readonly requestBytes: number;
	readonly responseBytes: number;
	encode(value: T): Uint8Array;
}
export interface ServiceBinding<Q, R> {
	readonly fingerprint: bigint;
	readonly operation: 'Service';
	readonly id: number;
	readonly requestBytes: number;
	readonly responseBytes: number;
	encode(value: Q): Uint8Array;
	decodeResponse(input: ByteInput): R;
}
export interface Field<T> {
	readonly read: ReadBinding<T>;
	readonly write?: WriteBinding<T>;
}
export type BatchOperation = 'FieldRead'|'FieldWrite'|'Command'|'Service';
export const BatchValueStatus = Object.freeze(
    {Ok : 'Ok', NoValue : 'NoValue', InvalidPayload : 'InvalidPayload', Stale : 'Stale'} as const);
export type BatchValueStatus = typeof BatchValueStatus[keyof typeof BatchValueStatus];
declare const batchTypes: unique symbol;
export interface BatchItem<T> {
	readonly operation: BatchOperation;
	readonly[batchTypes]: T;
}
export type BatchItemResult<T> = Omit<Result<T>, 'value'>&{readonly id : number}&({
	readonly valueStatus: typeof BatchValueStatus.Ok;
	readonly value: T
}|{
	readonly valueStatus: Exclude<BatchValueStatus, typeof BatchValueStatus.Ok>;
	readonly value: undefined
});
export type BatchItems<I extends readonly BatchItem<unknown>[]> = {
	readonly[K in keyof I]: BatchItemResult<I[K] extends BatchItem<infer T>? T : never>;
};
type BatchRequest<B extends {encode : (value: never) => Uint8Array}> = Parameters<B['encode']>[ 0 ];
type BatchArguments<B extends {encode : (value: never) => Uint8Array}> =
    [ BatchRequest<B>] extends [void] ? [ value?: BatchRequest<B>] : [ value: BatchRequest<B>];
export interface BatchResult<T extends readonly unknown[]> extends Result<undefined> {
	readonly items: T|undefined;
	readonly valueStatus: BatchValueStatus;
}
export interface BatchPromise<T extends readonly unknown[]> extends Promise<BatchResult<T>> {
	readonly requestId: number;
	readonly session: number;
	cancel(): boolean;
}
interface BatchArgument {
	readonly operation: BatchOperation;
	readonly binding: unknown;
	readonly argument: unknown;
}
const batchArguments = new WeakMap<object, BatchArgument>();

function batchItem<T>(operation: BatchOperation, binding: unknown, argument?: unknown): BatchItem<T>
{
	const item = Object.freeze({operation}) as BatchItem<T>;
	batchArguments.set(item, {operation, binding, argument});
	return item;
}

// Pure argument selectors. Encoding/ownership starts only inside DeviceClient.batch().
export const Batch = Object.freeze({
	items<const I extends readonly BatchItem<unknown>[]>(...items: I) : I {
	    return Object.freeze(items) as I;
	},
	read<T>(field: Field<T>|ReadBinding<T>) : BatchItem<T> {
	    return batchItem('FieldRead', field);
	},
	write<T>(field: {readonly write: WriteBinding<T>}|WriteBinding<T>, value: NoInfer<T>) :
	    BatchItem<undefined> {
	        return batchItem('FieldWrite', field, value);
	    },
	command<const B extends CommandBinding<never>>(
	    binding: B, ...value: BatchArguments<NoInfer<B>>) : BatchItem<undefined> {
	    return batchItem('Command', binding, value[0]);
	},
	service<const B extends ServiceBinding<never, unknown>>(binding: B,
	                                                        ...value: BatchArguments<NoInfer<B>>) :
	    BatchItem<ReturnType<B['decodeResponse']>> {
	        return batchItem('Service', binding, value[0]);
	    }
});
interface BatchEntry {
	readonly operation: BatchOperation;
	readonly id: number;
	readonly requestBytes: number;
	readonly responseBytes: number;
	readonly encode?: (value: never) => Uint8Array;
	readonly decode?: (input: ByteInput) => unknown;
	readonly voidResponse: boolean;
}
interface BatchMetadata {
	fingerprint: bigint;
	operation: BatchOperation;
	id: number;
	requestBytes: number;
	responseBytes: number;
	responseTypeId?: number;
	encode?: (value: never) => Uint8Array;
	decode?: (input: ByteInput) => unknown;
	decodeResponse?: (input: ByteInput) => unknown;
}
interface BatchState {
	readonly session: number;
	readonly fingerprint: bigint;
	readonly startedAt: number;
	readonly payload: Uint8Array;
	readonly responseBytes: number;
	readonly entries: readonly BatchEntry[];
	requestId: number;
	checked?: WireResult;
	items?: readonly BatchItemResult<unknown>[];
	valueStatus: BatchValueStatus;
}
export interface Result<T = undefined> {
	readonly status: ClientStatus;
	readonly operation: Operation;
	readonly session: number;
	readonly requestId: number;
	readonly dispatch: number;
	readonly endpointStatus: number;
	readonly succeeded: boolean;
	readonly accepted: boolean;
	readonly executed: boolean;
	readonly payload: Uint8Array;
	readonly value: T|undefined;
}
// Normal refusals and operational failures resolve a Result; they do not reject.
export interface RequestPromise<T> extends Promise<Result<T>> {
	readonly requestId: number;
	readonly session: number;
	cancel(): boolean;
}
export interface Clock {
	now(): number;
	setTimeout(callback: () => void, milliseconds: number): unknown;
	clearTimeout(handle: unknown): void;
}
const systemClock: Clock = {
	now : () => performance.now(),
	setTimeout : (callback, milliseconds) => setTimeout(callback, milliseconds),
	clearTimeout : handle => clearTimeout(handle as ReturnType<typeof setTimeout>),
};
const bytes = (input: ByteInput): Uint8Array =>
    input instanceof ArrayBuffer ? new Uint8Array(input)
                                 : new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
const u32 = (n: number): boolean => Number.isInteger(n) && n >= 0 && n <= 0xffffffff;
const typedArrayLength =
    Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), 'length')!.get!;

function encodedSnapshot(input: unknown, expected: number): Uint8Array
{
	if (!(input instanceof Uint8Array) || typedArrayLength.call(input) !== expected)
		throw new BackendFailure(Status.CodecError, 'Exact encoded request length');
	return new Uint8Array(input);
}

const validOperation = (operation: string): operation is Operation =>
    Object.hasOwn(Operations, operation);
export interface WireRequest {
	readonly operation: Operation;
	readonly endpointId: number;
	readonly session: number;
	readonly requestId: number;
	readonly payload: Uint8Array;
	readonly responseBytes: number;
}
export interface WireResult {
	readonly dispatch: number;
	readonly endpointStatus: number;
	readonly payload: Uint8Array;
	// A checked outcome can precede physical release. Resolve/reject both mean quiescent,
	// without changing the known endpoint outcome. Backend cleanup must precede settlement.
	readonly released?: Promise<void>;
}
export interface ExchangeContext {
	readonly session: number;
	readonly requestId: number;
	readonly signal: AbortSignal;
}
export type Exchange = (packet: Uint8Array, context?: ExchangeContext) =>
    ByteInput|Promise<ByteInput>;
export interface BackendContext extends ExchangeContext {
	readonly clock: Clock;
	// Immediately before the first possible external transfer. False means do not send.
	handoff(mayExecute: boolean): boolean;
	// Explicit admission revocation after a prior authorized execution-capable handoff.
	// A denied new handoff then preserves passive correlation until a terminal event/abort.
	admissionRevoked?(): boolean;
}
export interface Backend {
	// Pure synchronous opt-in for optional operations; legacy operations do not need a hook.
	supports?(operation: Operation): boolean;
	// Settlement releases physical I/O, unless a checked result supplies released.
	execute(request: WireRequest, context: BackendContext): Promise<WireResult>;
	// Application asserts this aborted session's transport has actually closed/drained.
	confirmSessionClosed?(generation: number): boolean;
}

class BackendFailure extends Error {
	constructor(readonly status: ClientStatus, message: string)
	{
		super(message);
	}
}

function checkWireResult(request: WireRequest, result: WireResult): void
{
	const {dispatch, endpointStatus, payload} = result;
	if (![0, 4, 5, 6, 7, 8, 9].includes(dispatch) || !u32(endpointStatus) ||
	    (dispatch !== 0 && (endpointStatus !== 0 || payload.length !== 0)))
		throw new BackendFailure(Status.MalformedReply, 'Invalid dispatch/status envelope');
	if (dispatch !== 0)
		return;
	if (request.operation === 'ReadMany') {
		if (request.endpointId !== 0 || endpointStatus !== 0 || request.responseBytes < 10 ||
		    request.responseBytes > 514 || payload.length < 2 ||
		    payload.length > request.responseBytes)
			throw new BackendFailure(Status.MalformedReply, 'Invalid ReadMany envelope');
		return;
	}
	if (request.operation === 'Batch') {
		if (request.endpointId !== 0 || endpointStatus !== 0 || request.responseBytes < 14 ||
		    request.responseBytes > 65535 || payload.length < 14 ||
		    payload.length > request.responseBytes)
			throw new BackendFailure(Status.MalformedReply, 'Invalid Batch envelope');
		return;
	}
	const maximum = {FieldRead : 0, FieldWrite : 5, Command : 7, Service : 4}[request.operation];
	const expected = endpointStatus === 0 &&
	                         (request.operation === 'FieldRead' || request.operation === 'Service')
	                     ? request.responseBytes
	                     : 0;
	if (endpointStatus > maximum || payload.length !== expected)
		throw new BackendFailure(Status.MalformedReply,
		                         'Invalid endpoint status or payload length');
}

export interface ClientOptions {
	readonly backend: Backend;
	readonly matchesHeader: (header: ByteInput) => boolean;
	readonly maxPending?: number;
	readonly maxPayloadBytes?: number;
	readonly timeoutMs?: number;
	readonly clock?: Clock;
}
interface Pending {
	request: WireRequest;
	deadline: number;
	resolve: (result: Result<unknown>) => void;
	decode?: (input: ByteInput) => unknown;
	controller: AbortController;
	possibleExecution: boolean;
	checkedReadMany?: WireResult;
	batch?: BatchState;
	done: boolean;
}

export class DeviceClient {
	private readonly backend: Backend;
	private readonly identity: (header: ByteInput) => boolean;
	private readonly clock: Clock;
	private readonly limit: number;
	private readonly byteLimit: number;
	private readonly timeout: number;
	private pending: Pending[] = [];
	private active?: Pending;
	private invocation?: Pending;
	private epoch = 0;
	private fingerprint = 0n;
	private lastEpoch = 0;
	private nextId = 1;
	private payloadBytes = 0;
	private recoveryEpoch = 0;
	private stopped = false;
	private admitted = false;
	private revoked = false;
	private timer?: unknown;
	private scheduled = false;
	private pumping = false;
	private confirming = false;
	private readonly readManyBindings = new WeakMap<object, ReadManySnapshot>();

	constructor(options: ClientOptions)
	{
		this.backend = options.backend;
		this.identity = options.matchesHeader;
		this.clock = options.clock ?? systemClock;
		this.limit = options.maxPending ?? 64;
		this.byteLimit = options.maxPayloadBytes ?? 1048576;
		this.timeout = options.timeoutMs ?? 5000;
		if (!Number.isSafeInteger(this.limit) || this.limit < 1 ||
		    !Number.isSafeInteger(this.byteLimit) || this.byteLimit < 0 ||
		    !Number.isFinite(this.timeout) || this.timeout <= 0 || this.timeout > 0x7fffffff)
			throw new RangeError('Client capacity/deadline');
	}

	get session(): number
	{
		return this.epoch;
	}

	get ready(): boolean
	{
		return this.admitted && !this.stopped && this.recoveryEpoch === 0;
	}

	get recoveryRequired(): boolean
	{
		return this.recoveryEpoch !== 0;
	}

	get recoveryGeneration(): number
	{
		return this.recoveryEpoch;
	}

	get pendingCount(): number
	{
		return this.pending.length;
	}

	get snapshotBytes(): number
	{
		return this.payloadBytes;
	}

	get drained(): boolean
	{
		return this.stopped && this.pending.length === 0 && !this.invocation;
	}

	// Caller opens a fresh physical session and supplies its admitted descriptor header.
	openSession(generation: number, header: ByteInput): boolean
	{
		if (this.stopped || this.confirming || this.recoveryRequired || this.pending.length ||
		    this.invocation || !u32(generation) || generation === 0 || generation <= this.lastEpoch)
			return false;
		let matches = false;
		let declared: Uint8Array;
		try {
			declared = bytes(header).slice();
			matches = declared.length === 64 && this.identity(declared);
		} catch {
			return false;
		}
		// The identity callback may synchronously close, admit or enqueue on this client.
		if (!matches || this.stopped || this.confirming || this.recoveryRequired ||
		    this.pending.length || this.invocation || generation <= this.lastEpoch)
			return false;
		this.fingerprint = new DataView(declared!.buffer).getBigUint64(16, true);
		this.epoch = this.lastEpoch = generation;
		this.nextId = 1;
		this.admitted = true;
		this.revoked = false;
		return true;
	}

	// Assertion by the application: old transport and remote retained session are closed/drained.
	// Re-admitting a header alone never makes an uncertain old session safe.
	confirmSessionClosed(generation: number): boolean
	{
		if (this.confirming || this.admitted || !this.epoch || generation !== this.epoch ||
		    this.pending.length ||
		    (this.invocation && (this.invocation.request.session !== generation ||
		                         !this.invocation.controller.signal.aborted)))
			return false;
		this.confirming = true;
		try {
			const confirm = this.backend.confirmSessionClosed;
			if (confirm) {
				const confirmed: unknown = confirm.call(this.backend, generation);
				if (confirmed !== true) {
					// Refuse async/truthy hooks; observe a rejected async hook without trusting it.
					Promise.resolve(confirmed).catch(() => {
					                                 });
					return false;
				}
			}
		} catch {
			return false;
		} finally {
			this.confirming = false;
		}
		this.invocation = undefined;
		this.recoveryEpoch = 0;
		return true;
	}

	resetSession(): void
	{
		this.admitted = false;
		this.cancelAll(Status.SessionLost);
	}

	// Revoke future execution without cancelling a possibly executed active request.
	// Application-owned physical/remote cleanup is still required before readmission.
	invalidateAdmission(): void
	{
		this.revoked = true;
		this.admitted = false;
		for (const pending of this.pending.slice()) {
			if (pending !== this.active || !pending.possibleExecution)
				this.finish(pending, Status.Cancelled);
		}
	}

	close(): void
	{
		this.stopped = true;
		this.admitted = false;
		this.cancelAll(Status.Cancelled);
	}

	read<T>(field: Field<T>|ReadBinding<T>, timeoutMs?: number): RequestPromise<T>
	{
		const binding = 'read' in field ? field.read : field;
		return this.enqueue(binding, undefined, binding.decode, timeoutMs);
	}

	write<T>(field: Field<T>|WriteBinding<T>, value: T,
	         timeoutMs?: number): RequestPromise<undefined>
	{
		const binding = 'read' in field ? field.write : field;
		if (!binding)
			return this.refused('FieldWrite', Status.ReadOnly);
		return this.enqueue(binding, value, undefined, timeoutMs);
	}

	command<T>(binding: CommandBinding<T>, value: T, timeoutMs?: number): RequestPromise<undefined>
	{
		return this.enqueue(binding, value, undefined, timeoutMs);
	}

	service<Q, R>(binding: ServiceBinding<Q, R>, value: Q, timeoutMs?: number): RequestPromise<R>
	{
		return this.enqueue(binding, value, binding.decodeResponse, timeoutMs);
	}

	// Discover TRM1 outside this client, then cache this binding for the admitted session.
	bindReadMany<const F extends readonly ReadManyField<unknown>[]>(capability: ByteInput,
	                                                                fields: F):
	    ReadManyBindingResult<ReadManyValues<F>>
	{
		const refused = (status: ClientStatus): ReadManyBindingResult<ReadManyValues<F>> =>
		    ({status : status as Exclude<ClientStatus, typeof Status.Ok>});
		const gate = this.optionalGate();
		if (gate !== Status.Ok)
			return refused(gate);
		const session = this.epoch, fingerprint = this.fingerprint;
		let entries: ReadManyEntry[], payload: Uint8Array, responseBytes = 2;
		try {
			const cap = inspectReadManyCapability(capability);
			if (!cap)
				return refused(Status.InvalidArgument);
			const {itemLimit, requestLimit, replyLimit} = cap;
			if (cap.session !== session || cap.fingerprint !== fingerprint)
				return refused(Status.NotAdmitted);
			if (!Array.isArray(fields))
				return refused(Status.InvalidArgument);
			const count = fields.length;
			if (!Number.isInteger(count) || count < 1 || count > itemLimit)
				return refused(Status.InvalidArgument);
			if (2 + 4 * count > requestLimit)
				return refused(Status.BufferTooSmall);
			// Snapshot exactly the validated indices, never an overridable iterator/species.
			const selected: ReadManyField<unknown>[] = [];
			for (let i = 0; i < count; ++i)
				selected.push(fields[i]);
			entries = [];
			const sizes = [ 0, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8 ];
			for (const field of selected) {
				const read = 'read' in field ? field.read : field;
				const {
					id,
					operation,
					requestBytes,
					responseBytes : size,
					readManyScalar : scalar,
					decode,
					fingerprint : identity
				} = read;
				if (identity !== fingerprint)
					return refused(Status.NotAdmitted);
				if (operation !== 'FieldRead' || !u32(id) || requestBytes !== 0 ||
				    typeof decode !== 'function' || entries.some(entry => entry.id === id))
					return refused(Status.InvalidArgument);
				if (!Number.isInteger(scalar) || scalar < 1 || scalar > 11 ||
				    size !== sizes[scalar])
					return refused(Status.Unsupported);
				entries.push(Object.freeze({id, size, scalar, decode}));
				responseBytes += 8 + size;
			}
			if (responseBytes > replyLimit)
				return refused(Status.BufferTooSmall);
			payload = new Uint8Array(2 + 4 * entries.length);
			const request = new DataView(payload.buffer);
			request.setUint16(0, entries.length, true);
			entries.forEach((entry, i) => request.setUint32(2 + 4 * i, entry.id, true));
		} catch {
			return refused(Status.InvalidArgument);
		}
		// Property getters and the backend support hook can re-enter. Never publish across admission.
		const finalGate = this.optionalGate();
		if (finalGate !== Status.Ok || this.epoch !== session || this.fingerprint !== fingerprint)
			return refused(finalGate === Status.Ok ? Status.NotAdmitted : finalGate);
		const binding = Object.freeze({session, fingerprint, count : entries.length}) as
		                ReadManyBinding<ReadManyValues<F>>;
		this.readManyBindings.set(
		    binding,
		    Object.freeze(
		        {session, fingerprint, payload, responseBytes, entries : Object.freeze(entries)}));
		return Object.freeze({status : Status.Ok, binding});
	}

	readMany<T extends readonly unknown[]>(binding: ReadManyBinding<T>,
	                                       timeoutMs?: number): RequestPromise<T>
	{
		const gate = this.optionalGate();
		if (gate !== Status.Ok)
			return this.refused('ReadMany', gate);
		const snapshot = this.readManyBindings.get(binding);
		if (!snapshot || snapshot.session !== this.epoch ||
		    snapshot.fingerprint !== this.fingerprint)
			return this.refused('ReadMany', Status.NotAdmitted);
		return this.enqueue({
			fingerprint : snapshot.fingerprint,
			operation : 'ReadMany',
			id : 0,
			requestBytes : snapshot.payload.length,
			responseBytes : snapshot.responseBytes,
			encode : () => snapshot.payload
		},
		                    undefined,
		                    input => this.decodeReadMany(snapshot, input) as unknown as T,
		                    timeoutMs, snapshot);
	}

	batch<const I extends readonly BatchItem<unknown>[]>(capability: ByteInput, items: I,
	                                                     timeoutMs
	                                                     ?: number): BatchPromise<BatchItems<I>>
	{
		const refused = (status: ClientStatus) => this.refusedBatch<BatchItems<I>>(status);
		const gate = this.optionalGate('Batch');
		if (gate !== Status.Ok)
			return refused(gate);
		if (this.pending.length >= this.limit)
			return refused(Status.QueueFull);
		if (this.nextId > 0xffffffff)
			return refused(Status.IdsExhausted);
		const session = this.epoch, fingerprint = this.fingerprint, startedAt = this.clock.now();
		if (!this.ready || this.epoch !== session || this.fingerprint !== fingerprint)
			return refused(this.stopped            ? Status.Closed
			               : this.recoveryRequired ? Status.RecoveryRequired
			                                       : Status.NotAdmitted);
		const timeout = timeoutMs ?? this.timeout;
		if (!Number.isFinite(timeout) || timeout <= 0 || timeout > 0x7fffffff)
			return refused(Status.InvalidArgument);
		let entries: BatchEntry[], payload: Uint8Array, responseBytes = 2;
		try {
			const record = new Uint8Array(bytes(capability));
			if (record.length !== 24)
				return refused(Status.InvalidArgument);
			const cap = new DataView(record.buffer), countLimit = record[5];
			const requestLimit = cap.getUint16(8, true), replyLimit = cap.getUint16(10, true);
			if (cap.getUint32(0, true) !== 0x314d4254 || record[4] !== 1 || record[6] !== 1 ||
			    record[7] !== 1 || countLimit < 1 || countLimit > 32 || requestLimit < 10 ||
			    replyLimit < 14 || cap.getUint32(12, true) === 0)
				return refused(Status.InvalidArgument);
			if (cap.getUint32(12, true) !== session || cap.getBigUint64(16, true) !== fingerprint)
				return refused(Status.NotAdmitted);
			if (!Array.isArray(items))
				return refused(Status.InvalidArgument);
			const count = items.length;
			if (!Number.isInteger(count) || count < 1 || count > countLimit)
				return refused(Status.InvalidArgument);
			const selected: BatchArgument[] = [];
			for (let i = 0; i < count; ++i) {
				const argument = batchArguments.get(items[i]);
				if (!argument)
					return refused(Status.InvalidArgument);
				selected.push(argument);
			}
			entries = [];
			let requestBytes = 2;
			for (const selectedItem of selected) {
				let metadata = selectedItem.binding as object;
				if (!metadata || (typeof metadata !== 'object' && typeof metadata !== 'function'))
					return refused(Status.InvalidArgument);
				if (selectedItem.operation === 'FieldRead' && 'read' in metadata)
					metadata = (metadata as Field<unknown>).read;
				if (selectedItem.operation === 'FieldWrite' &&
				    ('read' in metadata || 'write' in metadata)) {
					metadata = (metadata as Field<unknown>).write!;
					if (!metadata)
						return refused(Status.ReadOnly);
				}
				const {
					operation,
					id,
					fingerprint : identity,
					requestBytes : requestSize,
					responseBytes : responseSize,
					encode,
					decode,
					decodeResponse,
					responseTypeId
				} = metadata as BatchMetadata;
				if (identity !== fingerprint)
					return refused(Status.NotAdmitted);
				if (operation !== selectedItem.operation || !u32(id) || !u32(requestSize) ||
				    !u32(responseSize) || (responseTypeId !== undefined && !u32(responseTypeId)) ||
				    (operation === 'FieldRead' ? requestSize !== 0 || typeof decode !== 'function'
				                               : typeof encode !== 'function') ||
				    (operation === 'Service' && typeof decodeResponse !== 'function') ||
				    ((operation === 'FieldWrite' || operation === 'Command') &&
				     responseSize !== 0) ||
				    (operation === 'Service' && responseTypeId === 0 && responseSize !== 0))
					return refused(Status.InvalidArgument);
				requestBytes += 8 + requestSize;
				responseBytes += 12 + responseSize;
				if (requestBytes > requestLimit || responseBytes > replyLimit)
					return refused(Status.BufferTooSmall);
				entries.push(Object.freeze({
					operation,
					id,
					requestBytes : requestSize,
					responseBytes : responseSize,
					encode,
					decode : operation === 'Service'     ? decodeResponse
					         : operation === 'FieldRead' ? decode
					                                     : undefined,
					voidResponse : operation === 'FieldWrite' || operation === 'Command' ||
					                   (operation === 'Service' && responseTypeId === 0)
				}));
			}
			if (requestBytes > this.byteLimit - this.payloadBytes)
				return refused(Status.PayloadFull);
			const ownershipGate = this.optionalGate('Batch');
			if (ownershipGate !== Status.Ok || this.epoch !== session ||
			    this.fingerprint !== fingerprint)
				return refused(ownershipGate === Status.Ok ? Status.NotAdmitted : ownershipGate);
			if (this.pending.length >= this.limit)
				return refused(Status.QueueFull);
			if (this.nextId > 0xffffffff)
				return refused(Status.IdsExhausted);
			if (requestBytes > this.byteLimit - this.payloadBytes)
				return refused(Status.PayloadFull);
			payload = new Uint8Array(requestBytes);
			const output = new DataView(payload.buffer);
			output.setUint16(0, count, true);
			let cursor = 2;
			for (let i = 0; i < count; ++i) {
				const entry = entries[i];
				const value = entry.operation === 'FieldRead'
				                  ? new Uint8Array(0)
				                  : encodedSnapshot(entry.encode!(selected[i].argument as never),
				                                    entry.requestBytes);
				output.setUint32(cursor, entry.id, true);
				payload[cursor + 4] = Operations[entry.operation];
				output.setUint16(cursor + 6, value.length, true);
				payload.set(value, cursor + 8);
				cursor += 8 + value.length;
				const current = this.optionalGate('Batch');
				if (current !== Status.Ok || this.epoch !== session ||
				    this.fingerprint !== fingerprint)
					return refused(current === Status.Ok ? Status.NotAdmitted : current);
			}
		} catch {
			return refused(Status.CodecError);
		}
		const finalGate = this.optionalGate('Batch');
		if (finalGate !== Status.Ok || this.epoch !== session || this.fingerprint !== fingerprint)
			return refused(finalGate === Status.Ok ? Status.NotAdmitted : finalGate);
		const state: BatchState = {
			session,
			fingerprint,
			startedAt,
			payload,
			responseBytes,
			entries : Object.freeze(entries),
			requestId : 0,
			valueStatus : BatchValueStatus.NoValue
		};
		return this.enqueue({
			fingerprint,
			operation : 'Batch',
			id : 0,
			requestBytes : payload.length,
			responseBytes,
			encode : () => payload
		},
		                    undefined, input => this.decodeBatch(state, input), timeoutMs,
		                    undefined, state) as unknown as BatchPromise<BatchItems<I>>;
	}

	private refusedBatch<T extends readonly unknown[]>(status: ClientStatus): BatchPromise<T>
	{
		const result = this.makeBatchResult(this.makeResult('Batch', this.epoch, 0, status));
		return Object.assign(Promise.resolve(result as BatchResult<T>),
		                     {requestId : 0, session : this.epoch, cancel : () => false});
	}

	private decodeBatch(state: BatchState, input: ByteInput): undefined
	{
		state.valueStatus = BatchValueStatus.InvalidPayload;
		const payload = new Uint8Array(bytes(input)), data = new DataView(payload.buffer);
		if (payload.length < 2 || data.getUint16(0, true) !== state.entries.length)
			return;
		const rows: BatchItemResult<unknown>[] = [];
		let cursor = 2;
		for (const entry of state.entries) {
			if (payload.length - cursor < 12)
				return;
			const id = data.getUint32(cursor, true), operation = payload[cursor + 4],
			      dispatch = payload[cursor + 5], endpointStatus = payload[cursor + 6],
			      length = data.getUint16(cursor + 8, true);
			if (id !== entry.id || operation !== Operations[entry.operation] ||
			    payload[cursor + 7] !== 0 || payload[cursor + 10] !== 0 ||
			    payload[cursor + 11] !== 0 || ![0, 4, 5, 6, 7, 8, 9].includes(dispatch))
				return;
			const maximum = {FieldRead : 0,
				             FieldWrite : 5,
				             Command : 7,
				             Service : 4}[entry.operation];
			const successful =
			    dispatch === 0 &&
			    (endpointStatus === 0 || (entry.operation === 'Command' && endpointStatus === 1));
			const hasPayload =
			    successful && (entry.operation === 'FieldRead' || entry.operation === 'Service');
			if ((dispatch !== 0 && endpointStatus !== 0) || endpointStatus > maximum ||
			    length !== (hasPayload ? entry.responseBytes : 0))
				return;
			cursor += 12;
			if (length > payload.length - cursor)
				return;
			const operationResult = this.makeResult(
			    entry.operation, state.session, state.requestId, Status.Ok,
			    {dispatch, endpointStatus, payload : payload.subarray(cursor, cursor + length)});
			rows.push(Object.freeze({
				...operationResult,
				id,
				value : undefined,
				valueStatus : successful && entry.decode && !entry.voidResponse
				                  ? BatchValueStatus.InvalidPayload
				                  : BatchValueStatus.NoValue
			}));
			cursor += length;
		}
		if (cursor !== payload.length)
			return;
		// All operation outcomes are owned/checked before any user decoder may re-enter.
		state.items = Object.freeze(rows);
		const current = () =>
		    this.ready && this.epoch === state.session && this.fingerprint === state.fingerprint;
		const values: unknown[] = [];
		for (let i = 0; i < rows.length; ++i) {
			if (!current()) {
				state.valueStatus = BatchValueStatus.Stale;
				return;
			}
			const entry = state.entries[i], row = rows[i];
			if (row.succeeded && entry.decode) {
				try {
					const value = entry.decode(new Uint8Array(row.payload));
					if (!entry.voidResponse)
						values[i] = value;
				} catch {
					return;
				}
			}
			if (!current()) {
				state.valueStatus = BatchValueStatus.Stale;
				return;
			}
		}
		state.items = Object.freeze(rows.map((row, i) => Object.freeze({
			...row,
			valueStatus : values[i] === undefined ? BatchValueStatus.NoValue : BatchValueStatus.Ok,
			value : values[i]
		})) as BatchItemResult<unknown>[]);
		state.valueStatus = BatchValueStatus.Ok;
		return;
	}

	private makeBatchResult(operation: Result<unknown>,
	                        state?: BatchState): BatchResult<readonly BatchItemResult<unknown>[]>
	{
		let valueStatus = state?.valueStatus ?? BatchValueStatus.NoValue, items = state?.items;
		if (state?.checked &&
		    (!this.ready || state.session !== this.epoch || state.fingerprint !== this.fingerprint))
			valueStatus = BatchValueStatus.Stale;
		if (items && valueStatus !== BatchValueStatus.Ok) {
			items = Object.freeze(items.map((row, i) => Object.freeze({
				...row,
				value : undefined,
				valueStatus :
				    row.succeeded && state!.entries[i].decode && !state!.entries[i].voidResponse
				        ? valueStatus
				        : BatchValueStatus.NoValue
			}))) as readonly BatchItemResult<unknown>[];
		}
		return Object.freeze({...operation, value : undefined, valueStatus, items});
	}

	private optionalGate(operation: 'ReadMany'|'Batch' = 'ReadMany'): ClientStatus
	{
		if (this.stopped)
			return Status.Closed;
		if (this.recoveryRequired)
			return Status.RecoveryRequired;
		if (!this.ready)
			return Status.NotAdmitted;
		const session = this.epoch;
		let supported = false;
		try {
			const answer: unknown = this.backend.supports?.(operation);
			supported = answer === true;
			// Invalid asynchronous hooks are refused; still observe their rejected promises.
			if (answer !== null && (typeof answer === 'object' || typeof answer === 'function'))
				Promise.resolve(answer).catch(() => {
				                              });
		} catch { /* A support query never advances the queue. */
		}
		if (this.stopped)
			return Status.Closed;
		if (this.recoveryRequired)
			return Status.RecoveryRequired;
		if (!this.ready || session !== this.epoch)
			return Status.NotAdmitted;
		return supported ? Status.Ok : Status.Unsupported;
	}

	private decodeReadMany(snapshot: ReadManySnapshot,
	                       input: ByteInput): readonly ReadManyItem<unknown>[]
	{
		const payload = bytes(input).slice(), data = new DataView(payload.buffer);
		const invalid = (): never => {
			throw new BackendFailure(Status.CodecError, 'Invalid ReadMany item body');
		};
		if (payload.length < 2 || data.getUint16(0, true) !== snapshot.entries.length)
			invalid();
		let cursor = 2;
		const items: {id: number; status : 0 | 1 | 2 | 3 | 4; payload : Uint8Array}[] = [];
		for (const entry of snapshot.entries) {
			if (payload.length - cursor < 8)
				invalid();
			const id = data.getUint32(cursor, true), status = payload[cursor + 4],
			      length = data.getUint16(cursor + 6, true);
			if (id !== entry.id || status > 4 || payload[cursor + 5] !== 0 ||
			    (status === 0 ? length !== entry.size : length !== 0))
				invalid();
			cursor += 8;
			if (length > payload.length - cursor ||
			    (status === 0 && entry.scalar === 1 && payload[cursor] > 1))
				invalid();
			items.push({
				id,
				status : status as 0 | 1 | 2 | 3 | 4,
				payload : payload.slice(cursor, cursor + length)
			});
			cursor += length;
		}
		if (cursor !== payload.length)
			invalid();
		// Only after the entire body and every scalar are canonical may user codecs run.
		const current = () => this.ready && this.epoch === snapshot.session &&
		                      this.fingerprint === snapshot.fingerprint;
		const result: ReadManyItem<unknown>[] = [];
		for (let i = 0; i < items.length; ++i) {
			if (!current())
				throw new BackendFailure(Status.CodecError,
				                         'ReadMany session changed during decode');
			const item = items[i];
			if (item.status === 0) {
				const value = snapshot.entries[i].decode(item.payload.slice());
				if (!current())
					throw new BackendFailure(Status.CodecError,
					                         'ReadMany session changed during decode');
				result.push(Object.freeze({...item, status : 0, value}));
			} else {
				result.push(Object.freeze({...item, status : item.status}));
			}
		}
		return Object.freeze(result);
	}

	private refused<T>(operation: Operation, status: ClientStatus): RequestPromise<T>
	{
		if (operation === 'Batch')
			return this.refusedBatch(status) as unknown as RequestPromise<T>;
		const result = this.makeResult(operation, this.epoch, 0, status);
		return Object.assign(Promise.resolve(result as Result<T>), {
			requestId : 0,
			session : this.epoch,
			cancel : () => false,
		});
	}

	private enqueue<T>(binding: {
		fingerprint: bigint; operation : Operation; id : number; requestBytes : number;
		responseBytes : number;
		encode?: (value: never) => Uint8Array;
	},
	                   value: unknown, decode?: (input: ByteInput) => T, timeoutMs?: number,
	                   readMany?: ReadManySnapshot, batch?: BatchState): RequestPromise<T>
	{
		const operation = binding.operation;
		if (this.stopped)
			return this.refused(operation, Status.Closed);
		if (this.recoveryRequired)
			return this.refused(operation, Status.RecoveryRequired);
		if (!this.ready || binding.fingerprint !== this.fingerprint)
			return this.refused(operation, Status.NotAdmitted);
		if (operation === 'ReadMany' || operation === 'Batch') {
			const gate = this.optionalGate(operation);
			if (gate !== Status.Ok)
				return this.refused(operation, gate);
			const snapshot = operation === 'ReadMany' ? readMany : batch;
			if (!snapshot || snapshot.session !== this.epoch ||
			    snapshot.fingerprint !== this.fingerprint)
				return this.refused(operation, Status.NotAdmitted);
		}
		if (this.pending.length >= this.limit)
			return this.refused(operation, Status.QueueFull);
		const timeout = timeoutMs ?? this.timeout, generation = this.epoch,
		      now = batch?.startedAt ?? this.clock.now();
		if (!validOperation(operation) || !u32(binding.id) || !u32(binding.requestBytes) ||
		    !u32(binding.responseBytes) || !Number.isFinite(timeout) || timeout <= 0 ||
		    timeout > 0x7fffffff)
			return this.refused(operation, Status.InvalidArgument);
		if (this.nextId > 0xffffffff)
			return this.refused(operation, Status.IdsExhausted);
		if (binding.requestBytes > this.byteLimit - this.payloadBytes)
			return this.refused(operation, Status.PayloadFull);
		let payload: Uint8Array;
		try {
			payload = operation === 'FieldRead'
			              ? new Uint8Array(0)
			              : encodedSnapshot(binding.encode!(value as never), binding.requestBytes);
		} catch {
			return this.refused(operation, Status.CodecError);
		}
		// Re-entrant getters/codec callbacks may have changed session or capacity.
		if (this.stopped)
			return this.refused(operation, Status.Closed);
		if (!this.ready || generation !== this.epoch || binding.fingerprint !== this.fingerprint)
			return this.refused(operation, this.recoveryRequired ? Status.RecoveryRequired
			                                                     : Status.NotAdmitted);
		if (this.pending.length >= this.limit)
			return this.refused(operation, Status.QueueFull);
		if (this.nextId > 0xffffffff)
			return this.refused(operation, Status.IdsExhausted);
		if (payload.length > this.byteLimit - this.payloadBytes)
			return this.refused(operation, Status.PayloadFull);
		const request: WireRequest = Object.freeze({
			operation,
			endpointId : binding.id,
			session : this.epoch,
			requestId : this.nextId++,
			payload,
			responseBytes : binding.responseBytes
		});
		let resolve!: (result: Result<unknown>) => void;
		const promise = new Promise<Result<T>>(done => {
			resolve = done as (result: Result<unknown>) => void;
		});
		const pending: Pending = {
			request,
			resolve,
			decode,
			batch,
			deadline : now + timeout,
			controller : new AbortController(),
			possibleExecution : false,
			done : false
		};
		if (batch)
			batch.requestId = request.requestId;
		this.pending.push(pending);
		this.payloadBytes += payload.length;
		this.schedule();
		this.armTimer();
		return Object.assign(promise, {
			requestId : request.requestId,
			session : request.session,
			cancel : () => this.cancel(pending)
		});
	}

	private makeResult(operation: Operation, session: number, requestId: number,
	                   status: ClientStatus, wire?: WireResult, value?: unknown): Result<unknown>
	{
		const dispatch = wire?.dispatch ?? 0, endpointStatus = wire?.endpointStatus ?? 0;
		const succeeded =
		    status === Status.Ok && dispatch === 0 &&
		    (endpointStatus === 0 || (operation === 'Command' && endpointStatus === 1));
		return Object.freeze({
			status,
			operation,
			session,
			requestId,
			dispatch,
			endpointStatus,
			succeeded,
			accepted : succeeded && operation === 'Command' && endpointStatus === 1,
			executed : succeeded && operation === 'Command' && endpointStatus === 0,
			payload : wire ? new Uint8Array(wire.payload) : new Uint8Array(0),
			value
		});
	}

	private schedule(): void
	{
		if (this.scheduled)
			return;
		this.scheduled = true;
		queueMicrotask(() => {
			this.scheduled = false;
			this.process();
		});
	}

	process(): void
	{
		if (this.pumping)
			return;
		this.pumping = true;
		try {
			const now = this.clock.now();
			for (const pending of this.pending.slice()) {
				if (!pending.done && now >= pending.deadline)
					this.finish(pending, Status.Timeout);
			}
			if (!this.active && !this.invocation && this.ready && this.pending.length) {
				const pending = this.pending[0];
				if (pending.request.operation === 'ReadMany' ||
				    pending.request.operation === 'Batch') {
					const gate = this.optionalGate(pending.request.operation);
					if (gate !== Status.Ok) {
						this.finish(pending, gate);
						return;
					}
					// A support hook can cancel/replace this queued operation synchronously.
					if (pending.done || this.pending[0] !== pending || !this.ready ||
					    pending.request.session !== this.epoch) {
						this.schedule();
						return;
					}
				}
				this.invocation = this.active = pending;
				const context: BackendContext = {
					session : pending.request.session,
					requestId : pending.request.requestId,
					signal : pending.controller.signal,
					clock : this.clock,
					admissionRevoked : () => this.revoked && pending === this.active &&
					                         !pending.done && pending.possibleExecution &&
					                         !pending.controller.signal.aborted,
					handoff : mayExecute => {
					    if ((pending.request.operation === 'ReadMany' ||
					         pending.request.operation === 'Batch') &&
					        this.optionalGate(pending.request.operation) !== Status.Ok)
						    return false;
					    if (pending.done || pending !== this.active || !this.ready ||
					        this.epoch !== pending.request.session)
						    return false;
					    const handoffTime = this.clock.now();
					    // Clock hooks can revoke the operation synchronously, just like codecs.
					    if (pending.done || pending !== this.active || !this.ready ||
					        this.epoch !== pending.request.session)
						    return false;
					    if (handoffTime >= pending.deadline) {
						    this.finish(pending, Status.Timeout);
						    return false;
					    }
					    pending.possibleExecution ||= mayExecute;
					    return true;
					}
				};
				const release = () => {
					if (this.invocation === pending)
						this.invocation = undefined;
				};
				Promise.resolve()
				    .then(() => {
					    if (pending.done)
						    throw new BackendFailure(Status.Cancelled, 'Cancelled before start');
					    return this.backend.execute(pending.request, context);
				    })
				    .then(
				        wire => {
					        const released = wire?.released;
					        if (released) {
						        const quiescent = () => {
							        release();
							        this.schedule();
						        };
						        Promise.resolve(released).then(quiescent, quiescent);
					        } else {
						        release();
					        }
					        if (pending.done) {
						        if (!released)
							        this.schedule();
						        return;
					        }
					        if (this.clock.now() >= pending.deadline) {
						        this.finish(pending, Status.Timeout);
						        return;
					        }
					        try {
						        checkWireResult(pending.request, wire);
						        // A valid ReadMany envelope is known before re-entrant user codecs run.
						        if (pending.request.operation === 'ReadMany')
							        pending.checkedReadMany = Object.freeze({
								        dispatch : wire.dispatch,
								        endpointStatus : wire.endpointStatus,
								        payload : new Uint8Array(wire.payload)
							        });
						        if (pending.batch) {
							        pending.batch.checked = Object.freeze({
								        dispatch : wire.dispatch,
								        endpointStatus : wire.endpointStatus,
								        payload : new Uint8Array(wire.payload)
							        });
							        pending.batch.valueStatus =
							            wire.dispatch === 0 && wire.endpointStatus === 0
							                ? BatchValueStatus.InvalidPayload
							                : BatchValueStatus.NoValue;
						        }
						        let value: unknown;
						        if (wire.dispatch === 0 && wire.endpointStatus === 0 &&
						            pending.decode) {
							        try {
								        value = pending.decode(pending.batch?.checked?.payload ??
								                               pending.checkedReadMany?.payload ??
								                               wire.payload);
							        } catch {
								        this.finish(pending, Status.CodecError, wire);
								        return;
							        }
						        }
						        this.finish(pending, Status.Ok, wire, value);
					        } catch {
						        this.finish(pending, Status.MalformedReply);
					        }
				        },
				        error => {
					        release();
					        if (!pending.done)
						        this.finish(pending, error instanceof BackendFailure
						                                 ? error.status
						                                 : Status.TransportError);
					        else
						        this.schedule();
				        });
			}
		} finally {
			this.pumping = false;
			this.armTimer();
		}
	}

	private cancel(pending: Pending): boolean
	{
		if (pending.done)
			return false;
		this.finish(pending, Status.Cancelled);
		return true;
	}

	private cancelAll(reason: ClientStatus): void
	{
		for (const pending of this.pending.slice())
			this.finish(pending, pending === this.active ? reason : Status.Cancelled);
	}

	private finish(pending: Pending, status: ClientStatus, wire?: WireResult, value?: unknown): void
	{
		if (pending.done)
			return;
		if (pending.checkedReadMany) {
			// Closing/resetting in a decoder invalidates values, not an already checked outcome.
			wire = pending.checkedReadMany;
			if (status !== Status.Ok) {
				status = Status.CodecError;
				value = undefined;
			}
		}
		if (pending.batch?.checked) {
			wire = pending.batch.checked;
			status = Status.Ok;
			value = undefined;
		}
		// A checked reply proves an endpoint outcome. A failed transaction after
		// possible execution requires external session recovery, including reads.
		const uncertain = pending === this.active && pending.possibleExecution &&
		                  status !== Status.Ok && status !== Status.CodecError;
		if (uncertain) {
			this.recoveryEpoch = pending.request.session;
			this.admitted = false;
			if (pending.possibleExecution && pending.request.operation !== 'FieldRead' &&
			    pending.request.operation !== 'ReadMany')
				status = Status.OutcomeUnknown;
		}
		pending.done = true;
		this.pending.splice(this.pending.indexOf(pending), 1);
		this.payloadBytes -= pending.request.payload.length;
		if (this.active === pending)
			this.active = undefined;
		// Abort listeners may finish recovery and enqueue a newer session synchronously.
		const abandoned = uncertain ? this.pending.slice() : [];
		const request = pending.request;
		const result = this.makeResult(request.operation, request.session, request.requestId,
		                               status, wire, value);
		pending.resolve(request.operation === 'Batch' ? this.makeBatchResult(result, pending.batch)
		                                              : result);
		pending.controller.abort();
		for (const queued of abandoned)
			this.finish(queued, Status.Cancelled);
		// Resolution queues Promise reactions before the next FIFO request starts.
		this.schedule();
		this.armTimer();
	}

	private armTimer(): void
	{
		if (this.timer !== undefined) {
			this.clock.clearTimeout(this.timer);
			this.timer = undefined;
		}
		if (this.pending.length) {
			let deadline = Infinity;
			for (const pending of this.pending)
				deadline = Math.min(deadline, pending.deadline);
			this.timer = this.clock.setTimeout(() => {
				this.timer = undefined;
				this.process();
			}, Math.max(0, deadline - this.clock.now()));
		}
	}
}

export type EncodedSend = (packet: Uint8Array, context: ExchangeContext) => void|Promise<void>;

export interface EncodedOptions {
	// Explicit local transport opt-in; a matching remote TRM1 is required separately.
	readonly readMany?: boolean;
	// Batch is independently enabled and requires a matching remote TBM1.
	readonly batch?: boolean;
}
export interface EncodedRetryOptions extends EncodedOptions {
	readonly replyTimeoutMs?: number;
	// Retransmissions after the initial send. Endpoints may execute more than once.
	readonly maxRetries?: number;
}
interface EncodedRetryPolicy {
	readonly replyTimeoutMs: number;
	readonly maxRetries: number;
}

function encodedRetryPolicy(options: EncodedRetryOptions): EncodedRetryPolicy
{
	const replyTimeoutMs = options.replyTimeoutMs ?? 100, maxRetries = options.maxRetries ?? 3;
	if (!Number.isInteger(replyTimeoutMs) || replyTimeoutMs <= 0 || replyTimeoutMs > 0x7fffffff ||
	    !u32(maxRetries))
		throw new RangeError('Encoded retry options');
	return {replyTimeoutMs, maxRetries};
}

// One TEQ1 serializer/receiver serves both public policies; standalone codecs do not depend on it.
class EncodedEngine implements Backend {
	private execution?: {context: BackendContext};
	private waiting?: {
		request: WireRequest; context : BackendContext; resolve : (value: WireResult) => void;
		reject : (error: unknown) => void
	};

	constructor(private readonly send: EncodedSend, private readonly retry?: EncodedRetryPolicy,
	            private readonly readManyEnabled = false, private readonly batchEnabled = false)
	{}

	supports(operation: Operation): boolean
	{
		return operation === 'ReadMany' ? this.readManyEnabled
		       : operation === 'Batch'  ? this.batchEnabled
		                                : validOperation(operation);
	}

	confirmSessionClosed(generation: number): boolean
	{
		if (this.execution && (this.execution.context.session !== generation ||
		                       !this.execution.context.signal.aborted))
			return false;
		this.execution = undefined;
		return true;
	}

	async execute(request: WireRequest, context: BackendContext): Promise<WireResult>
	{
		if (!this.supports(request.operation))
			throw new BackendFailure(Status.Unsupported, 'Encoded operation not enabled');
		if (this.execution)
			throw new BackendFailure(Status.Busy, 'Encoded backend already active');
		const packet = new Uint8Array(32 + request.payload.length),
		      data = new DataView(packet.buffer);
		data.setUint32(0, 0x31514554, true);
		data.setUint8(4, 1);
		data.setUint8(6, Operations[request.operation]);
		data.setUint32(8, request.session, true);
		data.setUint32(12, request.requestId, true);
		data.setUint32(16, request.endpointId, true);
		data.setUint32(20, request.payload.length, true);
		data.setUint32(24, request.responseBytes, true);
		packet.set(request.payload, 32);
		const execution = this.execution = {context};
		let transfer: Promise<unknown> = Promise.resolve();
		const response = new Promise<WireResult>((resolve, reject) => {
			let settled = false, timer: unknown, retries = 0;
			const cleanup = () => {
				if (timer !== undefined) {
					context.clock.clearTimeout(timer);
					timer = undefined;
				}
				context.signal.removeEventListener('abort', abort);
				if (this.waiting === waiting)
					this.waiting = undefined;
			};
			const finish = (value?: WireResult, error?: unknown) => {
				if (settled)
					return;
				settled = true;
				cleanup();
				if (value)
					resolve(value);
				else
					reject(error);
			};
			const abort = () => finish(undefined, new BackendFailure(Status.Cancelled, 'Aborted'));
			const waiting = {
				request,
				context,
				resolve : (value: WireResult) => finish(value),
				reject : (error: unknown) => finish(undefined, error)
			};
			this.waiting = waiting;
			context.signal.addEventListener('abort', abort, {once : true});
			const attempt = () => {
				if (settled || context.signal.aborted || this.execution !== execution) {
					abort();
					return;
				}
				if (!context.handoff(true)) {
					if (context.admissionRevoked?.() === true && !context.signal.aborted)
						return; // Keep the original receiver; do not schedule another send.
					finish(undefined, new BackendFailure(Status.Cancelled, 'Handoff refused'));
					return;
				}
				const controller = this.retry ? new AbortController() : undefined;
				const abortTransfer = () => controller?.abort();
				if (controller)
					context.signal.addEventListener('abort', abortTransfer, {once : true});
				const sendContext = controller ? {
					session : context.session,
					requestId : context.requestId,
					signal : controller.signal
				}
				                               : context;
				const detach = () => context.signal.removeEventListener('abort', abortTransfer);
				let released = false, expired = false, advanced = false;
				const advance = () => {
					if (settled || advanced || !released || !expired)
						return;
					advanced = true;
					if (retries === this.retry!.maxRetries)
						finish(undefined,
						       new BackendFailure(Status.Timeout, 'Encoded reply timeout'));
					else {
						++retries;
						attempt();
					}
				};
				if (this.retry) {
					timer = context.clock.setTimeout(() => {
						timer = undefined;
						expired = true;
						// Abort requests release; only actual settlement permits another attempt.
						abortTransfer();
						advance();
					}, this.retry.replyTimeoutMs);
				}
				try {
					// Each port owns its attempt copy; mutation cannot corrupt a later retry.
					transfer = Promise.resolve(
					    this.send(this.retry ? packet.slice() : packet, sendContext));
					transfer.then(
					    () => {
						    detach();
						    released = true;
						    advance();
					    },
					    error => {
						    detach();
						    if (expired && !context.signal.aborted) {
							    released = true;
							    advance();
						    } else {
							    finish(undefined, error);
						    }
					    });
				} catch (error) {
					detach();
					finish(undefined, error);
				}
			};
			attempt();
		});
		const release = () => {
			if (this.execution === execution)
				this.execution = undefined;
		};
		try {
			const wire = await response;
			// Checked outcomes are timely even while the last send owns physical I/O.
			return {...wire, released : transfer.then(release, release)};
		} catch (error) {
			// Cancellation remains logical; only physical settlement releases the invocation.
			await transfer.then(release, release);
			throw error;
		}
	}

	protected receiveFromExchange(input: ByteInput, context: ExchangeContext): boolean
	{
		// A detached old exchange may settle after a newer admission. Check its
		// carrier identity before even inspecting potentially malformed input.
		const waiting = this.waiting;
		if (!waiting || waiting.context.signal.aborted ||
		    waiting.request.session !== context.session ||
		    waiting.request.requestId !== context.requestId)
			return false;
		return this.receive(input);
	}

	receive(input: ByteInput): boolean
	{
		const waiting = this.waiting;
		if (!waiting || waiting.context.signal.aborted)
			return false;
		const malformed = () => {
			this.waiting = undefined;
			waiting.reject(new BackendFailure(Status.MalformedReply, 'Malformed TEQ1 reply'));
			return false;
		};
		let frame: Uint8Array;
		try {
			frame = bytes(input);
		} catch {
			return malformed();
		}
		const request = waiting.request;
		if (frame.length < 20)
			return false;
		const data = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
		// Old or unrelated complete frames never complete the live request.
		if (data.getUint32(8, true) !== request.session ||
		    data.getUint32(12, true) !== request.requestId ||
		    data.getUint32(16, true) !== request.endpointId ||
		    data.getUint8(6) !== Operations[request.operation])
			return false;
		if (frame.length < 32)
			return malformed();
		if (data.getUint32(0, true) !== 0x31514554 || data.getUint8(4) !== 1 ||
		    data.getUint8(5) !== 1 || data.getUint32(20, true) !== frame.length - 32 ||
		    data.getUint32(24, true) !== request.responseBytes || frame[29] !== 0 ||
		    frame[30] !== 0 || frame[31] !== 0)
			return malformed();
		const result = {
			dispatch : frame[7],
			endpointStatus : frame[28],
			payload : frame.subarray(32)
		};
		try {
			checkWireResult(request, result);
		} catch {
			return malformed();
		}
		this.waiting = undefined;
		waiting.resolve({...result, payload : result.payload.slice()});
		return true;
	}
}

// Complete frames only. The port owns framing and exposes received frames via receive().
export class EncodedBackend extends EncodedEngine {
	constructor(send: EncodedSend, options: EncodedOptions = {})
	{
		super(send, undefined, options.readMany === true, options.batch === true);
	}

	static fromExchange(exchange: Exchange, options: EncodedOptions = {}): EncodedBackend
	{
		const backend = new EncodedBackend(async (packet, context) => {
			const reply = await exchange(packet, context);
			if (!context.signal.aborted)
				backend.receive(reply);
		}, options);
		return backend;
	}
}

// Opt-in repeated execution: one immutable request identity, initial send + maxRetries.
// The reply timer starts at each handoff. A retry waits for physical send settlement;
// a stuck send instead remains bounded logically by DeviceClient's overall deadline.
export class RetryingEncodedBackend extends EncodedEngine {
	constructor(send: EncodedSend, options: EncodedRetryOptions = {})
	{
		super(send, encodedRetryPolicy(options), options.readMany === true, options.batch === true);
	}

	static fromExchange(exchange: Exchange,
	                    options: EncodedRetryOptions = {}): RetryingEncodedBackend
	{
		const backend = new RetryingEncodedBackend(async (packet, context) => {
			const reply = await exchange(packet, context);
			// A late matching reply can still establish the result after attempt abort.
			backend.receiveFromExchange(reply, context);
		}, options);
		return backend;
	}
}

// Resource/RPC packet algorithm shared with the optional application adapter.
export interface RpcOptions {
	transactionId: number;
	fileIndex?: number;
	fragmentBytes?: number;
	maxPackets?: number;
	maxReplyBytes?: number;
	maxRequestBytes?: number;
}
export interface RpcOutcome {
	readonly operation: Operation;
	readonly id: number;
	readonly transactionId: number;
	readonly dispatchStatus: number;
	readonly endpointStatus: number;
	readonly accepted: boolean;
	readonly payload: Uint8Array;
}

export class RpcError extends Error {
	readonly clientStatus?: ClientStatus;
	readonly resourceStatus?: number;
	readonly dispatchStatus?: number;
	readonly endpointStatus?: number;

	constructor(message: string, details: object = {})
	{
		super(message);
		this.name = 'RpcError';
		Object.assign(this, details);
	}
}

// RPC supports only operations 1-4, independently of optional Encoded operations.
const codes = Object.freeze({FieldWrite : 1, Command : 2, Service : 3, FieldRead : 4} as const);
const ensure: (condition: unknown, message: string) => asserts condition = (condition, message) => {
	if (!condition)
		throw new RpcError(message);
};
const cursor = (transactionId: number, offset: number) =>
    (BigInt(transactionId) << 32n) | BigInt(offset);
const integer = (value: number, minimum: number, maximum: number, name: string) =>
    ensure(Number.isSafeInteger(value) && value >= minimum && value <= maximum, name);
const view = (bytes: Uint8Array) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

function resourceRead(fileIndex: number, offset: bigint)
{
	const packet = new Uint8Array(13), data = view(packet);
	data.setUint8(0, 3);
	data.setUint32(1, fileIndex, true);
	data.setBigUint64(5, offset, true);
	return packet;
}

function resourceWrite(fileIndex: number, offset: bigint, fragment: Uint8Array, final: boolean)
{
	const packet = new Uint8Array(16 + fragment.length), data = view(packet);
	data.setUint8(0, 4);
	data.setUint32(1, fileIndex, true);
	data.setBigUint64(5, offset, true);
	data.setUint8(13, final ? 1 : 0);
	data.setUint16(14, fragment.length, true);
	packet.set(fragment, 16);
	return packet;
}

function parseResource(reply: Uint8Array, bytes: number)
{
	ensure(reply instanceof Uint8Array, 'Exchange must return Uint8Array');
	if (reply.length === 1 && reply[0] === 7)
		throw new RpcError('Resource refused malformed request',
		                   {resourceStatus : 7, clientStatus : Status.TransportError});
	ensure(reply.length >= bytes, 'Truncated Resource reply');
	ensure(reply[0] <= 8, 'Unknown Resource status');
	if (reply[0] !== 0) {
		const data = view(reply);
		ensure(reply.length === bytes &&
		           (bytes === 14 ? data.getUint32(9, true) === 0 && reply[13] === 0
		                         : data.getUint16(10, true) === 0 && reply[9] === 0),
		       'Malformed Resource refusal');
		throw new RpcError('Resource refused packet',
		                   {resourceStatus : reply[0], clientStatus : Status.TransportError});
	}
	return view(reply);
}

function readChunk(reply: Uint8Array)
{
	const data = parseResource(reply, 12), count = data.getUint16(10, true);
	ensure(reply.length === 12 + count, 'Resource READ length');
	ensure(reply[9] <= 1, 'Resource READ eof');
	return {next : data.getBigUint64(1, true), eof : reply[9] === 1, bytes : reply.subarray(12)};
}

// Return checked transport outcomes; this function never decodes value payloads.
async function rpcPacketExchange(exchange: (packet: Uint8Array) => Uint8Array | Promise<Uint8Array>,
                                 operation: Operation, id: number, payload: Uint8Array,
                                 options: RpcOptions, preserveRefusals: boolean,
                                 waitForPoll?: () => Promise<void>): Promise<RpcOutcome>
{
	ensure(typeof exchange === 'function', 'Exchange callback required');
	if (!Object.hasOwn(codes, operation))
		throw new RpcError('RPC supports only operations 1-4', {clientStatus : Status.Unsupported});
	ensure(payload instanceof Uint8Array, 'RPC payload requires Uint8Array');
	ensure(options !== null && typeof options === 'object',
	       'RPC options with transaction ID required');
	const {
		transactionId,
		fileIndex = 0,
		fragmentBytes = 48,
		maxPackets = 256,
		maxReplyBytes = 1048576,
		maxRequestBytes = 1048576
	} = options;
	integer(transactionId, 1, 0xffffffff, 'Transaction ID must be a nonzero u32');
	integer(fileIndex, 0, 0xffffffff, 'File index must be u32');
	integer(id, 0, 0xffffffff, 'Endpoint ID must be u32');
	integer(fragmentBytes, 24, 65535, 'First WRITE must fit the RPC header');
	integer(maxPackets, 1, 1000000, 'Packet budget');
	integer(maxReplyBytes, 32, 0xffffffff, 'Reply byte budget');
	integer(maxRequestBytes, 24, 0xffffffff, 'Request byte budget');
	ensure(payload.length <= maxRequestBytes - 24, 'Request exceeds byte budget');
	ensure(operation !== 'FieldRead' || payload.length === 0, 'FieldRead payload must be empty');
	let packets = 0;
	const send = async (packet: Uint8Array) => {
		if (++packets > maxPackets)
			throw new RpcError('Packet budget exhausted', {clientStatus : Status.Timeout});
		return await exchange(packet);
	};
	const request = new Uint8Array(24 + payload.length), header = view(request);
	header.setUint32(0, 0x43505254, true); // TRPC
	header.setUint16(4, 1, true);
	header.setUint16(6, 0, true);
	header.setUint32(8, transactionId, true);
	header.setUint32(12, id, true);
	header.setUint32(16, payload.length, true);
	header.setUint8(20, codes[operation as keyof typeof codes]);
	request.set(payload, 24);
	let offset = 0;
	while (offset < request.length) {
		const fragment = request.subarray(offset, Math.min(offset + fragmentBytes, request.length));
		const final = offset + fragment.length === request.length;
		const reply = await send(
		    resourceWrite(fileIndex, offset ? cursor(transactionId, offset) : 0n, fragment, final));
		const data = parseResource(reply, 14);
		ensure(reply.length === 14 && reply[13] <= 1, 'Resource WRITE envelope');
		ensure(data.getUint32(9, true) === fragment.length, 'Resource WRITE consumed count');
		ensure(data.getBigUint64(1, true) === cursor(transactionId, offset + fragment.length),
		       'Resource WRITE cursor identity');
		ensure((reply[13] === 1) === final, 'Resource WRITE completion');
		offset += fragment.length;
	}
	// WRITE next belongs to the request stream. Every status poll starts at zero.
	let first: ReturnType<typeof readChunk>,
	    resultHeader: {payloadBytes: number; dispatchStatus : number; endpointStatus : number;};
	for (;;) {
		first = readChunk(await send(resourceRead(fileIndex, 0n)));
		ensure(first.bytes.length >= 32, 'RPC header cannot be fragmented');
		const data = view(first.bytes);
		ensure(data.getUint32(0, true) === 0x53505254 && data.getUint16(4, true) === 1 &&
		           data.getUint16(6, true) === 0,
		       'RPC reply magic/version');
		ensure(data.getUint32(8, true) === transactionId && data.getUint32(12, true) === id &&
		           data.getUint8(20) === codes[operation as keyof typeof codes],
		       'RPC reply transaction/endpoint/operation identity');
		const state = data.getUint8(21), payloadBytes = data.getUint32(16, true);
		ensure(data.getUint32(24, true) === request.length &&
		           data.getUint32(28, true) === request.length,
		       'RPC accepted request byte counts');
		if (state === 4) {
			resultHeader = {
				payloadBytes,
				dispatchStatus : data.getUint8(22),
				endpointStatus : data.getUint8(23)
			};
			break;
		}
		ensure(state === 2 || state === 3, 'RPC must be Pending, Running or Ready');
		ensure(payloadBytes === 0 && data.getUint8(22) === 0 && data.getUint8(23) === 0 &&
		           first.bytes.length === 32 && first.eof && first.next === 0n,
		       'RPC pending snapshot');
		// The first READ follows the final WRITE ACK immediately. Only a checked
		// Pending/Running reply schedules another delayed status poll.
		if (waitForPoll)
			await waitForPoll();
	}
	const total = 32 + resultHeader.payloadBytes;
	ensure(total <= maxReplyBytes, 'RPC result exceeds byte budget');
	ensure(first.bytes.length <= total &&
	           first.next === cursor(transactionId, first.bytes.length) &&
	           first.eof === (first.bytes.length === total),
	       'RPC first READ cursor/length/eof');
	const record = new Uint8Array(total);
	record.set(first.bytes);
	offset = first.bytes.length;
	while (offset < total) {
		const chunk = readChunk(await send(resourceRead(fileIndex, cursor(transactionId, offset))));
		ensure(chunk.bytes.length > 0 && chunk.bytes.length <= total - offset,
		       'RPC READ progress/bounds');
		ensure(chunk.next === cursor(transactionId, offset + chunk.bytes.length) &&
		           chunk.eof === (offset + chunk.bytes.length === total),
		       'RPC continuation cursor/length/eof');
		record.set(chunk.bytes, offset);
		offset += chunk.bytes.length;
	}
	const {dispatchStatus, endpointStatus} = resultHeader;
	ensure([ 0, 4, 5, 6, 7, 8, 9 ].includes(dispatchStatus), 'Unknown dispatch status');
	if (dispatchStatus !== 0) {
		ensure(endpointStatus === 0 && resultHeader.payloadBytes === 0,
		       'Failed dispatch carried endpoint result');
		if (!preserveRefusals)
			throw new RpcError('Encoded dispatch refused operation',
			                   {dispatchStatus, endpointStatus});
	}
	const maximumStatus = {FieldRead : 0,
		                   FieldWrite : 5,
		                   Command : 7,
		                   Service : 4}[operation as keyof typeof codes];
	ensure(endpointStatus <= maximumStatus, 'Unknown endpoint status');
	const successful = endpointStatus === 0 || (operation === 'Command' && endpointStatus === 1);
	const hasValue = operation === 'FieldRead' || operation === 'Service';
	ensure((successful && hasValue) || resultHeader.payloadBytes === 0,
	       'Operation/status must have no payload');
	if (!successful && !preserveRefusals)
		throw new RpcError('Endpoint refused operation', {dispatchStatus, endpointStatus});

	return Object.freeze({
		operation,
		id,
		transactionId,
		dispatchStatus,
		endpointStatus,
		accepted : operation === 'Command' && endpointStatus === 1,
		payload : record.subarray(32)
	});
}

export function rpcExchange(exchange: (packet: Uint8Array) => Uint8Array | Promise<Uint8Array>,
                            operation: Operation, id: number, payload: Uint8Array,
                            options: RpcOptions): Promise<RpcOutcome>
{
	return rpcPacketExchange(exchange, operation, id, payload, options, false);
}

// Generated metadata supplies the operation and codec. Status checks above run
// before any decode, including zero-byte Empty/Void service response codecs.
export function rpcInvoke<R>(exchange: (packet: Uint8Array) => Uint8Array | Promise<Uint8Array>,
                             binding: Pick<ReadBinding<R>, 'operation'|'id'|'decode'>,
                             value: undefined,
                             options: RpcOptions): Promise<RpcOutcome&{readonly value : R}>;
export function rpcInvoke<Q, R>(
    exchange: (packet: Uint8Array) => Uint8Array | Promise<Uint8Array>,
    binding: Pick<ServiceBinding<Q, R>, 'operation'|'id'|'encode'|'decodeResponse'>, value: Q,
    options: RpcOptions): Promise<RpcOutcome&{readonly value : R}>;
export function rpcInvoke<Q>(
    exchange: (packet: Uint8Array) => Uint8Array | Promise<Uint8Array>,
    binding: Pick<WriteBinding<Q>|CommandBinding<Q>, 'operation'|'id'|'encode'>, value: Q,
    options: RpcOptions): Promise<RpcOutcome&{readonly value : undefined}>;

export async function rpcInvoke<Q, R>(
    exchange: (packet: Uint8Array) => Uint8Array | Promise<Uint8Array>, binding: {
	    readonly operation: Operation; readonly id : number; encode ? (value: Q) : Uint8Array;
	    decode ? (input: ByteInput) : R;
	    decodeResponse ? (input: ByteInput) : R
    },
    value: Q, options: RpcOptions): Promise<RpcOutcome&{readonly value : R | undefined}>
{
	ensure(binding !== null && typeof binding === 'object',
	       'Generated operation metadata required');
	const payload = binding.operation === 'FieldRead' ? new Uint8Array(0) : binding.encode!(value);
	const outcome = await rpcExchange(exchange, binding.operation, binding.id, payload, options);
	if (outcome.dispatchStatus !== 0 || (outcome.endpointStatus !== 0 && !outcome.accepted))
		throw new RpcError('Endpoint refused operation', outcome);
	const decoded = binding.operation === 'FieldRead' ? binding.decode!(outcome.payload)
	                : binding.operation === 'Service' ? binding.decodeResponse!(outcome.payload)
	                                                  : undefined;
	return Object.freeze({...outcome, value : decoded});
}

export interface RpcBackendOptions extends Omit<RpcOptions, 'transactionId'> {
	readonly ackTimeoutMs?: number;
	readonly pollIntervalMs?: number;
	readonly maxRetries?: number;
}

// RPC retries repeat only the same active Resource packet/transaction. An uncertain
// terminal outcome is quarantined by DeviceClient; a fresh request is never a retry.
export class RpcBackend implements Backend {
	private readonly options: RpcBackendOptions;
	private executing?: {context: BackendContext};

	constructor(private readonly exchange: Exchange, options: RpcBackendOptions = {})
	{
		this.options = Object.freeze({...options});
		const ack = options.ackTimeoutMs ?? 1000, poll = options.pollIntervalMs ?? 10,
		      retries = options.maxRetries ?? 2;
		if (!Number.isFinite(ack) || ack <= 0 || ack > 0x7fffffff || !Number.isFinite(poll) ||
		    poll < 0 || poll > 0x7fffffff || !Number.isSafeInteger(retries) || retries < 0 ||
		    retries > 1000)
			throw new RangeError('RPC retry/poll policy');
		integer(options.fileIndex ?? 0, 0, 0xffffffff, 'File index');
		integer(options.fragmentBytes ?? 48, 24, 65535, 'Fragment capacity');
		integer(options.maxPackets ?? 256, 1, 1000000, 'Packet budget');
		integer(options.maxRequestBytes ?? 1048576, 24, 0xffffffff, 'Request capacity');
		integer(options.maxReplyBytes ?? 1048576, 32, 0xffffffff, 'Reply capacity');
	}

	supports(operation: Operation): boolean
	{
		return Object.hasOwn(codes, operation);
	}

	confirmSessionClosed(generation: number): boolean
	{
		if (this.executing && (this.executing.context.session !== generation ||
		                       !this.executing.context.signal.aborted))
			return false;
		this.executing = undefined;
		return true;
	}

	async execute(request: WireRequest, context: BackendContext): Promise<WireResult>
	{
		if (!this.supports(request.operation))
			throw new BackendFailure(Status.Unsupported, 'RPC supports only operations 1-4');
		if (this.executing)
			throw new BackendFailure(Status.Busy, 'RPC backend already active');
		if (request.payload.length > (this.options.maxRequestBytes ?? 1048576) - 24 ||
		    request.responseBytes > (this.options.maxReplyBytes ?? 1048576) - 32)
			throw new BackendFailure(Status.BufferTooSmall, 'RPC request/result capacity');
		const execution = {context};
		this.executing = execution;
		const release = () => {
			if (this.executing === execution)
				this.executing = undefined;
		};
		try {
			return await this.run(request, context);
		} finally {
			release();
		}
	}

	private async run(request: WireRequest, context: BackendContext): Promise<WireResult>
	{
		const pollInterval = this.options.pollIntervalMs ?? 10;
		const exchange = async(packet: Uint8Array): Promise<Uint8Array> => {
			if (context.signal.aborted)
				throw new BackendFailure(Status.Cancelled, 'RPC cancelled');
			const final = packet[0] === 4 && packet[13] === 1;
			const maximum = this.options.maxRetries ?? 2;
			for (let attempt = 0;; ++attempt) {
				try {
					const reply = await this.attempt(packet, context, final);
					try {
						return bytes(reply).slice();
					} catch {
						throw new BackendFailure(Status.MalformedReply,
						                         'Resource reply requires bytes');
					}
				} catch (error) {
					if (context.signal.aborted)
						throw new BackendFailure(Status.Cancelled, 'RPC cancelled');
					if (!(error instanceof BackendFailure) || error.status !== Status.Timeout ||
					    attempt >= maximum)
						throw error;
				}
			}
		};
		let outcome: RpcOutcome;
		try {
			outcome = await rpcPacketExchange(
			    exchange, request.operation, request.endpointId, request.payload,
			    {...this.options, transactionId : request.requestId}, true,
			    pollInterval > 0 ? () => this.delay(pollInterval, context) : undefined);
		} catch (error) {
			if (error instanceof BackendFailure)
				throw error;
			if (error instanceof RpcError)
				throw new BackendFailure(error.clientStatus ?? Status.MalformedReply,
				                         error.message);
			throw error;
		}
		return {
			dispatch : outcome.dispatchStatus,
			endpointStatus : outcome.endpointStatus,
			payload : outcome.payload
		};
	}

	private delay(milliseconds: number, context: BackendContext): Promise<void>
	{
		return new Promise((resolve, reject) => {
			if (context.signal.aborted) {
				reject(new BackendFailure(Status.Cancelled, 'RPC cancelled'));
				return;
			}
			const abort = () => {
				context.clock.clearTimeout(timer);
				reject(new BackendFailure(Status.Cancelled, 'RPC cancelled'));
			};
			const timer = context.clock.setTimeout(() => {
				context.signal.removeEventListener('abort', abort);
				resolve();
			}, milliseconds);
			context.signal.addEventListener('abort', abort, {once : true});
		});
	}

	private attempt(packet: Uint8Array, context: BackendContext,
	                mayExecute: boolean): Promise<ByteInput>
	{
		return new Promise((resolve, reject) => {
			const ownedPacket = packet.slice();
			const controller = new AbortController();
			let done = false;
			let passiveAbort: (() => void)|undefined;
			let interrupted: {error: unknown}|undefined;
			const settle = (error?: unknown, reply?: ByteInput, failed = true) => {
				if (done)
					return;
				done = true;
				context.clock.clearTimeout(timer);
				context.signal.removeEventListener('abort', abort);
				if (passiveAbort)
					context.signal.removeEventListener('abort', passiveAbort);
				controller.abort();
				if (interrupted || failed)
					reject(interrupted ? interrupted.error : error);
				else
					resolve(reply!);
			};
			const interrupt = (error: unknown) => {
				interrupted ??= {error};
				context.clock.clearTimeout(timer);
				controller.abort();
			};
			const abort = () => interrupt(new BackendFailure(Status.Cancelled, 'RPC cancelled'));
			const timer = context.clock.setTimeout(
			    () => interrupt(new BackendFailure(Status.Timeout, 'RPC ACK timeout')),
			    this.options.ackTimeoutMs ?? 1000);
			context.signal.addEventListener('abort', abort, {once : true});
			if (context.signal.aborted) {
				settle(new BackendFailure(Status.Cancelled, 'RPC cancelled'));
				return;
			}
			if (!context.handoff(mayExecute)) {
				if (context.admissionRevoked?.() === true && !context.signal.aborted) {
					// No exchange started, so abort can settle immediately after passive waiting.
					context.clock.clearTimeout(timer);
					context.signal.removeEventListener('abort', abort);
					passiveAbort = () =>
					    settle(new BackendFailure(Status.Cancelled, 'RPC admission revoked'));
					context.signal.addEventListener('abort', passiveAbort, {once : true});
					if (context.signal.aborted)
						passiveAbort();
					return;
				}
				settle(new BackendFailure(Status.Cancelled, 'RPC handoff refused'));
				return;
			}
			// Port owns this copy until its Promise settles, including timed-out late replies.
			try {
				Promise
				    .resolve(this.exchange(ownedPacket, {
					    session : context.session,
					    requestId : context.requestId,
					    signal : controller.signal
				    }))
				    .then(reply => settle(undefined, reply, false), error => settle(error));
			} catch (error) {
				settle(error);
			}
		});
	}
}
