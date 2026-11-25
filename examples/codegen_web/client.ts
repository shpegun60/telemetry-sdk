// Generate the native tests/codegen model into ./generated before compiling.
// Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
import {device, edge, matchesHeader} from './generated/js/device.js';
import {rpcInvoke, type Exchange} from './transport.mjs';

// exchange owns complete packet I/O and a timeout. One admitted connection has
// exclusive mailbox ownership and has agreed this descriptor identity already.
// Transaction IDs below are for a fresh session; the caller must not reuse them.
export async function run(exchange: Exchange, declaredHeader: Uint8Array)
{
	if (!matchesHeader(declaredHeader))
		throw new Error('Descriptor identity mismatch');
	const current =
	    await rpcInvoke(exchange, device.Config.read, undefined, {transactionId : 1});
	const config: device.types.Config = current.value;
	config.gain = 1.5;
	await rpcInvoke(exchange, device.Config.write, config, {transactionId : 2});
	await rpcInvoke(exchange, device.Apply, config, {transactionId : 3});
	const inspected = await rpcInvoke(exchange, device.Inspect, {config, multiplier : 4},
	                                  {transactionId : 4});
	const accepted = await rpcInvoke(exchange, edge.Accept, {}, {transactionId : 5});
	// Accepted acknowledges application admission; physical work may continue.
	return {
		config : inspected.value.config,
		checksum : inspected.value.checksum,
		accepted : accepted.accepted
	};
}
