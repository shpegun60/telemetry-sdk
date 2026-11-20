// Strict TypeScript over the generated handles of the group-tree Model: the path
// forms are checked at compile time, and every @ts-expect-error must be an error.
// Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
import {bindDevice, type FieldPath, type Path, type ValueAt} from './sdk/ts/handles.js';
import type {DeviceClient} from './sdk/ts/client.js';
import type * as api from './sdk/ts/device.js';

export async function usage(client: DeviceClient): Promise<void> {
	const device = bindDevice(client);
	const gain1 = device.adc.channel1.Gain; // a handle kept in a variable
	const same: typeof gain1 = device.endpoint('adc/channel1/Gain');
	void same;
	const gain: number | undefined = (await gain1.read()).value;
	await gain1.write((gain ?? 0) + 1);
	const config: api.types.adc.Config | undefined = (await device.read('adc/Config')).value;
	if (config !== undefined)
		await device.write('adc/Config', {...config, rateHz : 2000});
	const window: api.types.metrology.Window | undefined =
	    (await device.metrology.Config.read()).value?.window;
	void window;
	await device.call('adc/Reset');
	await device.adc.Reset.call(1000);
	const offset: api.types.adc.Offset | undefined =
	    (await device.call('adc/Calibrate', {volts : 2})).value;
	void offset;
	const paths: Path[] = [ 'adc/Config', 'adc/channel2/Gain', 'metrology/Config', 'adc/Calibrate' ];
	const fields: FieldPath[] = [ 'adc/Status', 'adc/channel1/Gain' ];
	const value: ValueAt<'adc/Status'> = {flags : 1, raw : [ 2, 3 ]};
	void paths, fields, value;

	// @ts-expect-error not a path of this device
	device.endpoint('adc/channel9/Gain');
	// @ts-expect-error a member path is the Dynamic client's
	device.read('adc/Config.rateHz');
	// @ts-expect-error read takes a Field
	device.read('adc/Reset');
	// @ts-expect-error a read-only Field has no write
	device.write('adc/Status', {flags : 0, raw : [ 0, 0 ]});
	// @ts-expect-error call takes a Command or Service
	device.call('adc/Config');
	// @ts-expect-error the value type of adc/channel1/Gain is a number
	device.write('adc/channel1/Gain', 'loud');
	// @ts-expect-error Calibrate takes a request
	device.call('adc/Calibrate');
	// @ts-expect-error the nested read-only handle has no write
	device.adc.Status_value.write({flags : 0, raw : [ 0, 0 ]});
	// @ts-expect-error a Command handle has no read
	device.adc.Reset.read();
}
