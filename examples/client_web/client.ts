/* Generate with --targets js --client-runtime into ./generated first. MIT. */
import {device, matchesHeader} from './generated/js/device.js';
import {
	DeviceClient,
	EncodedBackend,
	RpcBackend,
	type Exchange,
	type EncodedSend
} from './generated/js/client.js';

// Explicit cross-platform profile, including FIFO wait and all retry attempts.
const requestTimeoutMs = 30000;

// One exclusive Resource/RPC mailbox. The complete-packet exchange owns framing.
export function rpcClient(exchange: Exchange, header: Uint8Array,
                          freshSession: number): DeviceClient
{
	const client = new DeviceClient({
		backend : new RpcBackend(exchange),
		matchesHeader,
		maxPending : 16,
		maxPayloadBytes : 4096,
		timeoutMs : requestTimeoutMs
	});
	if (!client.openSession(freshSession, header))
		throw Error('Descriptor/session admission refused');
	return client;
}

// Encoded complete-frame port. Call backend.receive(frame) on each received frame.
export function encodedClient(send: EncodedSend, header: Uint8Array, freshSession: number)
{
	const backend = new EncodedBackend(send);
	const client = new DeviceClient({
		backend,
		matchesHeader,
		maxPending : 16,
		maxPayloadBytes : 4096,
		timeoutMs : requestTimeoutMs
	});
	if (!client.openSession(freshSession, header))
		throw Error('Descriptor/session admission refused');
	return {client, receive : (frame: Uint8Array) => backend.receive(frame)};
}

export async function run(client: DeviceClient, config: device.types.Config)
{
	// Snapshot and enqueue immediately. The FIFO sends one transaction at a time.
	const update = client.write(device.Config, config);
	const current = client.read(device.Config);
	const results = await Promise.all([ update, current ]);
	if (!results.every(result => result.succeeded))
		return results;
	return results;
}

// For recoveryRequired: stop/drain the old transport and release its remote
// retained session first. Then confirmSessionClosed(oldGeneration), establish a
// fresh physical session, and openSession(newGeneration, itsHeader). No replay.
