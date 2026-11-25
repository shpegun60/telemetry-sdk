// One native Model, four SDK surfaces, actual Encoded packets. MIT.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {readFile, writeFile} from 'node:fs/promises';
import {createInterface} from 'node:readline';
import {pathToFileURL} from 'node:url';

const [dynamicPath, runtimePath, sdkPath, descriptorPath, trmPath, tbmPath, executable, nativePath,
       metadataPath, metadataModulePath, outputPath] = process.argv.slice(2);
const {DynamicClient} = await import(pathToFileURL(dynamicPath).href);
const {parseDescriptor, endpointPath} =
    await import(new URL('./telemetry.js', pathToFileURL(dynamicPath)));
const {DeviceClient, EncodedBackend, Status, Operations, Batch} =
    await import(pathToFileURL(runtimePath).href);
const {control, matchesHeader} = await import(pathToFileURL(sdkPath).href);
const descriptor = await readFile(descriptorPath), header = descriptor.subarray(0, 64);
const trm = await readFile(trmPath), tbm = await readFile(tbmPath);
const schema = parseDescriptor(descriptor);
let checks = 0, packets = 0;
const equal = (a, b) => {
	++checks;
	assert.deepEqual(a, b);
};
const truth = (value, message) => {
	++checks;
	assert(value, message);
};
const hex = bytes => Buffer.from(bytes).toString('hex');
const dto = number => ({
	number,
	enabled : true,
	gains : [ 0, 65535, 17 ],
	mode : -123,
	large : 0xffffffffffffffffn,
	negative : -0x8000000000000000n,
	f32 : 4.25,
	f64 : -0
});
const exact = (value, number) => {
	equal(Object.keys(value), Object.keys(dto(number)));
	equal({...value}, dto(number));
	truth(Object.is(value.f64, -0));
};
const child = spawn(executable, [], {stdio : [ 'pipe', 'pipe', 'pipe' ]});
let pending, stderr = '';
child.stderr.on('data', data => stderr += data);
createInterface({input : child.stdout}).on('line', line => {
	assert(pending, 'Unexpected native peer response');
	const resolve = pending;
	pending = undefined;
	resolve(line);
});
const exited = new Promise((resolve, reject) => {
	child.once('error', reject);
	child.once('exit', code => pending ? reject(Error(`Peer ${code}: ${stderr}`)) : resolve(code));
});
const native = command => new Promise(resolve => {
	assert(!pending, 'Parallel exchanges bypassed the one FIFO');
	pending = resolve;
	child.stdin.write(command + '\n');
});
const facts = (result, present, valueStatus = null) => ({
	operation : Operations[result.operation],
	status : result.status,
	dispatch : result.dispatch,
	endpointStatus : result.endpointStatus,
	succeeded : result.succeeded,
	accepted : result.accepted,
	executed : result.executed,
	hasValue : present,
	valueStatus
});
const record = (result, dynamic, request, reply) => {
	const operation = dynamic ? result.operation : result;
	const rawItems =
	    result.items ?? (!dynamic && operation.operation === 'ReadMany' ? result.value : undefined);
	const items = rawItems?.map(item => {
		const present = item.value !== undefined;
		if (operation.operation === 'ReadMany')
			return {
				id : item.id,
				status : item.status,
				hasValue : present,
				valueStatus : item.valueStatus ?? null
			};
		return {id : item.id, ...facts(item, present, item.valueStatus ?? null)};
	}) ?? [];
	return {
		...facts(operation, items.length ? items.some(i => i.hasValue) : result.value !== undefined,
		         result.valueStatus ?? null),
		request,
		reply,
		payload : hex(operation.payload),
		items
	};
};

