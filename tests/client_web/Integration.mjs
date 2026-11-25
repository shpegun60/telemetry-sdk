// Generated bindings against actual C++ Encoded/RPC servers. MIT.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {readFile} from 'node:fs/promises';
import {createInterface} from 'node:readline';
import {pathToFileURL} from 'node:url';
import {FakeClock, flush} from './Trace.mjs';

const [runtimePath, sdkPath, descriptorPath, bridgePath, mode, examplePath] = process.argv.slice(2);
const {DeviceClient, EncodedBackend, RetryingEncodedBackend, RpcBackend, Status} =
    await import(pathToFileURL(runtimePath).href);
const sdk = await import(pathToFileURL(sdkPath).href);
const declaredHeader = (await readFile(descriptorPath)).subarray(0, 64);
let checks = 0, packets = 0;
const equal = (actual, expected) => {
	++checks;
	assert.deepEqual(actual, expected);
};
const truth = value => {
	++checks;
	assert(value);
};

function server()
{
	const child = spawn(bridgePath, mode === 'rpc' ? [ 'resource' ] : [],
	                    {stdio : [ 'pipe', 'pipe', 'pipe' ]});
	let pending;
	let errors = '';
	child.stderr.on('data', data => errors += data);
	createInterface({input : child.stdout}).on('line', line => {
		assert(pending, 'Unsolicited bridge reply');
		const current = pending;
		pending = undefined;
		current.resolve(Uint8Array.from(Buffer.from(line, 'hex')));
	});
	child.on('error', error => pending?.reject(error));
	child.on('exit', code => {
		if (pending)
			pending.reject(Error(`Bridge exited ${code}: ${errors}`));
	});
	return {
		exchange(packet) {
		    assert(!pending, 'One packet exchange at a time');
		    ++packets;
		    return new Promise((resolve, reject) => {
			    pending = {resolve, reject};
			    child.stdin.write(Buffer.from(packet).toString('hex') + '\n');
		    });
		},
		async stop() {
		    assert(!pending);
		    child.stdin.end();
		    const code = await new Promise(resolve => child.once('exit', resolve));
		    equal(code, 0);
		}
	};
}

