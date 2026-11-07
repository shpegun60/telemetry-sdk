// Generated bindings, inferred ordered values and strict negative API controls. MIT.
import {
	DeviceClient,
	EncodedBackend,
	Status,
	inspectReadManyCapability,
	type ReadManyItem,
	type RequestPromise,
	type ReadBinding
} from './runtime/Client.js';
import {scalar, extra, control, matchesHeader} from './sdk/device.js';

const capacity = inspectReadManyCapability(new Uint8Array(24));
if (capacity) {
	const exact: bigint = capacity.fingerprint;
	void exact;
	// @ts-expect-error Planning metadata is immutable and does not grant admission.
	capacity.itemLimit = 32;
}

const client = new DeviceClient({
	backend : new EncodedBackend(
	    () => {
	    },
	    {readMany : true}),
	matchesHeader
});
const bound = client.bindReadMany(new Uint8Array(24),
                                  [ scalar.fields.F00, extra.fields.Wide, extra.fields.Enabled ]);
if (bound.status === Status.Ok) {
	const pending: RequestPromise<
	    readonly[ReadManyItem<number>, ReadManyItem<bigint>, ReadManyItem<boolean>]> =
	    client.readMany(bound.binding);
	pending.then(result => {
		if (result.value?.[1].status === 0) {
			const exact: bigint = result.value[1].value;
			void exact;
			// @ts-expect-error U64 never silently becomes a Number.
			const rounded: number = result.value[1].value;
			void rounded;
		}
		if (result.value?.[0].status === 3) {
			// @ts-expect-error Unavailable has no typed value.
			const absent: number = result.value[0].value;
			void absent;
		}
	});
}
// @ts-expect-error A four-byte aggregate is not a scalar.
client.bindReadMany(new Uint8Array(24), [ extra.fields.Aggregate ]);
// @ts-expect-error Commands cannot be read as Fields.
client.bindReadMany(new Uint8Array(24), [ control.commands.Configure ]);
const legacy: ReadBinding<number> = scalar.fields.F00.read;
// @ts-expect-error Old bindings without explicit eligibility remain ineligible.
client.bindReadMany(new Uint8Array(24), [ legacy ]);
// @ts-expect-error A caller cannot construct a binding from public metadata.
client.readMany({session : 1, fingerprint : 0n, count : 1});
