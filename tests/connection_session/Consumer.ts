// Public installed types: generated static inference and deliberately unknown Dynamic values. MIT.
import {connectStatic, connectDynamic, Status, Batch} from 'telemetry-sdk';
import {type ConnectionProfile, type SessionResult, type SessionRequest, type StaticSdk} from 'telemetry-sdk/session';
import {demoWebSocket, bindWebSocket} from 'telemetry-sdk/transports/websocket';
import {bindWebSocket as borrowedCarrier} from 'telemetry-sdk/transports/websocket/carrier';
import * as sdk from './generated/js/device.js';

export async function staticConsumer(transport: ConnectionProfile) {
    const session = await connectStatic({sdk, transport});
	const generatedContext: StaticSdk = session.sdk;
    const read: SessionRequest<sdk.control.types.Config> = session.read(sdk.control.Config);
    const id: number = read.requestId, generation: number = read.session;
    const cancelled: boolean = read.cancel();
    const result: SessionResult<sdk.control.types.Config> = await read;
    const status: number = result.rawResult.status;
    if (result.valueStatus === 'Ok' && result.value !== undefined) {
        const number: number = result.value.number, large: bigint = result.value.large;
        void number; void large;
    }
    session.write(sdk.control.Config, {number: 1, enabled: true, gains: [1, 2, 3],
        mode: -123, large: 1n, negative: -1n, f32: 0, f64: -0});
	const preserving: SessionRequest<undefined> = session.writePreserving(sdk.control.Config,
	    {number: 1, enabled: true, gains: [1, 2, 3], mode: -123, large: 1n,
	     negative: -1n, f32: 0, f64: -0}, new Uint8Array(41));
    session.command(sdk.control.Reset, undefined);
    session.service(sdk.control.Echo, {number: 2, enabled: true, gains: [1, 2, 3],
        mode: -123, large: 1n, negative: -1n, f32: 0, f64: 0});
    // @ts-expect-error Static reads require a generated Field binding rather than a numeric ID.
    session.read(65536);
    // @ts-expect-error Generated Config DTO cannot be replaced with an arbitrary number.
    session.write(sdk.control.Config, 123);
	// @ts-expect-error Preserving writes retain generated DTO inference.
	session.writePreserving(sdk.control.Config, 123, new Uint8Array(41));
	// @ts-expect-error Original payload must be bytes, not serialized text.
	session.writePreserving(sdk.control.Config, result.value!, '00');
	// @ts-expect-error Generated context cannot be replaced after connection.
	session.sdk = sdk;
    // @ts-expect-error Static inference retains exact bigint leaves.
    const rounded: number = result.value!.large;
    // @ts-expect-error Core is private; no untracked lifecycle/execution escape hatch.
    session.client.close();
    const unsubscribe = session.subscribe(snapshot => { const ready: boolean = snapshot.ready; void ready; });
    unsubscribe(); await session.disconnect(); await session.reconnect(); await session.close();
    void id; void generation; void cancelled; void status; void rounded;
	void generatedContext; void preserving;
}
export async function dynamicConsumer(transport: ConnectionProfile) {
    const session = await connectDynamic({transport});
    const read: SessionRequest<unknown> = session.read(65536);
    const result = await read;
    if (result.status === Status.Ok && typeof result.value === 'object' && result.value !== null &&
        'number' in result.value && typeof result.value.number === 'number') {
        const number: number = result.value.number; void number;
    }
    // @ts-expect-error Runtime values remain unknown without application narrowing.
    const guessed: sdk.control.types.Config = result.value;
    // @ts-expect-error Dynamic API deliberately uses current runtime IDs, not generated static bindings.
    session.read(sdk.control.Config);
    session.write(65536, {}); session.command(65537); session.service(65537);
	const preserving: SessionRequest<undefined> = session.writePreserving(65536, {}, new DataView(new ArrayBuffer(41)));
	// @ts-expect-error Dynamic preserving writes use numeric runtime IDs.
	session.writePreserving(sdk.control.Config, {}, new Uint8Array(41));
	// @ts-expect-error Dynamic Session has no generated SDK context.
	void session.sdk;
    const schema = session.schema, capabilities = session.capabilities;
    void guessed; void schema; void capabilities;
	void preserving;
    await session.close();
}
const reference: ConnectionProfile = demoWebSocket('http://localhost:8080');
void reference; void bindWebSocket; void borrowedCarrier;

export async function optionalConsumer(transport: ConnectionProfile, capability: Uint8Array)
{
	const session = await connectStatic({sdk, transport});
	const selected = await session.readMany(
	    capability, [ sdk.control.Counter, sdk.control.Bool ] as const);
	if (selected.items) {
		const count: number|undefined = selected.items[0].value;
		const flag: boolean|undefined = selected.items[1].value;
		// @ts-expect-error The first item's scalar type is not boolean.
		const wrong: boolean = selected.items[0].value!;
		void count;
		void flag;
		void wrong;
	}
	const result =
	    await session.batch(capability, Batch.items(Batch.read(sdk.control.Config),
	                                                Batch.command(sdk.control.Reset),
	                                                Batch.service(sdk.control.Echo, {
		                                                number : 1,
		                                                enabled : false,
		                                                gains : [ 1, 2, 3 ],
		                                                mode : -123,
		                                                large : 1n,
		                                                negative : -1n,
		                                                f32 : 0,
		                                                f64 : 0
	                                                })));
	if (result.items) {
		const first: sdk.control.types.Config|undefined = result.items[0].value;
		const absent: undefined = result.items[1].value;
		const echo: sdk.control.types.Config|undefined = result.items[2].value;
		// @ts-expect-error Batch keeps the service DTO, not a scalar.
		const wrong: number = result.items[2].value!;
		void first;
		void absent;
		void echo;
		void wrong;
	}
	// @ts-expect-error ReadMany rejects aggregate fields at the Static type boundary.
	session.readMany(capability, [ sdk.control.Config ]);
	// @ts-expect-error Static Batch requires branded typed selectors, not runtime recipes.
	session.batch(capability, [ {operation : 'Command', id : 1} ]);
	const dynamic = await connectDynamic({transport});
	const runtime = await dynamic.batch(
	    capability, [ {operation : 'FieldRead', id : 65537}, {operation : 'Command', id : 65537} ]);
	// @ts-expect-error Dynamic item values remain unknown.
	const guessed: number = runtime.items![0].value;
	// @ts-expect-error Numeric runtime IDs are required for Dynamic ReadMany.
	dynamic.readMany(capability, [ sdk.control.Counter ]);
	void guessed;
	await session.close();
	await dynamic.close();
}