const bridge = server();
try {
	const backend = mode === 'rpc'
	                    ? new RpcBackend(packet => bridge.exchange(packet), {pollIntervalMs : 0})
	                    : EncodedBackend.fromExchange(packet => bridge.exchange(packet));
	const client = new DeviceClient(
	    {backend, matchesHeader : sdk.matchesHeader, maxPending : 8, timeoutMs : 5000});
	truth(client.openSession(mode === 'rpc' ? 1 : 17, declaredHeader));
	const {device} = sdk;
	const initial = await client.read(device.Config);
	truth(initial.succeeded);
	equal(device.Config.read.requestBytes, 0);
	equal(initial.payload.length, device.Config.read.responseBytes);
	equal(client.snapshotBytes, 0);
	if (mode === 'encoded') {
		equal(initial.value.value, 42);
		equal(initial.value.enabled, true);
		const config = {value : 0x12345678, enabled : false};
		const write = client.write(device.Config, config);
		config.value = 99;
		const read = client.read(device.Config);
		const echo = client.service(device.Echo, {value : 55, enabled : true});
		const outcomes = await Promise.all([ write, read, echo ]);
		truth(outcomes.every(result => result.succeeded));
		equal(outcomes[1].value.value, 0x12345678);
		equal(outcomes[2].value.value, 55);
		const command = await client.command(device.Configure, {value : 88, enabled : true});
		truth(command.executed);
		equal(command.accepted, false);
		const accepted = await client.command(device.Accepted, {value : 66, enabled : false});
		truth(accepted.accepted);
		equal(accepted.executed, false);
		const busy = await client.service(device.Busy, {value : 77, enabled : true});
		equal(busy.status, Status.Ok);
		equal(busy.succeeded, false);
		equal(busy.endpointStatus, 3);
		equal(busy.payload.length, 0);
		equal(busy.value, undefined);
		equal((await client.write(device.ReadOnly, {value : 5, enabled : false})).status,
		      Status.ReadOnly);
		const unavailable = await client.read(device.Optional);
		equal(unavailable.succeeded, false);
		equal(unavailable.dispatch, 9);
		equal(unavailable.payload.length, 0);
		truth((await client.service(device.Ping, undefined)).succeeded);
		truth((await client.command(device.Reset, undefined)).executed);
		const big = await client.read(device.Big);
		truth(big.succeeded);
		equal(big.payload.length, 512);
		// Optional TEQ1 retry against the real stateless dispatcher. A lost reply
		// sends the same command again; this backend intentionally has no deduplication.
		for (const lost of ['request', 'reply']) {
			for (const operation of ['read', 'write', 'command', 'service']) {
				const retryBridge = server(), clock = new FakeClock(), sent = [];
				let firstDone;
				const first = new Promise(resolve => firstDone = resolve);
				const retryBackend = new RetryingEncodedBackend(async packet => {
					sent.push(packet.slice());
					if (sent.length === 1 && lost === 'request') {
						firstDone();
						return;
					}
					const response = await retryBridge.exchange(packet);
					if (sent.length === 1 && lost === 'reply') {
						firstDone();
						return;
					}
					truth(retryBackend.receive(response));
				}, {replyTimeoutMs : 10, maxRetries : 1});
				const retryClient = new DeviceClient(
				    {backend : retryBackend, clock, matchesHeader : sdk.matchesHeader});
				try {
					truth(retryClient.openSession(17, declaredHeader));
					const config = {value : 0x10203040, enabled : true};
					const handle = ({
						read : () => retryClient.read(device.Config),
						write : () => retryClient.write(device.Config, config),
						command : () => retryClient.command(device.Configure, config),
						service : () => retryClient.service(device.Echo, config)
					})[operation]();
					await first;
					await flush();
					equal(sent.length, 1);
					clock.advance(10);
					await flush();
					const outcome = await handle;
					truth(outcome.succeeded);
					equal(outcome.status, Status.Ok);
					equal(sent.length, 2);
					equal(sent[0], sent[1]);
					equal(outcome.session, 17);
					equal(outcome.requestId, handle.requestId);
					equal(retryClient.recoveryRequired, false);
					if (operation === 'service')
						equal(outcome.value.value, config.value);
					if (operation === 'read')
						equal(outcome.value.value, 42);
					await flush();
					equal(clock.timers.size, 0);
				} finally {
					retryClient.close();
					await flush();
					truth(retryClient.drained);
					await retryBridge.stop();
				}
			}
		}
	} else {
		equal(initial.value.serial, 0xffffffffffffffffn);
		equal(initial.value.offset, -0x8000000000000000n);
		const config = initial.value;
		config.gain = 1.5;
		const write = client.write(device.Config, config);
		config.gain = 2.5;
		const read = client.read(device.Config);
		const call = client.service(device.Inspect, {config, multiplier : 4});
		const outcomes = await Promise.all([ write, read, call ]);
		truth(outcomes.every(result => result.succeeded));
		equal(outcomes[1].value.gain, 1.5);
		equal(outcomes[2].value.config.gain, 2.5);
		truth((await client.command(device.Apply, config)).executed);
		const accepted = await client.command(sdk.edge.Accept, {});
		truth(accepted.accepted);
		const busy = await client.service(sdk.edge.Fail, undefined);
		equal(busy.status, Status.Ok);
		equal(busy.succeeded, false);
		equal(busy.endpointStatus, 3);
		equal(busy.payload.length, 0);
		equal(busy.value, undefined);
		const empty = await client.service(sdk.edge.Good, undefined);
		truth(empty.succeeded);
		equal(Object.keys(empty.value), []);
		truth((await client.service(device.Ping, undefined)).succeeded);
		if (examplePath) {
			const example = await import(pathToFileURL(examplePath).href);
			const results = await example.run(client, config);
			equal(results.length, 2);
			truth(results.every(result => result.succeeded));
			equal(results[1].value.gain, config.gain);
		}
		// Drop the final WRITE ACK once, then retry the identical active packet.
		let dropped, retries = 0;
		const retryBridge = server();
		const retryBackend = new RpcBackend(async (packet, context) => {
			const response = await retryBridge.exchange(packet);
			if (packet[0] === 4 && packet[13] === 1 && !dropped) {
				dropped = packet.slice();
				return new Promise((resolve, reject) => {
					if (context.signal.aborted) {
						reject(Error('ACK omitted'));
						return;
					}
					context.signal.addEventListener('abort', () => reject(Error('ACK omitted')),
					                                {once : true});
				});
			}
			if (dropped && packet[0] === 4) {
				equal(packet, dropped);
				++retries;
			}
			return response;
		}, {ackTimeoutMs : 10, maxRetries : 1, pollIntervalMs : 0});
		const retryClient =
		    new DeviceClient({backend : retryBackend, matchesHeader : sdk.matchesHeader});
		try {
			truth(retryClient.openSession(1, declaredHeader));
			const outcome = await retryClient.command(device.Reset, undefined);
			truth(outcome.executed);
			equal(outcome.dispatch, 0);
			equal(outcome.endpointStatus, 0);
			equal(retries, 1);
			equal(retryClient.recoveryRequired, false);
		} finally {
			retryClient.close();
			await retryBridge.stop();
		}
		// A partial RPC request is still Receiving, so a later transaction can
		// replace it without executing the abandoned command or reopening session.
		const partialBridge = server();
		let abandoned, firstFragment = true;
		const partialBackend = new RpcBackend(async packet => {
			const response = await partialBridge.exchange(packet);
			if (firstFragment) {
				firstFragment = false;
				equal(packet[0], 4);
				equal(packet[13], 0);
				truth(abandoned.cancel());
			}
			return response;
		}, {fragmentBytes : 24, pollIntervalMs : 0});
		const partialClient =
		    new DeviceClient({backend : partialBackend, matchesHeader : sdk.matchesHeader});
		try {
			truth(partialClient.openSession(1, declaredHeader));
			const changed = {...initial.value, gain : 3.75};
			abandoned = partialClient.command(device.Apply, changed);
			const next = partialClient.read(device.Config);
			equal((await abandoned).status, Status.Cancelled);
			const current = await next;
			truth(current.succeeded);
			truth(Object.is(current.value.gain, -0));
			equal(partialClient.session, 1);
			truth(partialClient.ready);
			equal(partialClient.recoveryRequired, false);
		} finally {
			partialClient.close();
			await partialBridge.stop();
		}
	}
	await flush();
	equal(client.pendingCount, 0);
	equal(client.recoveryRequired, false);
	client.close();
} finally {
	await bridge.stop();
}
console.log(`async web ${mode} checks=${checks} packets=${packets} failures=0`);