async function sequence(dynamic)
{
	await native('reset');
	let retire = false, client;
	const sent = [], replies = [];
	const backend = EncodedBackend.fromExchange(async bytes => {
		++packets;
		sent.push(hex(bytes));
		const reply = Uint8Array.from(Buffer.from(await native(hex(bytes)), 'hex'));
		replies.push(hex(reply));
		if (retire)
			dynamic ? client.invalidate() : client.invalidateAdmission();
		return reply;
	}, {readMany : true, batch : true});
	client = dynamic ? new DynamicClient({backend, timeoutMs : 5000})
	                 : new DeviceClient({backend, matchesHeader, timeoutMs : 5000});
	if (dynamic)
		equal(client.admit(schema, header, 17), Status.Ok);
	else
		truth(client.openSession(17, header));
	const bind = (operation, id) => {
		const result = client.bind(operation, id);
		equal(result.status, Status.Ok);
		return result.endpoint;
	};
	const F = control, C = control, S = control;
	const read = dynamic ? bind('FieldRead', 65536) : F.Config;
	const write = dynamic ? bind('FieldWrite', 65536) : F.Config;
	const command = dynamic ? bind('Command', 65536) : C.Configure;
	const service = dynamic ? bind('Service', 65536) : S.Echo;
	const accepted = dynamic ? bind('Command', 65538) : C.Accepted;
	const busy = dynamic ? bind('Service', 65539) : S.Busy;
	const counter = dynamic ? bind('FieldRead', 65537) : F.Counter;
	const flag = dynamic ? bind('FieldRead', 65540) : F.Bool;
	const watch = client.bindReadMany(trm, [ counter, flag ]);
	equal(watch.status, Status.Ok);
	// Public happy API: no wire bytes or per-device transport wrapper.
	const batch = () =>
	    dynamic ? client.batch(tbm,
	                           [
		                           {endpoint : write, value : dto(888)}, {endpoint : read},
		                           {endpoint : accepted, value : dto(37)},
		                           {endpoint : busy, value : dto(99)},
		                           {endpoint : service, value : dto(55)}
	                           ])
	            : client.batch(tbm, Batch.items(Batch.write(write, dto(888)), Batch.read(read),
	                                            Batch.command(accepted, dto(37)),
	                                            Batch.service(busy, dto(99)),
	                                            Batch.service(service, dto(55))));
	const results = [
		dynamic ? await client.writeField(write, dto(1111)) : await client.write(write, dto(1111)),
		dynamic ? await client.readField(read) : await client.read(read),
		await client.command(command, dto(37)), await client.service(service, dto(99)),
		await client.readMany(watch.binding), await batch()
	];
	exact(results[1].value, 1000);
	exact(results[3].value, 99);
	const watchItems = dynamic ? results[4].items : results[4].value;
	truth(watchItems);
	equal(watchItems.map(i => i.value), [ 112, true ]);
	exact(results[5].items[1].value, 888);
	exact(results[5].items[4].value, 55);
	const acceptedResult = results[5].items[2];
	const busyResult = results[5].items[3];
	truth(acceptedResult.accepted && !acceptedResult.executed);
	truth(!busyResult.accepted && !busyResult.succeeded);
	equal(busyResult.endpointStatus, 3);
	retire = true;
	results.push(await batch());
	equal(results[6].valueStatus, 'Stale');
	truth(results[6].items.every(i => i.value === undefined));
	const staleAccepted = results[6].items[2];
	truth(staleAccepted.accepted && !staleAccepted.executed);
	const state = JSON.parse(await native('state'));
	equal(state.decoy, [ 0, 0, 0, 0 ]);
	delete state.decoy;
	client.close();
	truth(client.drained);
	equal(sent.length, 7);
	equal(replies.length, 7);
	return {
		name : dynamic ? 'js-dynamic' : 'js-static',
		records : results.map((r, i) => record(r, dynamic, sent[i], replies[i])),
		state
	};
}

