/* One-owner asynchronous FIFO client. Authors: Ruslan Kovtun (shpegun60), codexAi. MIT. */
export const Status = Object.freeze({
    Ok: 0,
    QueueFull: 1,
    PayloadFull: 2,
    NotAdmitted: 3,
    ReadOnly: 4,
    Closed: 5,
    Cancelled: 6,
    Timeout: 7,
    SessionLost: 8,
    OutcomeUnknown: 9,
    MalformedReply: 10,
    TransportError: 11,
    CodecError: 12,
    BufferTooSmall: 13,
    InvalidArgument: 14,
    Busy: 15,
    RecoveryRequired: 16,
    IdsExhausted: 17,
    Unsupported: 18,
});
export const Operations = Object.freeze({ FieldWrite: 1, Command: 2, Service: 3, FieldRead: 4, ReadMany: 5, Batch: 6 });
// Pure inspection for host planning. This never admits a session or bypasses
// bindReadMany(), which still checks identity, field types and byte budgets.
export function inspectReadManyCapability(input) {
    try {
        const record = new Uint8Array(bytes(input));
        if (record.length !== 24)
            return undefined;
        const cap = new DataView(record.buffer);
        const itemLimit = record[5], requestLimit = cap.getUint16(8, true), replyLimit = cap.getUint16(10, true), session = cap.getUint32(12, true);
        if (cap.getUint32(0, true) !== 0x314d5254 || record[4] !== 1 || record[6] !== 1 ||
            record[7] !== 1 || itemLimit < 1 || itemLimit > 32 || requestLimit < 6 ||
            requestLimit > 130 || replyLimit < 10 || replyLimit > 514 || session === 0)
            return undefined;
        return Object.freeze({
            itemLimit,
            requestLimit,
            replyLimit,
            session,
            fingerprint: cap.getBigUint64(16, true)
        });
    }
    catch {
        return undefined;
    }
}
export const ReadManyStatus = Object.freeze({ Ok: 0, NotFound: 1, UnsupportedType: 2, Unavailable: 3, ReadError: 4 });
export const BatchValueStatus = Object.freeze({ Ok: 'Ok', NoValue: 'NoValue', InvalidPayload: 'InvalidPayload', Stale: 'Stale' });
const batchArguments = new WeakMap();
function batchItem(operation, binding, argument) {
    const item = Object.freeze({ operation });
    batchArguments.set(item, { operation, binding, argument });
    return item;
}
// Pure argument selectors. Encoding/ownership starts only inside DeviceClient.batch().
export const Batch = Object.freeze({
    items(...items) {
        return Object.freeze(items);
    },
    read(field) {
        return batchItem('FieldRead', field);
    },
    write(field, value) {
        return batchItem('FieldWrite', field, value);
    },
    command(binding, ...value) {
        return batchItem('Command', binding, value[0]);
    },
    service(binding, ...value) {
        return batchItem('Service', binding, value[0]);
    }
});
const systemClock = {
    now: () => performance.now(),
    setTimeout: (callback, milliseconds) => setTimeout(callback, milliseconds),
    clearTimeout: handle => clearTimeout(handle),
};
const bytes = (input) => input instanceof ArrayBuffer ? new Uint8Array(input)
    : new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
