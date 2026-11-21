/*
 * JS (web/telemetry.js) side of the three-language descriptor v3.1 decoder table.
 *
 * Reads the JSON case list written by run.py and prints, per case, the refusal
 * reason or the canonical model dump shared with Decode.cpp and run.py. The
 * findType/minor additions are checked against the same model.
 * Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
 */
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';

const [moduleUrl, casesPath] = [ pathToFileURL(process.argv[2]).href, process.argv[3] ];
const {parseDescriptor, findType, findEndpoint, endpointPath, TelemetryFormatError, TypeKind} =
    await import(moduleUrl);
const cases = JSON.parse(readFileSync(casesPath, 'utf8'));
let checks = 0, failures = 0;
const check = (condition, what) => {
	++checks;
	if (!condition) {
		++failures;
		console.log('FAIL ' + what);
	}
};
const hex = text =>
    text === undefined || text === '' ? '-' : Buffer.from(text, 'utf8').toString('hex');
const lines = [];
for (const item of cases) {
	let model;
	try {
		model = parseDescriptor(Buffer.from(item.hex, 'hex'));
	} catch (error) {
		check(error instanceof TelemetryFormatError, 'refusal is TelemetryFormatError');
		lines.push(`case ${item.name} refused ${error.message}`);
		continue;
	}
	lines.push(`case ${item.name} Ok`);
	lines.push(`minor ${model.minor}`);
	lines.push(`fingerprint ${model.fingerprint.toString(16).padStart(16, '0')}`);
	for (const type of model.types) {
		let detail = '';
		if (type.kind === TypeKind.Void)
			detail = ' V';
		else if (type.kind === TypeKind.Scalar)
			detail = ` S ${type.scalarCode}`;
		else if (type.kind === TypeKind.Enum) {
			const width = BigInt(model.types[type.underlyingTypeId].wireBytes * 8);
			detail =
			    ` E ${type.underlyingTypeId}` +
				type.entries
				    .map(entry =>
				             ` ${BigInt.asUintN(Number(width), BigInt(entry.code)).toString(16)}=${
						         hex(entry.name)}`)
					.join('');
		} else if (type.kind === TypeKind.Struct)
			detail =
			    ' M' + type.members.map(member => ` ${member.typeId}=${hex(member.name)}`).join('');
		else
			detail = ` A ${type.elementTypeId} ${type.elementCount}`;
		lines.push(`type ${type.id} ${type.kind} ${type.wireBytes} ${hex(type.name)}${detail}`);
		check((type.kind === TypeKind.Enum || type.kind === TypeKind.Struct) ||
		          type.name === undefined,
		      'only Enum/Struct carry a name');
		if (type.name !== undefined)
			check(findType(model, type.name) === type, 'findType(name) finds the named type');
	}
	check(findType(model, '') === undefined && findType(model, 'NoSuchTypeName') === undefined,
	      'findType finds nothing for an empty or unknown name');
	const catalogs = [ model.fieldCatalogs, model.commandCatalogs, model.serviceCatalogs ];
	catalogs.forEach((rows, index) => {
		for (const row of rows)
			lines.push(
			    `catalog ${index + 1} ${row.group} ${row.first} ${row.count} ${hex(row.name)}`);
	});
	const endpoints = [ model.fields, model.commands, model.services ];
	endpoints.forEach((rows, index) => {
		for (const row of rows) {
			const type = index === 0 ? row.typeId : row.requestTypeId;
			lines.push(`endpoint ${index + 1} ${row.id} ${type} ${row.responseTypeId ?? 0} ${
				row.capabilities ?? 0} ${hex(row.name)}`);
		}
	});
	for (const group of model.groups ?? [])
		lines.push(`group ${group.index} ${group.depth} ${group.parent ?? '-'} ${hex(group.path)}`);
	endpoints.forEach((rows, index) => {
		for (const row of rows) {
			const path = endpointPath(model, index, row);
			lines.push(`path ${index + 1} ${row.id} ${hex(path)}`);
			const found = findEndpoint(model, path);
			check(!model.groupTree || (found?.endpoint === row && found.category === index),
			      'findEndpoint(endpointPath) finds the endpoint in a tree');
			check(found === undefined || found.endpoint === row,
			      'findEndpoint(endpointPath) finds this endpoint or none');
		}
	});
}
lines.push(`JS decoder: ${cases.length} cases, ${checks} checks, ${failures} failures`);
process.stdout.write(lines.join('\n') + '\n');
process.exitCode = failures === 0 ? 0 : 1;