// Compare structural wire metadata with the existing independent runtime parser.
function graph(model)
{
	const types = model.types.map(type => {
		const record = {id : type.id, kind : type.kind, wireBytes : type.wireBytes};
		if (type.kind === 1)
			record.scalarCode = type.scalarCode;
		if (type.kind === 2) {
			record.underlyingTypeId = type.underlyingTypeId;
			record.entries = type.entries.map(e => ({code : e.code.toString(), name : e.name}));
		}
		if (type.kind === 3) {
			let offset = 0;
			record.members = type.members.map(m => {
				const member =
				    {typeId : m.typeId, name : m.name, wireOffset : offset, jsName : m.name};
				offset += model.types[m.typeId].wireBytes;
				return member;
			});
		}
		if (type.kind === 4) {
			record.elementTypeId = type.elementTypeId;
			record.elementCount = type.elementCount;
		}
		// Descriptor v3.1: the wire name the generated metadata must repeat.
		if (type.name !== undefined)
			record.name = type.name;
		return record;
	});
	const catalogs = [], endpoints = [];
	for (const [index, key] of ['field', 'command', 'service'].entries()) {
		const rows = model[key + 'Catalogs'],
		      entries = model[key === 'field' ? 'fields' : key + 's'];
		for (const catalog of rows) {
			catalogs.push({
				category : index + 1,
				group : catalog.group,
				firstInCategory : catalog.first,
				count : catalog.count,
				name : catalog.name
			});
			for (const endpoint of entries.slice(catalog.first, catalog.first + catalog.count))
				endpoints.push({
					category : index + 1,
					id : endpoint.id,
					catalogName : catalog.name,
					name : endpoint.name,
					valueType : key === 'field' ? endpoint.typeId : 0,
					requestType : key === 'field' ? 0 : endpoint.requestTypeId,
					responseType : key === 'service' ? endpoint.responseTypeId : 0,
					capabilities : endpoint.capabilities ?? 0,
					...(model.minor >= 1 ? {path : endpointPath(model, index, endpoint)} : {})
				});
		}
	}
	return {types, catalogs, endpoints};
}

const stripPresentation = document => ({
	types : document.types.map(
	    ({cppName, jsName, ...t}) =>
	        ({...t, ...(t.members ? {members : t.members.map(({cppName, ...m}) => m)} : {})})),
	catalogs : document.catalogs.map(({category, group, firstInCategory, count, name}) =>
	                                     ({category, group, firstInCategory, count, name})),
	endpoints : document.endpoints.map(({
		                                   category,
		                                   id,
		                                   catalogName,
		                                   name,
		                                   valueType,
		                                   requestType,
		                                   responseType,
		                                   capabilities,
		                                   path
	                                   }) => ({
		                                   category,
		                                   id,
		                                   catalogName,
		                                   name,
		                                   valueType,
		                                   requestType,
		                                   responseType,
		                                   capabilities,
		                                   ...(path !== undefined ? {path} : {})
	                                   }))
});
const frozen = value => {
	if (!value || typeof value !== 'object')
		return;
	truth(Object.isFrozen(value));
	for (const child of Object.values(value))
		frozen(child);
};
const withoutLabels = ({valueStatus, ...value}) =>
    ({...value, ...(value.items ? {items : value.items.map(withoutLabels)} : {})});
