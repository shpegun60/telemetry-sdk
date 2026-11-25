// Compile the ordinary public SDK surface; no payload assertions or casts. MIT.
import {DeviceClient, Batch} from './lib/client/web/Client.js';
import {DynamicClient} from './web/DynamicClient.js';
import {parseDescriptor} from './web/telemetry.js';
import {control} from './generated/device.js';
import {structuralMetadata} from './generated/device.metadata.js';

export async function configure(client: DeviceClient, trm1: Uint8Array, tbm1: Uint8Array)
{
	const draft: control.types.Config = {
		number : 37,
		enabled : true,
		gains : [ 0, 65535, 17 ],
		mode : -123,
		large : 0xffffffffffffffffn,
		negative : -0x8000000000000000n,
		f32 : 4.25,
		f64 : -0
	};
	await client.write(control.Config, draft);
	const read = await client.read(control.Config);
	const number: number|undefined = read.value?.number;
	await client.command(control.Configure, draft);
	const response = await client.service(control.Echo, draft);
	const large: bigint|undefined = response.value?.large;
	const watch = client.bindReadMany(trm1, [ control.Counter, control.Bool ]);
	if (watch.status === 0) {
		const values = await client.readMany(watch.binding);
		if (values.value?.[0].status === 0) {
			const counter: number = values.value[0].value;
			void counter;
		}
		if (values.value?.[1].status === 0) {
			const flag: boolean = values.value[1].value;
			void flag;
		}
	}
	const batch =
	    await client.batch(tbm1, Batch.items(Batch.write(control.Config, draft),
	                                         Batch.read(control.Config),
	                                         Batch.command(control.Accepted, draft),
	                                         Batch.service(control.Echo, draft)));
	if (batch.items?.[1].valueStatus === 'Ok') {
		const value: number = batch.items[1].value.number;
		void value;
	}
	// @ts-expect-error Static DTOs require their exact declared member types.
	await client.write(control.Config, {...draft, large : 7});
	// @ts-expect-error Read-only fields cannot be written.
	Batch.write(control.Counter, 42);
	const category: 1|2|3|undefined = structuralMetadata.endpoints[0]?.category;
	return {number, large, category};
}

export async function inspect(client: DynamicClient, bytes: Uint8Array)
{
	const schema = parseDescriptor(bytes);
	client.admit(schema, bytes.subarray(0, 64), 17);
	const field = client.bind('FieldRead', 65536);
	if (field.status === 0) {
		const result = await client.readField(field.endpoint);
		// Runtime DTOs are unknown until narrowed by the application.
		// @ts-expect-error Dynamic data is not a generated static DTO.
		const wrong: number = result.value.number;
		void wrong;
		if (typeof result.value === 'object' && result.value !== null && 'number' in result.value)
			return result.value.number;
	}
	return undefined;
}