const u32 = (n) => Number.isInteger(n) && n >= 0 && n <= 0xffffffff;
const typedArrayLength = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), 'length').get;
function encodedSnapshot(input, expected) {
    if (!(input instanceof Uint8Array) || typedArrayLength.call(input) !== expected)
        throw new BackendFailure(Status.CodecError, 'Exact encoded request length');
    return new Uint8Array(input);
}
const validOperation = (operation) => Object.hasOwn(Operations, operation);
class BackendFailure extends Error {
    status;
    constructor(status, message) {
        super(message);
        this.status = status;
    }
}
function checkWireResult(request, result) {
    const { dispatch, endpointStatus, payload } = result;
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
    const maximum = { FieldRead: 0, FieldWrite: 5, Command: 7, Service: 4 }[request.operation];
    const expected = endpointStatus === 0 &&
        (request.operation === 'FieldRead' || request.operation === 'Service')
        ? request.responseBytes
        : 0;
    if (endpointStatus > maximum || payload.length !== expected)
        throw new BackendFailure(Status.MalformedReply, 'Invalid endpoint status or payload length');
}
export class DeviceClient {
    backend;
    identity;
    clock;
    limit;
    byteLimit;
    timeout;
    pending = [];
    active;
    invocation;
    epoch = 0;
    fingerprint = 0n;
    lastEpoch = 0;
    nextId = 1;
    payloadBytes = 0;
    recoveryEpoch = 0;
    stopped = false;
    admitted = false;
    revoked = false;
    timer;
    scheduled = false;
    pumping = false;
    confirming = false;
    readManyBindings = new WeakMap();
    constructor(options) {
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
    get session() {
        return this.epoch;
    }
    get ready() {
        return this.admitted && !this.stopped && this.recoveryEpoch === 0;
    }
    get recoveryRequired() {
        return this.recoveryEpoch !== 0;
    }
    get recoveryGeneration() {
        return this.recoveryEpoch;
    }
    get pendingCount() {
        return this.pending.length;
    }
    get snapshotBytes() {
        return this.payloadBytes;
    }
    get drained() {
        return this.stopped && this.pending.length === 0 && !this.invocation;
    }
    // Caller opens a fresh physical session and supplies its admitted descriptor header.
    openSession(generation, header) {
        if (this.stopped || this.confirming || this.recoveryRequired || this.pending.length ||
            this.invocation || !u32(generation) || generation === 0 || generation <= this.lastEpoch)
            return false;
        let matches = false;
        let declared;
        try {
            declared = bytes(header).slice();
            matches = declared.length === 64 && this.identity(declared);
        }
        catch {
            return false;
        }
        // The identity callback may synchronously close, admit or enqueue on this client.
        if (!matches || this.stopped || this.confirming || this.recoveryRequired ||
            this.pending.length || this.invocation || generation <= this.lastEpoch)
            return false;
        this.fingerprint = new DataView(declared.buffer).getBigUint64(16, true);
        this.epoch = this.lastEpoch = generation;
        this.nextId = 1;
        this.admitted = true;
        this.revoked = false;
        return true;
    }
    // Assertion by the application: old transport and remote retained session are closed/drained.
    // Re-admitting a header alone never makes an uncertain old session safe.
    confirmSessionClosed(generation) {
        if (this.confirming || this.admitted || !this.epoch || generation !== this.epoch ||
            this.pending.length ||
            (this.invocation && (this.invocation.request.session !== generation ||
                !this.invocation.controller.signal.aborted)))
            return false;
        this.confirming = true;
        try {
            const confirm = this.backend.confirmSessionClosed;
            if (confirm) {
                const confirmed = confirm.call(this.backend, generation);
                if (confirmed !== true) {
                    // Refuse async/truthy hooks; observe a rejected async hook without trusting it.
                    Promise.resolve(confirmed).catch(() => {
                    });
                    return false;
                }
            }
        }
        catch {
            return false;
        }
        finally {
            this.confirming = false;
        }
        this.invocation = undefined;
        this.recoveryEpoch = 0;
        return true;
    }
    resetSession() {
        this.admitted = false;
        this.cancelAll(Status.SessionLost);
    }
    // Revoke future execution without cancelling a possibly executed active request.
    // Application-owned physical/remote cleanup is still required before readmission.
    invalidateAdmission() {
        this.revoked = true;
        this.admitted = false;
        for (const pending of this.pending.slice()) {
            if (pending !== this.active || !pending.possibleExecution)
                this.finish(pending, Status.Cancelled);
        }
    }
    close() {
        this.stopped = true;
        this.admitted = false;
        this.cancelAll(Status.Cancelled);
    }
    read(field, timeoutMs) {
        const binding = 'read' in field ? field.read : field;
        return this.enqueue(binding, undefined, binding.decode, timeoutMs);
    }
    write(field, value, timeoutMs) {
        const binding = 'read' in field ? field.write : field;
        if (!binding)
            return this.refused('FieldWrite', Status.ReadOnly);
        return this.enqueue(binding, value, undefined, timeoutMs);
    }
    command(binding, value, timeoutMs) {
        return this.enqueue(binding, value, undefined, timeoutMs);
    }
    service(binding, value, timeoutMs) {
        return this.enqueue(binding, value, binding.decodeResponse, timeoutMs);
    }
    // Discover TRM1 outside this client, then cache this binding for the admitted session.
    bindReadMany(capability, fields) {
        const refused = (status) => ({ status: status });
        const gate = this.optionalGate();
        if (gate !== Status.Ok)
            return refused(gate);
        const session = this.epoch, fingerprint = this.fingerprint;
        let entries, payload, responseBytes = 2;
        try {
            const cap = inspectReadManyCapability(capability);
            if (!cap)
                return refused(Status.InvalidArgument);
            const { itemLimit, requestLimit, replyLimit } = cap;
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
            const selected = [];
            for (let i = 0; i < count; ++i)
                selected.push(fields[i]);
            entries = [];
            const sizes = [0, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8];
            for (const field of selected) {
                const read = 'read' in field ? field.read : field;
                const { id, operation, requestBytes, responseBytes: size, readManyScalar: scalar, decode, fingerprint: identity } = read;
                if (identity !== fingerprint)
                    return refused(Status.NotAdmitted);
                if (operation !== 'FieldRead' || !u32(id) || requestBytes !== 0 ||
                    typeof decode !== 'function' || entries.some(entry => entry.id === id))
                    return refused(Status.InvalidArgument);
                if (!Number.isInteger(scalar) || scalar < 1 || scalar > 11 ||
                    size !== sizes[scalar])
                    return refused(Status.Unsupported);
                entries.push(Object.freeze({ id, size, scalar, decode }));
                responseBytes += 8 + size;
            }
            if (responseBytes > replyLimit)
                return refused(Status.BufferTooSmall);
            payload = new Uint8Array(2 + 4 * entries.length);
            const request = new DataView(payload.buffer);
            request.setUint16(0, entries.length, true);
            entries.forEach((entry, i) => request.setUint32(2 + 4 * i, entry.id, true));
        }
        catch {
            return refused(Status.InvalidArgument);
        }
        // Property getters and the backend support hook can re-enter. Never publish across admission.
        const finalGate = this.optionalGate();
        if (finalGate !== Status.Ok || this.epoch !== session || this.fingerprint !== fingerprint)
            return refused(finalGate === Status.Ok ? Status.NotAdmitted : finalGate);
        const binding = Object.freeze({ session, fingerprint, count: entries.length });
        this.readManyBindings.set(binding, Object.freeze({ session, fingerprint, payload, responseBytes, entries: Object.freeze(entries) }));
        return Object.freeze({ status: Status.Ok, binding });
    }
    readMany(binding, timeoutMs) {
        const gate = this.optionalGate();
        if (gate !== Status.Ok)
            return this.refused('ReadMany', gate);
        const snapshot = this.readManyBindings.get(binding);
        if (!snapshot || snapshot.session !== this.epoch ||
            snapshot.fingerprint !== this.fingerprint)
            return this.refused('ReadMany', Status.NotAdmitted);
        return this.enqueue({
            fingerprint: snapshot.fingerprint,
            operation: 'ReadMany',
            id: 0,
            requestBytes: snapshot.payload.length,
            responseBytes: snapshot.responseBytes,
            encode: () => snapshot.payload
        }, undefined, input => this.decodeReadMany(snapshot, input), timeoutMs, snapshot);
    }
    batch(capability, items, timeoutMs) {
        const refused = (status) => this.refusedBatch(status);
        const gate = this.optionalGate('Batch');
        if (gate !== Status.Ok)
            return refused(gate);
        if (this.pending.length >= this.limit)
            return refused(Status.QueueFull);
        if (this.nextId > 0xffffffff)
            return refused(Status.IdsExhausted);
        const session = this.epoch, fingerprint = this.fingerprint, startedAt = this.clock.now();
        if (!this.ready || this.epoch !== session || this.fingerprint !== fingerprint)
            return refused(this.stopped ? Status.Closed
                : this.recoveryRequired ? Status.RecoveryRequired
                    : Status.NotAdmitted);
        const timeout = timeoutMs ?? this.timeout;
        if (!Number.isFinite(timeout) || timeout <= 0 || timeout > 0x7fffffff)
            return refused(Status.InvalidArgument);
        let entries, payload, responseBytes = 2;
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
            const selected = [];
            for (let i = 0; i < count; ++i) {
                const argument = batchArguments.get(items[i]);
                if (!argument)
                    return refused(Status.InvalidArgument);
                selected.push(argument);
            }
            entries = [];
            let requestBytes = 2;
            for (const selectedItem of selected) {
                let metadata = selectedItem.binding;
                if (!metadata || (typeof metadata !== 'object' && typeof metadata !== 'function'))
                    return refused(Status.InvalidArgument);
                if (selectedItem.operation === 'FieldRead' && 'read' in metadata)
                    metadata = metadata.read;
                if (selectedItem.operation === 'FieldWrite' &&
                    ('read' in metadata || 'write' in metadata)) {
                    metadata = metadata.write;
                    if (!metadata)
                        return refused(Status.ReadOnly);
                }
                const { operation, id, fingerprint: identity, requestBytes: requestSize, responseBytes: responseSize, encode, decode, decodeResponse, responseTypeId } = metadata;
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
                    requestBytes: requestSize,
                    responseBytes: responseSize,
                    encode,
                    decode: operation === 'Service' ? decodeResponse
                        : operation === 'FieldRead' ? decode
                            : undefined,
                    voidResponse: operation === 'FieldWrite' || operation === 'Command' ||
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
                    : encodedSnapshot(entry.encode(selected[i].argument), entry.requestBytes);
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
        }
        catch {
            return refused(Status.CodecError);
        }
        const finalGate = this.optionalGate('Batch');
        if (finalGate !== Status.Ok || this.epoch !== session || this.fingerprint !== fingerprint)
            return refused(finalGate === Status.Ok ? Status.NotAdmitted : finalGate);
        const state = {
            session,
            fingerprint,
            startedAt,
            payload,
            responseBytes,
            entries: Object.freeze(entries),
            requestId: 0,
            valueStatus: BatchValueStatus.NoValue
        };
        return this.enqueue({
            fingerprint,
            operation: 'Batch',
            id: 0,
            requestBytes: payload.length,
            responseBytes,
            encode: () => payload
        }, undefined, input => this.decodeBatch(state, input), timeoutMs, undefined, state);
    }
    refusedBatch(status) {
        const result = this.makeBatchResult(this.makeResult('Batch', this.epoch, 0, status));
        return Object.assign(Promise.resolve(result), { requestId: 0, session: this.epoch, cancel: () => false });
    }
    decodeBatch(state, input) {
        state.valueStatus = BatchValueStatus.InvalidPayload;
        const payload = new Uint8Array(bytes(input)), data = new DataView(payload.buffer);
        if (payload.length < 2 || data.getUint16(0, true) !== state.entries.length)
            return;
        const rows = [];
        let cursor = 2;
        for (const entry of state.entries) {
            if (payload.length - cursor < 12)
                return;
            const id = data.getUint32(cursor, true), operation = payload[cursor + 4], dispatch = payload[cursor + 5], endpointStatus = payload[cursor + 6], length = data.getUint16(cursor + 8, true);
            if (id !== entry.id || operation !== Operations[entry.operation] ||
                payload[cursor + 7] !== 0 || payload[cursor + 10] !== 0 ||
                payload[cursor + 11] !== 0 || ![0, 4, 5, 6, 7, 8, 9].includes(dispatch))
                return;
            const maximum = { FieldRead: 0,
                FieldWrite: 5,
                Command: 7,
                Service: 4 }[entry.operation];
            const successful = dispatch === 0 &&
                (endpointStatus === 0 || (entry.operation === 'Command' && endpointStatus === 1));
            const hasPayload = successful && (entry.operation === 'FieldRead' || entry.operation === 'Service');
            if ((dispatch !== 0 && endpointStatus !== 0) || endpointStatus > maximum ||
                length !== (hasPayload ? entry.responseBytes : 0))
                return;
            cursor += 12;
            if (length > payload.length - cursor)
                return;
            const operationResult = this.makeResult(entry.operation, state.session, state.requestId, Status.Ok, { dispatch, endpointStatus, payload: payload.subarray(cursor, cursor + length) });
            rows.push(Object.freeze({
                ...operationResult,
                id,
                value: undefined,
                valueStatus: successful && entry.decode && !entry.voidResponse
                    ? BatchValueStatus.InvalidPayload
                    : BatchValueStatus.NoValue
            }));
            cursor += length;
        }
        if (cursor !== payload.length)
            return;
        // All operation outcomes are owned/checked before any user decoder may re-enter.
        state.items = Object.freeze(rows);
        const current = () => this.ready && this.epoch === state.session && this.fingerprint === state.fingerprint;
        const values = [];
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
                }
                catch {
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
            valueStatus: values[i] === undefined ? BatchValueStatus.NoValue : BatchValueStatus.Ok,
            value: values[i]
        })));
        state.valueStatus = BatchValueStatus.Ok;
        return;
    }
    makeBatchResult(operation, state) {
        let valueStatus = state?.valueStatus ?? BatchValueStatus.NoValue, items = state?.items;
        if (state?.checked &&
            (!this.ready || state.session !== this.epoch || state.fingerprint !== this.fingerprint))
            valueStatus = BatchValueStatus.Stale;
        if (items && valueStatus !== BatchValueStatus.Ok) {
            items = Object.freeze(items.map((row, i) => Object.freeze({
                ...row,
                value: undefined,
                valueStatus: row.succeeded && state.entries[i].decode && !state.entries[i].voidResponse
                    ? valueStatus
                    : BatchValueStatus.NoValue
            })));
        }
        return Object.freeze({ ...operation, value: undefined, valueStatus, items });
    }
    optionalGate(operation = 'ReadMany') {
        if (this.stopped)
            return Status.Closed;
        if (this.recoveryRequired)
            return Status.RecoveryRequired;
        if (!this.ready)
            return Status.NotAdmitted;
        const session = this.epoch;
        let supported = false;
        try {
            const answer = this.backend.supports?.(operation);
            supported = answer === true;
            // Invalid asynchronous hooks are refused; still observe their rejected promises.
            if (answer !== null && (typeof answer === 'object' || typeof answer === 'function'))
                Promise.resolve(answer).catch(() => {
                });
        }
        catch { /* A support query never advances the queue. */
        }
        if (this.stopped)
            return Status.Closed;
        if (this.recoveryRequired)
            return Status.RecoveryRequired;
        if (!this.ready || session !== this.epoch)
            return Status.NotAdmitted;
        return supported ? Status.Ok : Status.Unsupported;
    }
    decodeReadMany(snapshot, input) {
        const payload = bytes(input).slice(), data = new DataView(payload.buffer);
        const invalid = () => {
            throw new BackendFailure(Status.CodecError, 'Invalid ReadMany item body');
        };
        if (payload.length < 2 || data.getUint16(0, true) !== snapshot.entries.length)
            invalid();
        let cursor = 2;
        const items = [];
        for (const entry of snapshot.entries) {
            if (payload.length - cursor < 8)
                invalid();
            const id = data.getUint32(cursor, true), status = payload[cursor + 4], length = data.getUint16(cursor + 6, true);
            if (id !== entry.id || status > 4 || payload[cursor + 5] !== 0 ||
                (status === 0 ? length !== entry.size : length !== 0))
                invalid();
            cursor += 8;
            if (length > payload.length - cursor ||
                (status === 0 && entry.scalar === 1 && payload[cursor] > 1))
                invalid();
            items.push({
                id,
                status: status,
                payload: payload.slice(cursor, cursor + length)
            });
            cursor += length;
        }
        if (cursor !== payload.length)
            invalid();
        // Only after the entire body and every scalar are canonical may user codecs run.
        const current = () => this.ready && this.epoch === snapshot.session &&
            this.fingerprint === snapshot.fingerprint;
        const result = [];
        for (let i = 0; i < items.length; ++i) {
            if (!current())
                throw new BackendFailure(Status.CodecError, 'ReadMany session changed during decode');
            const item = items[i];
            if (item.status === 0) {
                const value = snapshot.entries[i].decode(item.payload.slice());
                if (!current())
                    throw new BackendFailure(Status.CodecError, 'ReadMany session changed during decode');
                result.push(Object.freeze({ ...item, status: 0, value }));
            }
            else {
                result.push(Object.freeze({ ...item, status: item.status }));
            }
        }
        return Object.freeze(result);
    }
    refused(operation, status) {
        if (operation === 'Batch')
            return this.refusedBatch(status);
        const result = this.makeResult(operation, this.epoch, 0, status);
        return Object.assign(Promise.resolve(result), {
            requestId: 0,
            session: this.epoch,
            cancel: () => false,
        });
    }
    enqueue(binding, value, decode, timeoutMs, readMany, batch) {
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
        const timeout = timeoutMs ?? this.timeout, generation = this.epoch, now = batch?.startedAt ?? this.clock.now();
        if (!validOperation(operation) || !u32(binding.id) || !u32(binding.requestBytes) ||
            !u32(binding.responseBytes) || !Number.isFinite(timeout) || timeout <= 0 ||
            timeout > 0x7fffffff)
            return this.refused(operation, Status.InvalidArgument);
        if (this.nextId > 0xffffffff)
            return this.refused(operation, Status.IdsExhausted);
        if (binding.requestBytes > this.byteLimit - this.payloadBytes)
            return this.refused(operation, Status.PayloadFull);
        let payload;
        try {
            payload = operation === 'FieldRead'
                ? new Uint8Array(0)
                : encodedSnapshot(binding.encode(value), binding.requestBytes);
        }
        catch {
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
        const request = Object.freeze({
            operation,
            endpointId: binding.id,
            session: this.epoch,
            requestId: this.nextId++,
            payload,
            responseBytes: binding.responseBytes
        });
        let resolve;
        const promise = new Promise(done => {
            resolve = done;
        });
        const pending = {
            request,
            resolve,
            decode,
            batch,
            deadline: now + timeout,
            controller: new AbortController(),
            possibleExecution: false,
            done: false
        };
        if (batch)
            batch.requestId = request.requestId;
        this.pending.push(pending);
        this.payloadBytes += payload.length;
        this.schedule();
        this.armTimer();
        return Object.assign(promise, {
            requestId: request.requestId,
            session: request.session,
            cancel: () => this.cancel(pending)
        });
    }
    makeResult(operation, session, requestId, status, wire, value) {
        const dispatch = wire?.dispatch ?? 0, endpointStatus = wire?.endpointStatus ?? 0;
        const succeeded = status === Status.Ok && dispatch === 0 &&
            (endpointStatus === 0 || (operation === 'Command' && endpointStatus === 1));
        return Object.freeze({
            status,
            operation,
            session,
            requestId,
            dispatch,
            endpointStatus,
            succeeded,
            accepted: succeeded && operation === 'Command' && endpointStatus === 1,
            executed: succeeded && operation === 'Command' && endpointStatus === 0,
            payload: wire ? new Uint8Array(wire.payload) : new Uint8Array(0),
            value
        });
    }
    schedule() {
        if (this.scheduled)
            return;
        this.scheduled = true;
        queueMicrotask(() => {
            this.scheduled = false;
            this.process();
        });
    }
    process() {
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
                const context = {
                    session: pending.request.session,
                    requestId: pending.request.requestId,
                    signal: pending.controller.signal,
                    clock: this.clock,
                    admissionRevoked: () => this.revoked && pending === this.active &&
                        !pending.done && pending.possibleExecution &&
                        !pending.controller.signal.aborted,
                    handoff: mayExecute => {
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
                    .then(wire => {
                    const released = wire?.released;
                    if (released) {
                        const quiescent = () => {
                            release();
                            this.schedule();
                        };
                        Promise.resolve(released).then(quiescent, quiescent);
                    }
                    else {
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
                                dispatch: wire.dispatch,
                                endpointStatus: wire.endpointStatus,
                                payload: new Uint8Array(wire.payload)
                            });
                        if (pending.batch) {
                            pending.batch.checked = Object.freeze({
                                dispatch: wire.dispatch,
                                endpointStatus: wire.endpointStatus,
                                payload: new Uint8Array(wire.payload)
                            });
                            pending.batch.valueStatus =
                                wire.dispatch === 0 && wire.endpointStatus === 0
                                    ? BatchValueStatus.InvalidPayload
                                    : BatchValueStatus.NoValue;
                        }
                        let value;
                        if (wire.dispatch === 0 && wire.endpointStatus === 0 &&
                            pending.decode) {
                            try {
                                value = pending.decode(pending.batch?.checked?.payload ??
                                    pending.checkedReadMany?.payload ??
                                    wire.payload);
                            }
                            catch {
                                this.finish(pending, Status.CodecError, wire);
                                return;
                            }
                        }
                        this.finish(pending, Status.Ok, wire, value);
                    }
                    catch {
                        this.finish(pending, Status.MalformedReply);
                    }
                }, error => {
                    release();
                    if (!pending.done)
                        this.finish(pending, error instanceof BackendFailure
                            ? error.status
                            : Status.TransportError);
                    else
                        this.schedule();
                });
            }
        }
        finally {
            this.pumping = false;
            this.armTimer();
        }
    }
    cancel(pending) {
        if (pending.done)
            return false;
        this.finish(pending, Status.Cancelled);
        return true;
    }
    cancelAll(reason) {
        for (const pending of this.pending.slice())
            this.finish(pending, pending === this.active ? reason : Status.Cancelled);
    }
    finish(pending, status, wire, value) {
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
        const result = this.makeResult(request.operation, request.session, request.requestId, status, wire, value);
        pending.resolve(request.operation === 'Batch' ? this.makeBatchResult(result, pending.batch)
            : result);
        pending.controller.abort();
        for (const queued of abandoned)
            this.finish(queued, Status.Cancelled);
        // Resolution queues Promise reactions before the next FIFO request starts.
        this.schedule();
        this.armTimer();
    }
    armTimer() {
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
function encodedRetryPolicy(options) {
    const replyTimeoutMs = options.replyTimeoutMs ?? 100, maxRetries = options.maxRetries ?? 3;
    if (!Number.isInteger(replyTimeoutMs) || replyTimeoutMs <= 0 || replyTimeoutMs > 0x7fffffff ||
        !u32(maxRetries))
        throw new RangeError('Encoded retry options');
    return { replyTimeoutMs, maxRetries };
}
// One TEQ1 serializer/receiver serves both public policies; standalone codecs do not depend on it.
class EncodedEngine {
    send;
    retry;
    readManyEnabled;
    batchEnabled;
    execution;
    waiting;
    constructor(send, retry, readManyEnabled = false, batchEnabled = false) {
        this.send = send;
        this.retry = retry;
        this.readManyEnabled = readManyEnabled;
        this.batchEnabled = batchEnabled;
    }
    supports(operation) {
        return operation === 'ReadMany' ? this.readManyEnabled
            : operation === 'Batch' ? this.batchEnabled
                : validOperation(operation);
    }
    confirmSessionClosed(generation) {
        if (this.execution && (this.execution.context.session !== generation ||
            !this.execution.context.signal.aborted))
            return false;
        this.execution = undefined;
        return true;
    }
    async execute(request, context) {
        if (!this.supports(request.operation))
            throw new BackendFailure(Status.Unsupported, 'Encoded operation not enabled');
        if (this.execution)
            throw new BackendFailure(Status.Busy, 'Encoded backend already active');
        const packet = new Uint8Array(32 + request.payload.length), data = new DataView(packet.buffer);
        data.setUint32(0, 0x31514554, true);
        data.setUint8(4, 1);
        data.setUint8(6, Operations[request.operation]);
        data.setUint32(8, request.session, true);
        data.setUint32(12, request.requestId, true);
        data.setUint32(16, request.endpointId, true);
        data.setUint32(20, request.payload.length, true);
        data.setUint32(24, request.responseBytes, true);
        packet.set(request.payload, 32);
        const execution = this.execution = { context };
        let transfer = Promise.resolve();
        const response = new Promise((resolve, reject) => {
            let settled = false, timer, retries = 0;
            const cleanup = () => {
                if (timer !== undefined) {
                    context.clock.clearTimeout(timer);
                    timer = undefined;
                }
                context.signal.removeEventListener('abort', abort);
                if (this.waiting === waiting)
                    this.waiting = undefined;
            };
            const finish = (value, error) => {
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
                resolve: (value) => finish(value),
                reject: (error) => finish(undefined, error)
            };
            this.waiting = waiting;
            context.signal.addEventListener('abort', abort, { once: true });
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
                    context.signal.addEventListener('abort', abortTransfer, { once: true });
                const sendContext = controller ? {
                    session: context.session,
                    requestId: context.requestId,
                    signal: controller.signal
                }
                    : context;
                const detach = () => context.signal.removeEventListener('abort', abortTransfer);
                let released = false, expired = false, advanced = false;
                const advance = () => {
                    if (settled || advanced || !released || !expired)
                        return;
                    advanced = true;
                    if (retries === this.retry.maxRetries)
                        finish(undefined, new BackendFailure(Status.Timeout, 'Encoded reply timeout'));
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
                    transfer = Promise.resolve(this.send(this.retry ? packet.slice() : packet, sendContext));
                    transfer.then(() => {
                        detach();
                        released = true;
                        advance();
                    }, error => {
                        detach();
                        if (expired && !context.signal.aborted) {
                            released = true;
                            advance();
                        }
                        else {
                            finish(undefined, error);
                        }
                    });
                }
                catch (error) {
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
            return { ...wire, released: transfer.then(release, release) };
        }
        catch (error) {
            // Cancellation remains logical; only physical settlement releases the invocation.
            await transfer.then(release, release);
            throw error;
        }
    }
    receiveFromExchange(input, context) {
        // A detached old exchange may settle after a newer admission. Check its
        // carrier identity before even inspecting potentially malformed input.
        const waiting = this.waiting;
        if (!waiting || waiting.context.signal.aborted ||
            waiting.request.session !== context.session ||
            waiting.request.requestId !== context.requestId)
            return false;
        return this.receive(input);
    }
    receive(input) {
        const waiting = this.waiting;
        if (!waiting || waiting.context.signal.aborted)
            return false;
        const malformed = () => {
            this.waiting = undefined;
            waiting.reject(new BackendFailure(Status.MalformedReply, 'Malformed TEQ1 reply'));
            return false;
        };
        let frame;
        try {
            frame = bytes(input);
        }
        catch {
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
            dispatch: frame[7],
            endpointStatus: frame[28],
            payload: frame.subarray(32)
        };
        try {
            checkWireResult(request, result);
        }
        catch {
            return malformed();
        }
        this.waiting = undefined;
        waiting.resolve({ ...result, payload: result.payload.slice() });
        return true;
    }
}
// Complete frames only. The port owns framing and exposes received frames via receive().
export class EncodedBackend extends EncodedEngine {
    constructor(send, options = {}) {
        super(send, undefined, options.readMany === true, options.batch === true);
    }
    static fromExchange(exchange, options = {}) {
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
    constructor(send, options = {}) {
        super(send, encodedRetryPolicy(options), options.readMany === true, options.batch === true);
    }
    static fromExchange(exchange, options = {}) {
        const backend = new RetryingEncodedBackend(async (packet, context) => {
            const reply = await exchange(packet, context);
            // A late matching reply can still establish the result after attempt abort.
            backend.receiveFromExchange(reply, context);
        }, options);
        return backend;
    }
}
export class RpcError extends Error {
    clientStatus;
    resourceStatus;
    dispatchStatus;
    endpointStatus;
    constructor(message, details = {}) {
        super(message);
        this.name = 'RpcError';
        Object.assign(this, details);
    }
}
// RPC supports only operations 1-4, independently of optional Encoded operations.
const codes = Object.freeze({ FieldWrite: 1, Command: 2, Service: 3, FieldRead: 4 });
const ensure = (condition, message) => {
    if (!condition)
        throw new RpcError(message);
};
const cursor = (transactionId, offset) => (BigInt(transactionId) << 32n) | BigInt(offset);
const integer = (value, minimum, maximum, name) => ensure(Number.isSafeInteger(value) && value >= minimum && value <= maximum, name);
const view = (bytes) => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
function resourceRead(fileIndex, offset) {
    const packet = new Uint8Array(13), data = view(packet);
    data.setUint8(0, 3);
    data.setUint32(1, fileIndex, true);
    data.setBigUint64(5, offset, true);
    return packet;
}
function resourceWrite(fileIndex, offset, fragment, final) {
    const packet = new Uint8Array(16 + fragment.length), data = view(packet);
    data.setUint8(0, 4);
    data.setUint32(1, fileIndex, true);
    data.setBigUint64(5, offset, true);
    data.setUint8(13, final ? 1 : 0);
    data.setUint16(14, fragment.length, true);
    packet.set(fragment, 16);
    return packet;
}
function parseResource(reply, bytes) {
    ensure(reply instanceof Uint8Array, 'Exchange must return Uint8Array');
    if (reply.length === 1 && reply[0] === 7)
        throw new RpcError('Resource refused malformed request', { resourceStatus: 7, clientStatus: Status.TransportError });
    ensure(reply.length >= bytes, 'Truncated Resource reply');
    ensure(reply[0] <= 8, 'Unknown Resource status');
    if (reply[0] !== 0) {
        const data = view(reply);
        ensure(reply.length === bytes &&
            (bytes === 14 ? data.getUint32(9, true) === 0 && reply[13] === 0
                : data.getUint16(10, true) === 0 && reply[9] === 0), 'Malformed Resource refusal');
        throw new RpcError('Resource refused packet', { resourceStatus: reply[0], clientStatus: Status.TransportError });
    }
    return view(reply);
}
function readChunk(reply) {
    const data = parseResource(reply, 12), count = data.getUint16(10, true);
    ensure(reply.length === 12 + count, 'Resource READ length');
    ensure(reply[9] <= 1, 'Resource READ eof');
    return { next: data.getBigUint64(1, true), eof: reply[9] === 1, bytes: reply.subarray(12) };
}
// Return checked transport outcomes; this function never decodes value payloads.
async function rpcPacketExchange(exchange, operation, id, payload, options, preserveRefusals, waitForPoll) {
    ensure(typeof exchange === 'function', 'Exchange callback required');
    if (!Object.hasOwn(codes, operation))
        throw new RpcError('RPC supports only operations 1-4', { clientStatus: Status.Unsupported });
    ensure(payload instanceof Uint8Array, 'RPC payload requires Uint8Array');
    ensure(options !== null && typeof options === 'object', 'RPC options with transaction ID required');
    const { transactionId, fileIndex = 0, fragmentBytes = 48, maxPackets = 256, maxReplyBytes = 1048576, maxRequestBytes = 1048576 } = options;
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
    const send = async (packet) => {
        if (++packets > maxPackets)
            throw new RpcError('Packet budget exhausted', { clientStatus: Status.Timeout });
        return await exchange(packet);
    };
    const request = new Uint8Array(24 + payload.length), header = view(request);
    header.setUint32(0, 0x43505254, true); // TRPC
    header.setUint16(4, 1, true);
    header.setUint16(6, 0, true);
    header.setUint32(8, transactionId, true);
    header.setUint32(12, id, true);
    header.setUint32(16, payload.length, true);
    header.setUint8(20, codes[operation]);
    request.set(payload, 24);
    let offset = 0;
    while (offset < request.length) {
        const fragment = request.subarray(offset, Math.min(offset + fragmentBytes, request.length));
        const final = offset + fragment.length === request.length;
        const reply = await send(resourceWrite(fileIndex, offset ? cursor(transactionId, offset) : 0n, fragment, final));
        const data = parseResource(reply, 14);
        ensure(reply.length === 14 && reply[13] <= 1, 'Resource WRITE envelope');
        ensure(data.getUint32(9, true) === fragment.length, 'Resource WRITE consumed count');
        ensure(data.getBigUint64(1, true) === cursor(transactionId, offset + fragment.length), 'Resource WRITE cursor identity');
        ensure((reply[13] === 1) === final, 'Resource WRITE completion');
        offset += fragment.length;
    }
    // WRITE next belongs to the request stream. Every status poll starts at zero.
    let first, resultHeader;
    for (;;) {
        first = readChunk(await send(resourceRead(fileIndex, 0n)));
        ensure(first.bytes.length >= 32, 'RPC header cannot be fragmented');
        const data = view(first.bytes);
        ensure(data.getUint32(0, true) === 0x53505254 && data.getUint16(4, true) === 1 &&
            data.getUint16(6, true) === 0, 'RPC reply magic/version');
        ensure(data.getUint32(8, true) === transactionId && data.getUint32(12, true) === id &&
            data.getUint8(20) === codes[operation], 'RPC reply transaction/endpoint/operation identity');
        const state = data.getUint8(21), payloadBytes = data.getUint32(16, true);
        ensure(data.getUint32(24, true) === request.length &&
            data.getUint32(28, true) === request.length, 'RPC accepted request byte counts');
        if (state === 4) {
            resultHeader = {
                payloadBytes,
                dispatchStatus: data.getUint8(22),
                endpointStatus: data.getUint8(23)
            };
            break;
        }
        ensure(state === 2 || state === 3, 'RPC must be Pending, Running or Ready');
        ensure(payloadBytes === 0 && data.getUint8(22) === 0 && data.getUint8(23) === 0 &&
            first.bytes.length === 32 && first.eof && first.next === 0n, 'RPC pending snapshot');
        // The first READ follows the final WRITE ACK immediately. Only a checked
        // Pending/Running reply schedules another delayed status poll.
        if (waitForPoll)
            await waitForPoll();
    }
    const total = 32 + resultHeader.payloadBytes;
    ensure(total <= maxReplyBytes, 'RPC result exceeds byte budget');
    ensure(first.bytes.length <= total &&
        first.next === cursor(transactionId, first.bytes.length) &&
        first.eof === (first.bytes.length === total), 'RPC first READ cursor/length/eof');
    const record = new Uint8Array(total);
    record.set(first.bytes);
    offset = first.bytes.length;
    while (offset < total) {
        const chunk = readChunk(await send(resourceRead(fileIndex, cursor(transactionId, offset))));
        ensure(chunk.bytes.length > 0 && chunk.bytes.length <= total - offset, 'RPC READ progress/bounds');
        ensure(chunk.next === cursor(transactionId, offset + chunk.bytes.length) &&
            chunk.eof === (offset + chunk.bytes.length === total), 'RPC continuation cursor/length/eof');
        record.set(chunk.bytes, offset);
        offset += chunk.bytes.length;
    }
    const { dispatchStatus, endpointStatus } = resultHeader;
    ensure([0, 4, 5, 6, 7, 8, 9].includes(dispatchStatus), 'Unknown dispatch status');
    if (dispatchStatus !== 0) {
        ensure(endpointStatus === 0 && resultHeader.payloadBytes === 0, 'Failed dispatch carried endpoint result');
        if (!preserveRefusals)
            throw new RpcError('Encoded dispatch refused operation', { dispatchStatus, endpointStatus });
    }
    const maximumStatus = { FieldRead: 0,
        FieldWrite: 5,
        Command: 7,
        Service: 4 }[operation];
    ensure(endpointStatus <= maximumStatus, 'Unknown endpoint status');
    const successful = endpointStatus === 0 || (operation === 'Command' && endpointStatus === 1);
    const hasValue = operation === 'FieldRead' || operation === 'Service';
    ensure((successful && hasValue) || resultHeader.payloadBytes === 0, 'Operation/status must have no payload');
    if (!successful && !preserveRefusals)
        throw new RpcError('Endpoint refused operation', { dispatchStatus, endpointStatus });
    return Object.freeze({
        operation,
        id,
        transactionId,
        dispatchStatus,
        endpointStatus,
        accepted: operation === 'Command' && endpointStatus === 1,
        payload: record.subarray(32)
    });
}
export function rpcExchange(exchange, operation, id, payload, options) {
    return rpcPacketExchange(exchange, operation, id, payload, options, false);
}
export async function rpcInvoke(exchange, binding, value, options) {
    ensure(binding !== null && typeof binding === 'object', 'Generated operation metadata required');
    const payload = binding.operation === 'FieldRead' ? new Uint8Array(0) : binding.encode(value);
    const outcome = await rpcExchange(exchange, binding.operation, binding.id, payload, options);
    if (outcome.dispatchStatus !== 0 || (outcome.endpointStatus !== 0 && !outcome.accepted))
        throw new RpcError('Endpoint refused operation', outcome);
    const decoded = binding.operation === 'FieldRead' ? binding.decode(outcome.payload)
        : binding.operation === 'Service' ? binding.decodeResponse(outcome.payload)
            : undefined;
    return Object.freeze({ ...outcome, value: decoded });
}
// RPC retries repeat only the same active Resource packet/transaction. An uncertain
// terminal outcome is quarantined by DeviceClient; a fresh request is never a retry.
export class RpcBackend {
    exchange;
    options;
    executing;
    constructor(exchange, options = {}) {
        this.exchange = exchange;
        this.options = Object.freeze({ ...options });
        const ack = options.ackTimeoutMs ?? 1000, poll = options.pollIntervalMs ?? 10, retries = options.maxRetries ?? 2;
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
    supports(operation) {
        return Object.hasOwn(codes, operation);
    }
    confirmSessionClosed(generation) {
        if (this.executing && (this.executing.context.session !== generation ||
            !this.executing.context.signal.aborted))
            return false;
        this.executing = undefined;
        return true;
    }
    async execute(request, context) {
        if (!this.supports(request.operation))
            throw new BackendFailure(Status.Unsupported, 'RPC supports only operations 1-4');
        if (this.executing)
            throw new BackendFailure(Status.Busy, 'RPC backend already active');
        if (request.payload.length > (this.options.maxRequestBytes ?? 1048576) - 24 ||
            request.responseBytes > (this.options.maxReplyBytes ?? 1048576) - 32)
            throw new BackendFailure(Status.BufferTooSmall, 'RPC request/result capacity');
        const execution = { context };
        this.executing = execution;
        const release = () => {
            if (this.executing === execution)
                this.executing = undefined;
        };
        try {
            return await this.run(request, context);
        }
        finally {
            release();
        }
    }
    async run(request, context) {
        const pollInterval = this.options.pollIntervalMs ?? 10;
        const exchange = async (packet) => {
            if (context.signal.aborted)
                throw new BackendFailure(Status.Cancelled, 'RPC cancelled');
            const final = packet[0] === 4 && packet[13] === 1;
            const maximum = this.options.maxRetries ?? 2;
            for (let attempt = 0;; ++attempt) {
                try {
                    const reply = await this.attempt(packet, context, final);
                    try {
                        return bytes(reply).slice();
                    }
                    catch {
                        throw new BackendFailure(Status.MalformedReply, 'Resource reply requires bytes');
                    }
                }
                catch (error) {
                    if (context.signal.aborted)
                        throw new BackendFailure(Status.Cancelled, 'RPC cancelled');
                    if (!(error instanceof BackendFailure) || error.status !== Status.Timeout ||
                        attempt >= maximum)
                        throw error;
                }
            }
        };
        let outcome;
        try {
            outcome = await rpcPacketExchange(exchange, request.operation, request.endpointId, request.payload, { ...this.options, transactionId: request.requestId }, true, pollInterval > 0 ? () => this.delay(pollInterval, context) : undefined);
        }
        catch (error) {
            if (error instanceof BackendFailure)
                throw error;
            if (error instanceof RpcError)
                throw new BackendFailure(error.clientStatus ?? Status.MalformedReply, error.message);
            throw error;
        }
        return {
            dispatch: outcome.dispatchStatus,
            endpointStatus: outcome.endpointStatus,
            payload: outcome.payload
        };
    }
    delay(milliseconds, context) {
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
            context.signal.addEventListener('abort', abort, { once: true });
        });
    }
    attempt(packet, context, mayExecute) {
        return new Promise((resolve, reject) => {
            const ownedPacket = packet.slice();
            const controller = new AbortController();
            let done = false;
            let passiveAbort;
            let interrupted;
            const settle = (error, reply, failed = true) => {
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
                    resolve(reply);
            };
            const interrupt = (error) => {
                interrupted ??= { error };
                context.clock.clearTimeout(timer);
                controller.abort();
            };
            const abort = () => interrupt(new BackendFailure(Status.Cancelled, 'RPC cancelled'));
            const timer = context.clock.setTimeout(() => interrupt(new BackendFailure(Status.Timeout, 'RPC ACK timeout')), this.options.ackTimeoutMs ?? 1000);
            context.signal.addEventListener('abort', abort, { once: true });
            if (context.signal.aborted) {
                settle(new BackendFailure(Status.Cancelled, 'RPC cancelled'));
                return;
            }
            if (!context.handoff(mayExecute)) {
                if (context.admissionRevoked?.() === true && !context.signal.aborted) {
                    // No exchange started, so abort can settle immediately after passive waiting.
                    context.clock.clearTimeout(timer);
                    context.signal.removeEventListener('abort', abort);
                    passiveAbort = () => settle(new BackendFailure(Status.Cancelled, 'RPC admission revoked'));
                    context.signal.addEventListener('abort', passiveAbort, { once: true });
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
                    session: context.session,
                    requestId: context.requestId,
                    signal: controller.signal
                }))
                    .then(reply => settle(undefined, reply, false), error => settle(error));
            }
            catch (error) {
                settle(error);
            }
        });
    }
}
