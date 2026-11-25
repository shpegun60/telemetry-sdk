// ReadMany and ordinary operations share one actual persistent C++ Encoded server. MIT.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {readFile} from 'node:fs/promises';
import {createInterface} from 'node:readline';
import {pathToFileURL} from 'node:url';

const [runtimePath, sdkPath, descriptorPath, capabilityPath, executable] = process.argv.slice(2);
const {DeviceClient, EncodedBackend, Status} = await import(pathToFileURL(runtimePath).href);
const {scalar, extra, control, matchesHeader} = await import(pathToFileURL(sdkPath).href);
const declared = (await readFile(descriptorPath)).subarray(0, 64);
const capability = await readFile(capabilityPath);
let checks = 0, packets = 0;
const equal = (a, b) => {
	++checks;
	assert.deepEqual(a, b);
};
const truth = a => {
	++checks;
	assert(a);
};
const child = spawn(executable, [], {stdio : [ 'pipe', 'pipe', 'pipe' ]});
let pending, stderr = '';
child.stderr.on('data', data => stderr += data);
createInterface({input : child.stdout}).on('line', line => {
	assert(pending, 'unexpected native reply');
	const resolve = pending;
	pending = undefined;
	resolve(Uint8Array.from(Buffer.from(line, 'hex')));
});
child.on('error', error => {
	throw error;
});
child.on('exit', code => {
	if (pending)
		throw Error(`Native server exited ${code}: ${stderr}`);
});
const opcodes = [];
const backend = EncodedBackend.fromExchange(packet => {
	assert(!pending, 'second physical invocation');
	++packets;
	opcodes.push(packet[6]);
	return new Promise(resolve => {
		pending = resolve;
		child.stdin.write(Buffer.from(packet).toString('hex') + '\n');
	});
}, {readMany : true});
const client = new DeviceClient({backend, matchesHeader, timeoutMs : 5000, maxPending : 16});
const bind = selected => {
	const b = client.bindReadMany(capability, selected);
	equal(b.status, Status.Ok);
	return b.binding;
};
try {
	truth(client.openSession(17, declared));
	const selected = Array.from({length : 32}, (_, i) => scalar[`F${String(i).padStart(2, '0')}`]);
	const all = bind(selected);
	const values = await client.readMany(all);
	truth(values.succeeded);
	equal(values.value.length, 32);
	values.value.forEach((item, i) => {
		equal(item.status, 0);
		equal(item.value, 100 + i);
	});
	const mixed = bind([
		extra.Wide, extra.Signed, extra.Enabled, extra.Mode, extra.Real, extra.Double, scalar.F04,
		extra.Absent
	]);
	const result = await client.readMany(mixed);
	truth(result.succeeded);
	equal(result.value[0].value, 0xffffffffffffffffn);
	equal(result.value[1].value, -0x8000000000000000n);
	equal(result.value[2].value, true);
	equal(result.value[3].value, -123);
	truth(Object.is(result.value[4].value, -0));
	truth(Number.isNaN(result.value[5].value));
	equal(new DataView(result.value[5].payload.buffer).getBigUint64(0, true), 0x7ff8000012345678n);
	equal(result.value[6].value, 104);
	equal(result.value[7].status, 3);
	equal(result.value[7].value, undefined);
	equal(client.bindReadMany(capability, [ extra.Aggregate ]).status, Status.Unsupported);
	equal(extra.Aggregate.read.responseBytes, 4);
	equal(extra.Aggregate.read.readManyScalar, undefined);
	const unknown = {...scalar.F00.read, id : 0xffffffff};
	const aggregate = {...scalar.F00.read, id : extra.Aggregate.id};
	const refusals = await client.readMany(bind([ unknown, aggregate ]));
	equal(refusals.value.map(item => item.status), [ 1, 2 ]);
	// All five operations enter the same FIFO; a batch observes preceding mutations.
	const one = bind([ scalar.F00 ]);
	const promises = [
		client.write(scalar.F00, 700), client.readMany(one),
		client.command(control.Configure, {value : 900}), client.readMany(one),
		client.service(control.Echo, {value : 123}), client.read(scalar.F00)
	];
	const outcomes = await Promise.all(promises);
	truth(outcomes.every(outcome => outcome.succeeded));
	equal(outcomes[1].value[0].value, 700);
	truth(outcomes[2].accepted);
	truth(!outcomes[2].executed);
	equal(outcomes[3].value[0].value, 900);
	equal(outcomes[4].value.value, 123);
	equal(outcomes[5].value, 900);
	equal(opcodes.slice(-6), [ 1, 5, 2, 5, 3, 4 ]);
	equal(result.value[6].value, 104);
	equal(client.pendingCount, 0);
	client.close();
	truth(client.drained);
} finally {
	child.stdin.end();
	const code = await new Promise(resolve => child.once('exit', resolve));
	equal(code, 0);
	equal(stderr, '');
}
console.log(`ReadMany integration checks=${checks} packets=${packets} failures=0`);