const expectedState = {
	number : 888,
	reads : 5,
	writes : 3,
	commands : 3,
	services : 5,
	trace : [
		[ 2, 1, 0, 1111 ], [ 1, 1, 0, 1000 ], [ 3, 1, 0, 37 ], [ 4, 1, 0, 99 ], [ 1, 1, 1, 112 ],
		[ 1, 1, 4, 1 ], [ 2, 1, 0, 888 ], [ 1, 1, 0, 888 ], [ 3, 1, 2, 37 ], [ 4, 1, 3, 99 ],
		[ 4, 1, 0, 55 ], [ 2, 1, 0, 888 ], [ 1, 1, 0, 888 ], [ 3, 1, 2, 37 ], [ 4, 1, 3, 99 ],
		[ 4, 1, 0, 55 ]
	]
};
let report;
try {
	const original = JSON.parse(await readFile(nativePath, 'utf8'));
	const surfaces = [...original.surfaces, await sequence(false), await sequence(true) ];
	equal(surfaces.map(s => s.name), [ 'cpp-static', 'cpp-dynamic', 'js-static', 'js-dynamic' ]);
	for (const surface of surfaces) {
		equal(surface.state, expectedState);
		equal(surface.records.map(r => r.operation), [ 1, 4, 2, 3, 5, 6, 6 ]);
		equal(surface.records.map(r => r.hasValue),
		      [ false, true, false, true, true, true, false ]);
		equal(surface.records.map(r => r.status), [ 0, 0, 0, 0, 0, 0, 0 ]);
		equal(surface.records.map(r => r.items.length), [ 0, 0, 0, 0, 2, 5, 5 ]);
		equal(surface.records[5].items.map(i => i.endpointStatus), [ 0, 0, 1, 3, 0 ]);
		equal(surface.records[6].items.map(i => i.endpointStatus), [ 0, 0, 1, 3, 0 ]);
		equal(surface.records[6].items.map(i => i.hasValue), [ false, false, false, false, false ]);
		equal(surface.records[5].items.map(i => i.valueStatus),
		      [ 'NoValue', 'Ok', 'NoValue', 'NoValue', 'Ok' ]);
		for (const [index, record] of surface.records.entries()) {
			const request = Uint8Array.from(Buffer.from(record.request, 'hex'));
			const frame = new DataView(request.buffer);
			equal([...request.subarray(0, 4) ], [ 0x54, 0x45, 0x51, 0x31 ]);
			equal(frame.getUint32(8, true), 17);
			equal(frame.getUint32(12, true), index + 1);
		}
		equal(surface.records.map(withoutLabels), surfaces[0].records.map(withoutLabels));
	}
	// Report existing public vocabulary honestly rather than papering over the differences.
	equal(surfaces[0].records.map(r => r.valueStatus),
	      [ null, null, null, null, 'Ok', 'Ok', 'Stale' ]);
	equal(surfaces[1].records.map(r => r.valueStatus),
	      [ 'NoValue', 'Ok', 'NoValue', 'Ok', 'Ok', 'Ok', 'Stale' ]);
	equal(surfaces[2].records.map(r => r.valueStatus),
	      [ null, null, null, null, null, 'Ok', 'Stale' ]);
	equal(surfaces[3].records.map(r => r.valueStatus),
	      [ 'NoValue', 'Ok', 'NoValue', 'Ok', 'Ok', 'Ok', 'Stale' ]);
	for (const [index, surface] of surfaces.entries())
		equal(surface.records[4].items.map(i => i.valueStatus),
		      index === 2 ? [ null, null ] : [ 'Ok', 'Ok' ]);
	equal(surfaces[0].records[6].items.map(i => i.valueStatus), Array(5).fill('Stale'));
	equal(surfaces[1].records[6].items.map(i => i.valueStatus), Array(5).fill('Stale'));
	equal(surfaces[2].records[6].items.map(i => i.valueStatus),
	      [ 'NoValue', 'Stale', 'NoValue', 'NoValue', 'Stale' ]);
	equal(surfaces[3].records[6].items.map(i => i.valueStatus), Array(5).fill('Stale'));
	const metadata = JSON.parse(await readFile(metadataPath, 'utf8'));
	const {structuralMetadata} = await import(pathToFileURL(metadataModulePath).href);
	equal(structuralMetadata, metadata);
	frozen(structuralMetadata);
	equal(metadata.format, 'telemetry-structural-metadata');
	equal(metadata.version, 1);
	equal(metadata.fingerprint, '0x' + schema.fingerprint.toString(16).padStart(16, '0'));
	equal(metadata.descriptorHeader, [...header ]);
	equal(stripPresentation(metadata), graph(schema));
	const number = metadata.types.flatMap(t => t.members ?? []).find(m => m.name === 'number');
	equal(number.cppName, 'number_value');
	equal(number.jsName, 'number');
	report = {
		passed : true,
		hardware : false,
		checks,
		packets,
		surfaces,
		metadataGraph : {
			types : metadata.types.length,
			catalogs : metadata.catalogs.length,
			endpoints : metadata.endpoints.length
		}
	};
} finally {
	child.stdin.end();
	equal(await exited, 0);
	equal(stderr, '');
}
report.checks = checks;
await writeFile(outputPath, JSON.stringify(report, null, 2) + '\n');
console.log(`SDK four-surface conformance checks=${checks} packets=${packets} failures=0`);
