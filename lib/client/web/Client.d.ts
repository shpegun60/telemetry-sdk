export declare const Status: Readonly<{
    readonly Ok: 0;
    readonly QueueFull: 1;
    readonly PayloadFull: 2;
    readonly NotAdmitted: 3;
    readonly ReadOnly: 4;
    readonly Closed: 5;
    readonly Cancelled: 6;
    readonly Timeout: 7;
    readonly SessionLost: 8;
    readonly OutcomeUnknown: 9;
    readonly MalformedReply: 10;
    readonly TransportError: 11;
    readonly CodecError: 12;
    readonly BufferTooSmall: 13;
    readonly InvalidArgument: 14;
    readonly Busy: 15;
    readonly RecoveryRequired: 16;
    readonly IdsExhausted: 17;
    readonly Unsupported: 18;
}>;
export type ClientStatus = typeof Status[keyof typeof Status];
export type Operation = 'FieldWrite' | 'Command' | 'Service' | 'FieldRead' | 'ReadMany' | 'Batch';
export declare const Operations: Readonly<{
    readonly FieldWrite: 1;
    readonly Command: 2;
    readonly Service: 3;
    readonly FieldRead: 4;
    readonly ReadMany: 5;
    readonly Batch: 6;
}>;
export type ByteInput = ArrayBuffer | ArrayBufferView;
export interface ReadManyCapability {
    readonly itemLimit: number;
    readonly requestLimit: number;
    readonly replyLimit: number;
    readonly session: number;
    readonly fingerprint: bigint;
}
export declare function inspectReadManyCapability(input: ByteInput): ReadManyCapability | undefined;
export interface ReadBinding<T> {
    readonly fingerprint: bigint;
    readonly operation: 'FieldRead';
    readonly id: number;
    readonly requestBytes: number;
    readonly responseBytes: number;
    readonly readManyScalar?: ReadManyScalar;
    decode(input: ByteInput): T;
}
export type ReadManyScalar = 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11;
export interface ReadManyReadBinding<T> extends ReadBinding<T> {
    readonly readManyScalar: ReadManyScalar;
}
export type ReadManyField<T> = ReadManyReadBinding<T> | {
    readonly read: ReadManyReadBinding<T>;
};
export declare const ReadManyStatus: Readonly<{
    readonly Ok: 0;
    readonly NotFound: 1;
    readonly UnsupportedType: 2;
    readonly Unavailable: 3;
    readonly ReadError: 4;
}>;
export type ReadManyItem<T> = {
    readonly id: number;
    readonly status: 0;
    readonly value: T;
    readonly payload: Uint8Array;
} | {
    readonly id: number;
    readonly status: 1 | 2 | 3 | 4;
    readonly value?: never;
    readonly payload: Uint8Array;
};
type ReadManyValue<F> = F extends {
    readonly read: ReadBinding<infer T>;
} ? T : F extends ReadBinding<infer T> ? T : never;
export type ReadManyValues<F extends readonly ReadManyField<unknown>[]> = {
    readonly [K in keyof F]: ReadManyItem<ReadManyValue<F[K]>>;
};
declare const readManyTypes: unique symbol;
export interface ReadManyBinding<T extends readonly unknown[]> {
    readonly session: number;
    readonly fingerprint: bigint;
    readonly count: number;
    readonly [readManyTypes]: T;
}
export type ReadManyBindingResult<T extends readonly unknown[]> = {
    readonly status: typeof Status.Ok;
    readonly binding: ReadManyBinding<T>;
} | {
    readonly status: Exclude<ClientStatus, typeof Status.Ok>;
    readonly binding?: never;
};
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
export type BatchOperation = 'FieldRead' | 'FieldWrite' | 'Command' | 'Service';
export declare const BatchValueStatus: Readonly<{
    readonly Ok: "Ok";
    readonly NoValue: "NoValue";
    readonly InvalidPayload: "InvalidPayload";
    readonly Stale: "Stale";
}>;
export type BatchValueStatus = typeof BatchValueStatus[keyof typeof BatchValueStatus];
declare const batchTypes: unique symbol;
export interface BatchItem<T> {
    readonly operation: BatchOperation;
    readonly [batchTypes]: T;
}
export type BatchItemResult<T> = Omit<Result<T>, 'value'> & {
    readonly id: number;
} & ({
    readonly valueStatus: typeof BatchValueStatus.Ok;
    readonly value: T;
} | {
    readonly valueStatus: Exclude<BatchValueStatus, typeof BatchValueStatus.Ok>;
    readonly value: undefined;
});
export type BatchItems<I extends readonly BatchItem<unknown>[]> = {
    readonly [K in keyof I]: BatchItemResult<I[K] extends BatchItem<infer T> ? T : never>;
};
type BatchRequest<B extends {
    encode: (value: never) => Uint8Array;
}> = Parameters<B['encode']>[0];
type BatchArguments<B extends {
    encode: (value: never) => Uint8Array;
}> = [
    BatchRequest<B>
] extends [void] ? [value?: BatchRequest<B>] : [value: BatchRequest<B>];
export interface BatchResult<T extends readonly unknown[]> extends Result<undefined> {
    readonly items: T | undefined;
    readonly valueStatus: BatchValueStatus;
}
export interface BatchPromise<T extends readonly unknown[]> extends Promise<BatchResult<T>> {
    readonly requestId: number;
    readonly session: number;
    cancel(): boolean;
}
export declare const Batch: Readonly<{
    items<const I extends readonly BatchItem<unknown>[]>(...items: I): I;
    read<T>(field: Field<T> | ReadBinding<T>): BatchItem<T>;
    write<T>(field: {
        readonly write: WriteBinding<T>;
    } | WriteBinding<T>, value: NoInfer<T>): BatchItem<undefined>;
    command<const B extends CommandBinding<never>>(binding: B, ...value: BatchArguments<NoInfer<B>>): BatchItem<undefined>;
    service<const B extends ServiceBinding<never, unknown>>(binding: B, ...value: BatchArguments<NoInfer<B>>): BatchItem<ReturnType<B["decodeResponse"]>>;
}>;
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
    readonly value: T | undefined;
}
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
    readonly released?: Promise<void>;
}
export interface ExchangeContext {
    readonly session: number;
    readonly requestId: number;
    readonly signal: AbortSignal;
}
export type Exchange = (packet: Uint8Array, context?: ExchangeContext) => ByteInput | Promise<ByteInput>;
export interface BackendContext extends ExchangeContext {
    readonly clock: Clock;
    handoff(mayExecute: boolean): boolean;
    admissionRevoked?(): boolean;
}
export interface Backend {
    supports?(operation: Operation): boolean;
    execute(request: WireRequest, context: BackendContext): Promise<WireResult>;
    confirmSessionClosed?(generation: number): boolean;
}
export interface ClientOptions {
    readonly backend: Backend;
    readonly matchesHeader: (header: ByteInput) => boolean;
    readonly maxPending?: number;
    readonly maxPayloadBytes?: number;
    readonly timeoutMs?: number;
    readonly clock?: Clock;
}
export declare class DeviceClient {
    private readonly backend;
    private readonly identity;
    private readonly clock;
    private readonly limit;
    private readonly byteLimit;
    private readonly timeout;
    private pending;
    private active?;
    private invocation?;
    private epoch;
    private fingerprint;
    private lastEpoch;
    private nextId;
    private payloadBytes;
    private recoveryEpoch;
    private stopped;
    private admitted;
    private revoked;
    private timer?;
    private scheduled;
    private pumping;
    private confirming;
    private readonly readManyBindings;
    constructor(options: ClientOptions);
    get session(): number;
    get ready(): boolean;
    get recoveryRequired(): boolean;
    get recoveryGeneration(): number;
    get pendingCount(): number;
    get snapshotBytes(): number;
    get drained(): boolean;
    openSession(generation: number, header: ByteInput): boolean;
    confirmSessionClosed(generation: number): boolean;
    resetSession(): void;
    invalidateAdmission(): void;
    close(): void;
    read<T>(field: Field<T> | ReadBinding<T>, timeoutMs?: number): RequestPromise<T>;
    write<T>(field: Field<T> | WriteBinding<T>, value: T, timeoutMs?: number): RequestPromise<undefined>;
    command<T>(binding: CommandBinding<T>, value: T, timeoutMs?: number): RequestPromise<undefined>;
    service<Q, R>(binding: ServiceBinding<Q, R>, value: Q, timeoutMs?: number): RequestPromise<R>;
    bindReadMany<const F extends readonly ReadManyField<unknown>[]>(capability: ByteInput, fields: F): ReadManyBindingResult<ReadManyValues<F>>;
    readMany<T extends readonly unknown[]>(binding: ReadManyBinding<T>, timeoutMs?: number): RequestPromise<T>;
    batch<const I extends readonly BatchItem<unknown>[]>(capability: ByteInput, items: I, timeoutMs?: number): BatchPromise<BatchItems<I>>;
    private refusedBatch;
    private decodeBatch;
    private makeBatchResult;
    private optionalGate;
    private decodeReadMany;
    private refused;
    private enqueue;
    private makeResult;
    private schedule;
    process(): void;
    private cancel;
    private cancelAll;
    private finish;
    private armTimer;
}
export type EncodedSend = (packet: Uint8Array, context: ExchangeContext) => void | Promise<void>;
export interface EncodedOptions {
    readonly readMany?: boolean;
    readonly batch?: boolean;
}
export interface EncodedRetryOptions extends EncodedOptions {
    readonly replyTimeoutMs?: number;
    readonly maxRetries?: number;
}
interface EncodedRetryPolicy {
    readonly replyTimeoutMs: number;
    readonly maxRetries: number;
}
declare class EncodedEngine implements Backend {
    private readonly send;
    private readonly retry?;
    private readonly readManyEnabled;
    private readonly batchEnabled;
    private execution?;
    private waiting?;
    constructor(send: EncodedSend, retry?: EncodedRetryPolicy | undefined, readManyEnabled?: boolean, batchEnabled?: boolean);
    supports(operation: Operation): boolean;
    confirmSessionClosed(generation: number): boolean;
    execute(request: WireRequest, context: BackendContext): Promise<WireResult>;
    protected receiveFromExchange(input: ByteInput, context: ExchangeContext): boolean;
    receive(input: ByteInput): boolean;
}
export declare class EncodedBackend extends EncodedEngine {
    constructor(send: EncodedSend, options?: EncodedOptions);
    static fromExchange(exchange: Exchange, options?: EncodedOptions): EncodedBackend;
}
export declare class RetryingEncodedBackend extends EncodedEngine {
    constructor(send: EncodedSend, options?: EncodedRetryOptions);
    static fromExchange(exchange: Exchange, options?: EncodedRetryOptions): RetryingEncodedBackend;
}
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
export declare class RpcError extends Error {
    readonly clientStatus?: ClientStatus;
    readonly resourceStatus?: number;
    readonly dispatchStatus?: number;
    readonly endpointStatus?: number;
    constructor(message: string, details?: object);
}
export declare function rpcExchange(exchange: (packet: Uint8Array) => Uint8Array | Promise<Uint8Array>, operation: Operation, id: number, payload: Uint8Array, options: RpcOptions): Promise<RpcOutcome>;
export declare function rpcInvoke<R>(exchange: (packet: Uint8Array) => Uint8Array | Promise<Uint8Array>, binding: Pick<ReadBinding<R>, 'operation' | 'id' | 'decode'>, value: undefined, options: RpcOptions): Promise<RpcOutcome & {
    readonly value: R;
}>;
export declare function rpcInvoke<Q, R>(exchange: (packet: Uint8Array) => Uint8Array | Promise<Uint8Array>, binding: Pick<ServiceBinding<Q, R>, 'operation' | 'id' | 'encode' | 'decodeResponse'>, value: Q, options: RpcOptions): Promise<RpcOutcome & {
    readonly value: R;
}>;
export declare function rpcInvoke<Q>(exchange: (packet: Uint8Array) => Uint8Array | Promise<Uint8Array>, binding: Pick<WriteBinding<Q> | CommandBinding<Q>, 'operation' | 'id' | 'encode'>, value: Q, options: RpcOptions): Promise<RpcOutcome & {
    readonly value: undefined;
}>;
export interface RpcBackendOptions extends Omit<RpcOptions, 'transactionId'> {
    readonly ackTimeoutMs?: number;
    readonly pollIntervalMs?: number;
    readonly maxRetries?: number;
}
export declare class RpcBackend implements Backend {
    private readonly exchange;
    private readonly options;
    private executing?;
    constructor(exchange: Exchange, options?: RpcBackendOptions);
    supports(operation: Operation): boolean;
    confirmSessionClosed(generation: number): boolean;
    execute(request: WireRequest, context: BackendContext): Promise<WireResult>;
    private run;
    private delay;
    private attempt;
}
export {};
