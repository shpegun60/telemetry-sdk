/* Generated (Static) and Dynamic JS endpoint handles, by path, against one native Model.
 * Usage: node Parity.mjs DYNAMIC_CLIENT HANDLES DEVICE CLIENT_RUNTIME DESCRIPTOR BRIDGE
 * Both clients talk to the same native peer (Bridge.cpp). The same paths give the
 * same values and the same errors; device.endpoint(path) is the nested handle.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {readFile} from 'node:fs/promises';
import {createInterface} from 'node:readline';
import {pathToFileURL} from 'node:url';

const [dynamicPath, handlesPath, devicePath, runtimePath, descriptorPath, executable] =
    process.argv.slice(2);
const load = path => import(pathToFileURL(path).href);
const {DynamicClient, ValueStatus} = await load(dynamicPath);
const {parseDescriptor} = await import(new URL('./telemetry.js', pathToFileURL(dynamicPath)));
const {bindDevice} = await load(handlesPath);
const {matchesHeader} = await load(devicePath);
const {DeviceClient, EncodedBackend, Status} = await load(runtimePath);
const descriptor = await readFile(descriptorPath), header = descriptor.subarray(0, 64);
let checks = 0;
const equal = (a, b) => {
	++checks;
	assert.deepEqual(a, b);
};
const truth = a => {
	++checks;
	assert(a);
};
// Static DTOs and runtime values may differ in prototype only; compare their contents.
const plain = value => JSON.stringify(value, (_, v) => typeof v === 'bigint' ? v + 'n' : v);
const thrown = call => {
	try {
		call();
	} catch (error) {
		return error.message;
	}
	return 'no error';
};

const child = spawn(executable, [], {stdio : [ 'pipe', 'pipe', 'pipe' ]});
let pending;
createInterface({input : child.stdout}).on('line', line => {
	const resolve = pending;
	pending = undefined;
	resolve(line);
});
const native = command => new Promise(resolve => {
	assert(!pending, 'one exchange at a time');
	pending = resolve;
	child.stdin.write(command + '\n');
});
const backend = () => EncodedBackend.fromExchange(
    async bytes =>
        Uint8Array.from(Buffer.from(await native(Buffer.from(bytes).toString('hex')), 'hex')));
const staticClient =
    new DeviceClient({backend : backend(), matchesHeader, timeoutMs : 5000, maxPending : 16});
const dynamic = new DynamicClient({backend : backend(), timeoutMs : 5000, maxPending : 16});
try {
	truth(staticClient.openSession(17, header));
	equal(dynamic.admit(parseDescriptor(descriptor), header, 17), Status.Ok);
	const device = bindDevice(staticClient);

	// Every nested handle is the handle of its path; the path forms agree.
	const nested = [];
	const walk = node => {
		for (const value of Object.values(node))
			if (value && typeof value === 'object')
				(typeof value.path === 'string' ? nested.push(value) : walk(value));
	};
	walk(device);
	equal(nested.length, 7);
	for (const handle of nested) {
		truth(device.endpoint(handle.path) === handle && Object.isFrozen(handle));
		const runtime = dynamic.endpoint(handle.path);
		equal([ handle.id, handle.category ], [ runtime.id, runtime.category ]);
	}
	const gain1 = device.adc.channel1.Gain; // a handle kept in a variable
	truth(gain1 === device.endpoint('adc/channel1/Gain'));

	// Values: every Field, then members of the static whole value.
	for (const path of ['adc/Config', 'adc/Status', 'adc/channel1/Gain', 'adc/channel2/Gain',
	                    'metrology/Config']) {
		const a = await device.read(path), b = await dynamic.endpoint(path).read();
		equal([ a.succeeded, b.operation.succeeded, b.valueStatus ],
		      [ true, true, ValueStatus.Ok ]);
		equal(plain(a.value), plain(b.value));
		equal(plain((await device.endpoint(path).read()).value), plain(a.value));
	}
	const config = (await device.read('adc/Config')).value;
	const status = (await device.read('adc/Status')).value;
	const metrology = (await device.read('metrology/Config')).value;
	for (const [path, expected] of [[ 'adc/Config.rateHz', config.rateHz ],
	                                [ 'adc/Status.raw[1]', status.raw[1] ],
	                                [ 'metrology/Config.window', metrology.window ]]) {
		const member = dynamic.endpoint(path);
		truth(member.member && member.category === 'Field');
		equal((await member.read()).value, expected);
	}

	// A write through one client reads back through the other.
	truth((await gain1.write(4.5)).succeeded);
	equal((await dynamic.endpoint('adc/channel1/Gain').read()).value, 4.5);
	const written = {gain : 0.25, rateHz : 2000, enabled : false};
	truth((await dynamic.endpoint('adc/Config').write(written)).operation.succeeded);
	equal(plain((await device.adc.Config.read()).value), plain(written));

	// Commands and Services.
	const before = JSON.parse(await native('state'));
	truth((await device.call('adc/Reset')).executed);
	truth((await dynamic.endpoint('adc/Reset').call()).operation.executed);
	const after = JSON.parse(await native('state'));
	equal(after.resets, before.resets + 2);
	const offset = await device.call('adc/Calibrate', {volts : 2});
	const runtimeOffset = await dynamic.endpoint('adc/Calibrate').call({volts : 2});
	equal(plain(offset.value), plain({counts : 4}));
	equal(plain(runtimeOffset.value), plain(offset.value));

	// Refusals: the same wording from both clients, naming the path.
	const shared = [
		[
			'adc/channel9/Gain', 'endpoint', () => device.endpoint('adc/channel9/Gain'),
			() => dynamic.endpoint('adc/channel9/Gain')
		],
		[
			'adc/Reset', 'read', () => device.read('adc/Reset'),
			() => dynamic.endpoint('adc/Reset').read()
		],
		[
			'adc/Calibrate', 'write', () => device.write('adc/Calibrate', {volts : 1}),
			() => dynamic.endpoint('adc/Calibrate').write({volts : 1})
		],
		[
			'adc/Config', 'call', () => device.call('adc/Config'),
			() => dynamic.endpoint('adc/Config').call()
		],
		[
			'adc/Status', 'write', () => device.write('adc/Status', status),
			() => dynamic.endpoint('adc/Status').write(status)
		],
	];
	for (const [path, method, viaStatic, viaDynamic] of shared) {
		const message = thrown(viaDynamic);
		equal(thrown(viaStatic), message);
		truth(message.includes(`"${path}"`));
		console.log(`error ${path} ${method} ${message}`);
	}
	// Member paths are the Dynamic client's: the generated device has no member path form.
	for (const [path, method, viaDynamic] of [
	         [ 'adc/Config.nope', 'endpoint', () => dynamic.endpoint('adc/Config.nope') ],
	         [ 'adc/Status.raw[5]', 'endpoint', () => dynamic.endpoint('adc/Status.raw[5]') ],
	         [ 'adc/Calibrate.volts', 'endpoint', () => dynamic.endpoint('adc/Calibrate.volts') ],
	         [ 'adc/Config.rateHz', 'write', () => dynamic.endpoint('adc/Config.rateHz').write(1) ],
	]) {
		const message = thrown(viaDynamic);
		truth(message.includes(`"${path}"`));
		console.log(`error ${path} ${method} ${message}`);
	}
	equal(thrown(() => device.endpoint('adc/Config.rateHz')),
	      'No endpoint at path "adc/Config.rateHz"');

	// A Dynamic handle refuses after its admission ends; resolving then throws.
	const kept = dynamic.endpoint('adc/channel2/Gain');
	dynamic.invalidate();
	equal((await kept.read()).operation.status, Status.NotAdmitted);
	equal(thrown(() => dynamic.endpoint('adc/Config')),
	      'No admitted descriptor to resolve path "adc/Config"');
	console.log(`handles parity checks=${checks} failures=0`);
} finally {
	child.stdin.end();
}
