// Actual native dispatcher parity between generated static and runtime Descriptor clients. MIT.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {readFile} from 'node:fs/promises';
import {createInterface} from 'node:readline';
import {pathToFileURL} from 'node:url';

const [dynamicPath, runtimePath, sdkPath, descriptorPath, trmPath, tbmPath, executable] =
    process.argv.slice(2);
const {DynamicClient, ValueStatus : V} = await import(pathToFileURL(dynamicPath).href);
const {parseDescriptor} = await import(new URL('./telemetry.js', pathToFileURL(dynamicPath)));
const {DeviceClient, EncodedBackend, Status, Batch} = await import(pathToFileURL(runtimePath).href);
const {control, matchesHeader} = await import(pathToFileURL(sdkPath).href);
const descriptor = await readFile(descriptorPath), header = descriptor.subarray(0, 64);
const trm = await readFile(trmPath), tbm = await readFile(tbmPath);
const schema = parseDescriptor(descriptor);
let checks = 0, packets = 0;
const equal = (a, b) => {
	++checks;
	assert.deepEqual(a, b);
};
const truth = a => {
	++checks;
	assert(a);
};
const dto = number => ({
	number,
	enabled : true,
	gains : [ 0, 65535, 17 ],
	mode : -123,
	large : 0xffffffffffffffffn,
	negative : -0x8000000000000000n,
	f32 : NaN,
	f64 : -0
});
const child = spawn(executable, [], {stdio : [ 'pipe', 'pipe', 'pipe' ]});
let pending, stderr = '';
child.stderr.on('data', data => stderr += data);
createInterface({input : child.stdout}).on('line', line => {
	assert(pending);
	const resolve = pending;
	pending = undefined;
	resolve(line);
});
const exited = new Promise((resolve, reject) => {
	child.once('error', reject);
	child.once('exit', code => pending ? reject(Error(`Peer ${code}: ${stderr}`)) : resolve(code));
});
const native = command => new Promise(resolve => {
	assert(!pending, 'A second physical exchange bypassed the shared FIFO');
	pending = resolve;
	child.stdin.write(command + '\n');
});
const sent = [], replies = [];
const backend = () => EncodedBackend.fromExchange(async bytes => {
	++packets;
	sent.push(bytes.slice());
	const reply =
	    Uint8Array.from(Buffer.from(await native(Buffer.from(bytes).toString('hex')), 'hex'));
	replies.push(reply.slice());
	return reply;
}, {readMany : true, batch : true});
const bind = (client, operation, id) => {
	const result = client.bind(operation, id);
	equal(result.status, Status.Ok);
	return result.endpoint;
};
const staticClient =
    new DeviceClient({backend : backend(), matchesHeader, timeoutMs : 5000, maxPending : 16});
