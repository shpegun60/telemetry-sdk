/* Runtime session integration over one DeviceClient. Authors: Ruslan Kovtun, codexAi. MIT. */
import type {
	Backend, ByteInput, ClientStatus, Clock, Operation, Result} from '../lib/client/web/Client.js';
import type {RuntimeSchema, RuntimeType} from './telemetry.js';

export const ValueStatus:
    Readonly<{Ok : 'Ok'; Stale : 'Stale'; NoValue : 'NoValue'; InvalidPayload : 'InvalidPayload'}>;
export type DynamicValueStatus = typeof ValueStatus[keyof typeof ValueStatus];
export type EndpointOperation = Exclude<Operation, 'ReadMany'|'Batch'>;
declare const endpointBrand: unique symbol;
declare const readManyBrand: unique symbol;
export interface EndpointBinding {
	readonly[endpointBrand]: true;
	readonly operation: EndpointOperation;
	readonly id: number;
	readonly session: number;
	readonly requestType: number;
	readonly responseType: number;
}
export interface ReadManyBinding {
	readonly[readManyBrand]: true;
	readonly session: number;
	readonly count: number;
}
export type BindingResult<K extends string, T> = {
	readonly status: 0
}&{readonly[P in K] : T}|{readonly status : Exclude<ClientStatus, 0>};
export interface DynamicResult {
	readonly operation: Result<undefined>;
	readonly valueStatus: DynamicValueStatus;
	readonly value?: unknown;
}
export interface DynamicReadManyItem {
	readonly id: number;
	readonly status: 0|1|2|3|4;
	readonly payload: Uint8Array;
	readonly valueStatus: DynamicValueStatus;
	readonly value?: unknown;
}
export interface DynamicReadManyResult extends DynamicResult {
	readonly items?: readonly DynamicReadManyItem[];
}
export interface DynamicBatchItem extends Result<unknown> {
	readonly id: number;
	readonly valueStatus: DynamicValueStatus;
}
export interface DynamicBatchResult extends DynamicResult {
	readonly items?: readonly DynamicBatchItem[];
}
export interface DynamicRequest<T> extends Promise<T> {
	readonly requestId: number;
	readonly session: number;
	cancel(): boolean;
}
export interface Quiescence {
	readonly session: number;
	readonly txReleased: boolean;
	readonly oldFramesExcluded: boolean;
	readonly remoteSessionClosed: boolean;
}
/**
 * A handle by path: group/.../endpoint, then .member and [i] for part of a Field.
 * read()/write()/call() throw, naming the path, for a wrong kind, a member write or
 * a read-only Field; a member's read() resolves to the member's value.
 */
export interface DynamicEndpointHandle {
	readonly id: number;
	readonly path: string;
	readonly category: 'Field'|'Command'|'Service';
	/** True for a .member or [i] path: readable, never written by itself. */
	readonly member: boolean;
	read(timeoutMs?: number): DynamicRequest<DynamicResult>;
	write(value: unknown, timeoutMs?: number): DynamicRequest<DynamicResult>;
	/** A Command or Service: (request, timeoutMs?), or (timeoutMs?) without a request. */
	call(...args: unknown[]): DynamicRequest<DynamicResult>;
}
export interface DynamicBatchRequest {
	readonly endpoint: EndpointBinding;
	readonly value?: unknown;
}

export class DynamicClient {
	constructor(options: {
		readonly backend: Backend;
		readonly clock?: Clock;
		readonly maxPending?: number;
		readonly maxPayloadBytes?: number;
		readonly timeoutMs?: number;
	});
	get session(): number;
	get schema(): RuntimeSchema|undefined;
	/** The admitted schema's Enum/Struct with this unique v3.1 type name. */
	findType(name: string): RuntimeType|undefined;
	get ready(): boolean;
	get pendingCount(): number;
	get recoveryRequired(): boolean;
	get recoveryGeneration(): number;
	get drained(): boolean;
	admit(model: RuntimeSchema, observedHeader: ByteInput, session: number): ClientStatus;
	invalidate(): void;
	confirmQuiescence(proof: Quiescence): boolean;
	close(): void;
	process(): void;
	bind(operation: EndpointOperation, id: number): BindingResult<'endpoint', EndpointBinding>;
	/** Resolve a section-15 path once; throws `No endpoint at path "..."` and the like. */
	endpoint(path: string): DynamicEndpointHandle;
	readField(endpoint: EndpointBinding, timeoutMs?: number): DynamicRequest<DynamicResult>;
	writeField(endpoint: EndpointBinding, value: unknown,
	           timeoutMs?: number): DynamicRequest<DynamicResult>;
	writeFieldPreserving(endpoint: EndpointBinding, value: unknown, originalPayload: ByteInput,
	                     timeoutMs?: number): DynamicRequest<DynamicResult>;
	command(endpoint: EndpointBinding, value?: unknown,
	        timeoutMs?: number): DynamicRequest<DynamicResult>;
	service(endpoint: EndpointBinding, value?: unknown,
	        timeoutMs?: number): DynamicRequest<DynamicResult>;
	bindReadMany(capability: ByteInput,
	             endpoints: readonly EndpointBinding[]): BindingResult<'binding', ReadManyBinding>;
	readMany(binding: ReadManyBinding, timeoutMs?: number): DynamicRequest<DynamicReadManyResult>;
	batch(capability: ByteInput, requests: readonly DynamicBatchRequest[],
	      timeoutMs?: number): DynamicRequest<DynamicBatchResult>;
}
