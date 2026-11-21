/* Validated Descriptor and owned runtime payload types. Authors: Ruslan Kovtun, codexAi. MIT. */
import type {ByteInput} from '../lib/client/web/Client.js';

export const TypeKind: Readonly<{Void : 0; Scalar : 1; Enum : 2; Struct : 3; Array : 4}>;
export interface Limits {
	readonly descriptorBytes: number;
	readonly typeCount: number;
	readonly structMembers: number;
	readonly arrayElements: number;
	readonly stringBytes: number;
	readonly enumEntries: number;
	readonly catalogCount: number;
	readonly endpointCount: number;
	readonly valueWireBytes: number;
	readonly typeDepth: number;
	readonly expandedValueNodes: number;
	/** Group-tree ceilings; hosts default to 255 levels and 4096-byte names and paths. */
	readonly groupDepth: number;
	readonly nameBytes: number;
	readonly pathBytes: number;
}
export const defaultLimits: Limits;

export class TelemetryFormatError extends Error {
	constructor(reason: string);
}

interface TypeBase {
	readonly id: number;
	readonly wireBytes: number;
	readonly depth: number;
	readonly nodes: number;
	readonly offset: number;
}
export interface RuntimeEnumEntry {
	readonly code: number|bigint;
	readonly name: string;
}
export interface RuntimeStructMember {
	readonly typeId: number;
	readonly name: string;
}
export type RuntimeType = TypeBase&({readonly kind : 0}|{
	readonly kind: 1;
	readonly scalarCode: 1|2|3|4|5|6|7|8|9|10|11
}|{
	readonly kind: 2;
	/** v3.1 unique type name; absent when the enum is unnamed. */
	readonly name?: string;
	readonly underlyingTypeId: number;
	readonly entries: readonly RuntimeEnumEntry[]
}|{
	readonly kind: 3;
	/** v3.1 unique type name; absent when the struct is unnamed. */
	readonly name?: string;
	readonly members: readonly RuntimeStructMember[]
}|{
	readonly kind: 4;
	readonly elementTypeId: number;
	readonly elementCount: number
});
export interface Catalog {
	readonly group: number;
	readonly first: number;
	readonly count: number;
	readonly name: string;
	readonly offset: number;
}
interface EndpointBase {
	readonly id: number;
	readonly name: string;
	readonly offset: number;
}
export interface RuntimeField extends EndpointBase {
	readonly typeId: number;
	readonly capabilities: 1|3;
}
export interface RuntimeCommand extends EndpointBase {
	readonly requestTypeId: number;
}
export interface RuntimeService extends RuntimeCommand {
	readonly responseTypeId: number;
}
/** One group of a GroupTree descriptor (v3.1), in pre-order. */
export interface RuntimeGroup {
	readonly index: number;
	readonly name: string;
	readonly depth: number;
	/** The parent group's index; undefined at a root. */
	readonly parent: number|undefined;
	/** group/.../group */
	readonly path: string;
	readonly fieldCount: number;
	readonly commandCount: number;
	readonly serviceCount: number;
	readonly firstField: number;
	readonly firstCommand: number;
	readonly firstService: number;
	readonly offset: number;
}
declare const validatedModel: unique symbol;
export interface RuntimeSchema {
	readonly[validatedModel]: true;
	readonly fingerprint: bigint;
	readonly byteLength: number;
	/** 0 without type names and attributes (v3.0), 1 otherwise (v3.1). */
	readonly minor: 0|1;
	/** v3.1 GroupTree mode: one shared tree of groups replaces the family catalogs. */
	readonly groupTree: boolean;
	/** The tree's groups in pre-order; undefined for a flat descriptor. */
	readonly groups: readonly RuntimeGroup[]|undefined;
	readonly types: readonly RuntimeType[];
	readonly fieldCatalogs: readonly Catalog[];
	readonly commandCatalogs: readonly Catalog[];
	readonly serviceCatalogs: readonly Catalog[];
	readonly fields: readonly RuntimeField[];
	readonly commands: readonly RuntimeCommand[];
	readonly services: readonly RuntimeService[];
}
export function descriptorFingerprint(input: ByteInput): bigint;
export function parseDescriptor(input: ByteInput, overrides?: Partial<Limits>): RuntimeSchema;
export function matchesDescriptorHeader(model: RuntimeSchema, input: ByteInput): boolean;
/** The Enum/Struct with this unique v3.1 type name, or undefined. */
export function findType(model: RuntimeSchema, name: string): RuntimeType|undefined;
/** A flat name as one path segment: / . [ ] and backslash escaped with a backslash. */
export function escapePathSegment(name: string): string;
/** The group of a GroupTree descriptor at group/.../group, or undefined. */
export function findGroup(model: RuntimeSchema, path: string): RuntimeGroup|undefined;
export interface EndpointMatch {
	/** 0 Field, 1 Command, 2 Service. */
	readonly category: 0|1|2;
	readonly endpoint: RuntimeField|RuntimeCommand|RuntimeService;
	/** The .member and [i] suffix after the endpoint, '' for the endpoint itself. */
	readonly member: string;
}
/** The endpoint at group/.../endpoint[.member][[i]], or undefined (unknown or ambiguous). */
export function findEndpoint(model: RuntimeSchema, path: string): EndpointMatch|undefined;
/** group/.../endpoint in a tree, escaped group/endpoint in a flat descriptor. */
export function endpointPath(model: RuntimeSchema, category: 0|1|2,
                             endpoint: RuntimeField|RuntimeCommand|RuntimeService): string;
export function encodeValue(model: RuntimeSchema, typeId: number, value: unknown): Uint8Array;
/** Preserve original IEEE NaN wire bits when the corresponding edited leaf remains NaN. */
export function encodeValuePreserving(model: RuntimeSchema, typeId: number, value: unknown,
                                      originalPayload: ByteInput): Uint8Array;
export function decodeValue(model: RuntimeSchema, typeId: number, input: ByteInput): unknown;
export function encodeFieldWrite(model: RuntimeSchema, id: number, value: unknown): Uint8Array;
export function decodeFieldValue(model: RuntimeSchema, id: number, input: ByteInput): unknown;
export function encodeCommandRequest(model: RuntimeSchema, id: number, value?: unknown): Uint8Array;
export function encodeServiceRequest(model: RuntimeSchema, id: number, value?: unknown): Uint8Array;
export function decodeServiceResponse(model: RuntimeSchema, id: number, input: ByteInput): unknown;
export function decodeValues(model: RuntimeSchema, input: ByteInput):
    {readonly id: number; readonly status : 0 | 1; readonly value : unknown}[];

export class DescriptorCache {
	constructor(capacity?: number);
	load(input: ByteInput, limits?: Partial<Limits>): RuntimeSchema;
	clear(): void;
	get size(): number;
	get capacity(): number;
}