const dynamicClient = new DynamicClient({backend : backend(), timeoutMs : 5000, maxPending : 16});
try {
	truth(staticClient.openSession(17, header));
	equal(dynamicClient.admit(schema, header, 17), Status.Ok);
	equal(dynamicClient.schema, schema);
	equal(dynamicClient.session, 17);
	const F = control, C = control, S = control;
	const staticWatch = staticClient.bindReadMany(trm, [ F.Counter, F.Bool ]);
	equal(staticWatch.status, Status.Ok);
	const staticResults = [
		await staticClient.read(F.Config), await staticClient.write(F.Config, dto(1111)),
		await staticClient.command(C.Configure, dto(37)),
		await staticClient.service(S.Echo, dto(99)),
		await staticClient.readMany(staticWatch.binding),
		await staticClient.batch(
		    tbm, Batch.items(Batch.write(F.Config, dto(888)), Batch.read(F.Config),
		                     Batch.command(C.Accepted, dto(37)), Batch.service(S.Busy, dto(99))))
	];
	const staticPackets = sent.splice(0), staticReplies = replies.splice(0);
	const staticState = JSON.parse(await native('state'));
	await native('reset');
	const read = bind(dynamicClient, 'FieldRead', 65536),
	      write = bind(dynamicClient, 'FieldWrite', 65536);
	const command = bind(dynamicClient, 'Command', 65536),
	      service = bind(dynamicClient, 'Service', 65536);
	const counter = bind(dynamicClient, 'FieldRead', 65537),
	      flag = bind(dynamicClient, 'FieldRead', 65540);
	const accepted = bind(dynamicClient, 'Command', 65538),
	      busy = bind(dynamicClient, 'Service', 65539);
	const watch = dynamicClient.bindReadMany(trm, [ counter, flag ]);
	equal(watch.status, Status.Ok);
	// These six calls are the normal runtime API: no generated DTOs or handwritten payloads.
	const dynamicResults = [
		await dynamicClient.readField(read), await dynamicClient.writeField(write, dto(1111)),
		await dynamicClient.command(command, dto(37)),
		await dynamicClient.service(service, dto(99)), await dynamicClient.readMany(watch.binding),
		await dynamicClient.batch(tbm,
		                          [
			                          {endpoint : write, value : dto(888)}, {endpoint : read},
			                          {endpoint : accepted, value : dto(37)},
			                          {endpoint : busy, value : dto(99)}
		                          ])
	];
	equal(sent, staticPackets);
	equal(replies, staticReplies);
	equal(JSON.parse(await native('state')), staticState);
	for (let i = 0; i < 6; ++i) {
		const actual = dynamicResults[i], expected = staticResults[i];
		for (const property
		         of ['status', 'dispatch', 'endpointStatus', 'succeeded', 'accepted', 'executed'])
			equal(actual.operation[property], expected[property]);
		equal(actual.operation.payload, expected.payload);
		equal(actual.operation.operation, expected.operation);
	}
	equal(dynamicResults[0].value.number, 42);
	equal(dynamicResults[0].value.large, 0xffffffffffffffffn);
	equal(dynamicResults[0].value.negative, -0x8000000000000000n);
	truth(Number.isNaN(dynamicResults[0].value.f32));
	truth(Object.is(dynamicResults[0].value.f64, -0));
	equal(new DataView(dynamicResults[0].operation.payload.buffer).getUint32(29, true), 0xff812345);
	equal(dynamicResults[1].valueStatus, V.NoValue);
	equal(dynamicResults[2].valueStatus, V.NoValue);
	equal(dynamicResults[3].value.number, 99);
	equal(dynamicResults[4].items.map(i => i.value), [ 112, true ]);
	equal(dynamicResults[5].items[1].value.number, 888);
	truth(dynamicResults[5].items[2].accepted);
	truth(!dynamicResults[5].items[2].executed);
	truth(!dynamicResults[5].items[3].accepted);
	equal(dynamicResults[5].items[3].endpointStatus, 3);

	// All runtime operations are queued in one existing FIFO, including both optional opcodes.
	await native('reset');
	sent.length = 0;
	const queued = [
		dynamicClient.readField(counter), dynamicClient.writeField(write, dto(1111)),
		dynamicClient.command(command, dto(37)), dynamicClient.service(service, dto(99)),
		dynamicClient.readMany(watch.binding), dynamicClient.batch(tbm, [ {endpoint : read} ])
	];
	const results = await Promise.all(queued);
	truth(results.every(r => r.operation.succeeded));
	equal(sent.map(bytes => bytes[6]), [ 4, 1, 2, 3, 5, 6 ]);
	equal(results[0].value, 42);
	equal(results[4].items[0].value, 112);
	equal(results[5].items[0].value.number, 112);
	equal(JSON.parse(await native('state')).trace, [
		[ 1, 1, 1, 42 ], [ 2, 1, 0, 1111 ], [ 3, 1, 0, 37 ], [ 4, 1, 0, 99 ], [ 1, 1, 1, 112 ],
		[ 1, 1, 4, 1 ], [ 1, 1, 0, 112 ]
	]);

	const ping = bind(dynamicClient, 'Service', 65537),
	      emptyService = bind(dynamicClient, 'Service', 65538);
	const emptyField = bind(dynamicClient, 'FieldRead', 65539);
	equal((await dynamicClient.service(ping)).valueStatus, V.NoValue);
	const empty = await dynamicClient.service(emptyService);
	equal(empty.valueStatus, V.Ok);
	equal(Object.keys(empty.value), []);
	const emptyRead = await dynamicClient.readField(emptyField);
	equal(emptyRead.valueStatus, V.Ok);
	equal(Object.keys(emptyRead.value), []);
	equal(dynamicClient.bindReadMany(trm, [ read ]).status, Status.Unsupported);
	equal(dynamicClient.bind('FieldWrite', 65537).status, Status.ReadOnly);
	equal(dynamicClient.pendingCount, 0);
	dynamicClient.close();
	staticClient.close();
	truth(dynamicClient.drained);
	truth(staticClient.drained);
} finally {
	child.stdin.end();
	equal(await exited, 0);
	equal(stderr, '');
}
console.log(`Dynamic integration checks=${checks} packets=${packets} failures=0`);
