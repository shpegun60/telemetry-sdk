// Inferred generated DTO/tuple API. Negative controls must remain strict TypeScript errors. MIT.
import {DeviceClient, EncodedBackend, Batch, BatchValueStatus} from './runtime/Client.js';
import {control, matchesHeader} from './sdk/device.js';
const F = control.fields, C = control.commands, S = control.services;
const client = new DeviceClient({
	backend : new EncodedBackend(
	    () => {
	    },
	    {batch : true}),
	matchesHeader
});
const draft: control.types.Config = {
	number : 7,
	enabled : true,
	gains : [ 1, 2, 3 ],
	mode : -123,
	large : 0xffffffffffffffffn,
	negative : -0x8000000000000000n,
	f32 : 4.25,
	f64 : -0
};
const pending = client.batch(
    new Uint8Array(24), Batch.items(Batch.write(F.Config, draft), Batch.read(F.Counter),
                                    Batch.command(C.Configure, draft), Batch.service(S.Echo, draft),
                                    Batch.command(C.Reset), Batch.service(S.Ping),
                                    Batch.service(S.EmptyService), Batch.read(F.Bool)));
pending.then(result => {
	if (!result.items)
		return;
	if (result.items[1].valueStatus === BatchValueStatus.Ok) {
		const n: number = result.items[1].value;
		void n;
		// @ts-expect-error Counter is a number, not an aggregate.
		const wrong: control.types.Config = result.items[1].value;
		void wrong;
	}
	if (result.items[3].valueStatus === BatchValueStatus.Ok) {
		const large: bigint = result.items[3].value.large;
		void large;
		// @ts-expect-error Exact U64 must not silently become Number.
		const rounded: number = result.items[3].value.large;
		void rounded;
	}
	if (result.items[7].valueStatus === BatchValueStatus.Ok) {
		const flag: boolean = result.items[7].value;
		void flag;
	}
	if (result.items[1].valueStatus === BatchValueStatus.Stale) {
		// @ts-expect-error Stale values are absent even if the operation succeeded.
		const stale: number = result.items[1].value;
		void stale;
	}
	// @ts-expect-error Write items have no value.
	const written: control.types.Config = result.items[0].value;
	void written;
	// @ts-expect-error A Void Service never has a DTO.
	const noValue: control.types.Config = result.items[5].value;
	void noValue;
	// @ts-expect-error Const inference retains eight tuple elements without assertions.
	const ninth = result.items[8];
	void ninth;
});
Batch.read(F.Counter.read);
Batch.write(F.Config.write, draft);
Batch.service(S.EmptyRequest, {});
// @ts-expect-error Category mismatch: a Command cannot be read as a Field.
Batch.read(C.Configure);
// @ts-expect-error Category mismatch: a Field cannot be called as a Command.
Batch.command(F.Config, draft);
// @ts-expect-error A read-only generated Field has no write binding.
Batch.write(F.Counter, 5);
// @ts-expect-error A Command requires its complete request DTO.
Batch.command(C.Configure);
// @ts-expect-error Void Command accepts no extra argument.
Batch.command(C.Reset, draft);
// @ts-expect-error Void Service accepts no extra argument.
Batch.service(S.Ping, draft);
// @ts-expect-error Empty Struct is not Void: its object argument is required.
Batch.service(S.EmptyRequest);
// @ts-expect-error A wrong DTO cannot be coerced to the declared request.
Batch.service(S.Echo, 7);
// @ts-expect-error U64 arguments require BigInt.
Batch.write(F.Config, {...draft, large : 5});
// @ts-expect-error Items are typed operation descriptors, not arbitrary endpoints.
client.batch(new Uint8Array(24), [ F.Counter ]);
