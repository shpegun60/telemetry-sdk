"""Independent explicit fixtures for descriptor v3.0 and v3.1 (not producer-generated).

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
v3.0: mixed, edge, empty. v3.1: named (mixed with its three C++ type names) and
attrs (named plus advisory and application attributes on every record kind).
The v3.1 table lists accepted inputs and one malformed descriptor per rule,
with the verdict that the C++, JS and Python decoders must all give.
"""
import json
import struct
from parser import fingerprint


def u32(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def text(value):
    encoded = value if isinstance(value, bytes) else value.encode('utf-8')
    return u32(len(encoded)) + encoded


def attribute(tag, value=b''):
    """One attribute: u8 tag, u32 length, value. Its value describes the whole record."""
    return struct.pack('<BI', tag, len(value)) + value


def member_items(member_count, items):
    """The member-tag shape of a Struct record: a presence bitmap of
    ceil(memberCount/8) bytes (bit i = member i), then one item per set bit."""
    bitmap = bytearray((member_count + 7) // 8)
    for index in items:
        bitmap[index // 8] |= 1 << (index % 8)
    return bytes(bitmap) + b''.join(items[index] for index in sorted(items))


def record(kind, payload, attributes=b'', flags=None):
    if flags is None:
        flags = 1 if attributes else 0
    body = payload + attributes
    return struct.pack('<BBHI', kind, 1, flags, len(body)) + body


def type_record(index, kind, wire, tail=b'', name=None, attributes=b'', flags=None, type_flags=None):
    if type_flags is None:
        type_flags = 0 if name is None else 1
    payload = u32(index) + bytes((kind, type_flags, 0, 0)) + u32(wire)
    if name is not None:
        payload += text(name)
    return record(1, payload + tail, attributes, flags)


def struct_type(index, wire, members, **extra):
    return type_record(index, 3, wire, u32(len(members)) + b''.join(u32(t) + text(n) for t, n in members), **extra)


def enum_type(index, base, width, entries, **extra):
    return type_record(index, 2, width, u32(base, len(entries)) + b''.join(
        (code & ((1 << (8 * width)) - 1)).to_bytes(width, 'little') + text(name) for code, name in entries), **extra)


# The mixed model's C++ types as the default (names on) producer spells them.
MIXED_NAMES = {12: 'Mode', 14: 'State', 15: 'MotorConfig'}

# Unregistered SDK advisory tags and one application tag; v3.1 defines none.
ADVISORY_A, ADVISORY_B, APPLICATION = 0x3E, 0x3F, 0x40

# attrs: advisory attributes on a Struct (one of them in the member shape), an
# Enum, an Array, a Catalog, a Field, a Command and a Service record, plus one
# application tag. Tags ascend strictly within each record.
MIXED_ATTRIBUTES = {
    ('type', 12): attribute(ADVISORY_B, b'enum'),
    ('type', 13): attribute(ADVISORY_A, u32(7)),
    ('type', 14): attribute(ADVISORY_A),
    ('type', 15): (attribute(ADVISORY_A, member_items(3, {1: bytes((0x02, 0x03))})) +
                   attribute(ADVISORY_B, b'') + attribute(APPLICATION, b'app')),
    ('catalog', 1, 0): attribute(ADVISORY_A, b'group'),
    ('endpoint', 1, 0, 1): attribute(ADVISORY_B, u32(0, 0)),
    ('endpoint', 2, 0, 0): attribute(ADVISORY_A, b'c'),
    ('endpoint', 3, 0, 0): attribute(ADVISORY_A, b's'),
}


def fixture(which, names=None, attributes=None, minor=None, extra=None):
    """Build one descriptor; names/attributes/minor/extra exist for v3.1 and negative cases.

    extra maps ('type', id) to keyword overrides of that record (name, flags,
    type_flags, attributes) so one rule can be broken at a time.
    """
    if which == 'named':
        return fixture('mixed', MIXED_NAMES if names is None else names, attributes, minor, extra)
    if which == 'attrs':
        return fixture('mixed', MIXED_NAMES if names is None else names,
                       MIXED_ATTRIBUTES if attributes is None else attributes, minor, extra)
    names, attributes, extra = names or {}, attributes or {}, extra or {}

    def options(key):
        result = {}
        if key[0] == 'type' and key[1] in names:
            result['name'] = names[key[1]]
        if key in attributes:
            result['attributes'] = attributes[key]
        result.update(extra.get(key, {}))
        return result

    types = [type_record(0, 0, 0, **options(('type', 0)))]
    for code, size in enumerate((1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8), 1):
        types.append(type_record(code, 1, size, bytes((code, 0, 0, 0)), **options(('type', code))))
    catalogs, endpoints = [], []
    counts = [0, 0, 0, 0, 0, 0]
    if which != 'empty':
        types += [enum_type(12, 2, 1, [(0, 'Off'), (1, 'Run'), (2, 'Fault')], **options(('type', 12))),
                  type_record(13, 4, 6, u32(4, 3), **options(('type', 13))),
                  struct_type(14, 5, [(10, 'volts'), (1, 'active')], **options(('type', 14))),
                  struct_type(15, 7, [(10, 'target'), (4, 'rpm'), (1, 'enabled')], **options(('type', 15)))]
        field_groups = [('motor', [(1, 'Enabled', 1), (4, 'RPM', 1), (10, 'Temperature', 1),
                                    (11, 'Precise', 1), (12, 'Mode', 1), (13, 'Samples', 1),
                                    (14, 'State', 1), (15, 'Config', 3)])]
        command_groups = [('motor', [(15, 'Configure'), (0, 'Reset')])]
        service_groups = [('motor', [(15, 15, 'Echo')])]
        if which == 'edge':
            types += [struct_type(16, 0, []), type_record(17, 4, 10, u32(14, 2)),
                      type_record(18, 4, 0, u32(2, 0)),
                      struct_type(19, 17, [(15, 'config'), (17, 'phases'), (16, 'empty'), (18, 'zero')]),
                      enum_type(20, 5, 2, [(-1000, 'Négatif'), (2000, 'Haut')]),
                      enum_type(21, 8, 8, [(0xffffffffffffffff, 'Max')])]
            field_groups += [('empty', []), ('μ', [(16, 'Empty', 1), (19, 'Box', 1),
                                                      (20, 'Signed', 1), (21, 'Wide', 1), (6, 'Late', 3)])]
            service_groups += [('extra', [(0, 0, 'Ping')])]
        for category, groups in enumerate((field_groups, command_groups, service_groups), 1):
            first = 0
            counts[(category - 1) * 2] = len(groups)
            for group, (name, entries) in enumerate(groups):
                catalogs.append(record(2, bytes((category, 0, 0, 0)) + u32(group, first, len(entries)) + text(name),
                                       attributes.get(('catalog', category, group), b'')))
                for local, entry in enumerate(entries):
                    payload = u32(group << 16 | local, entry[0])
                    if category == 1:
                        payload += bytes((entry[2], 0, 0, 0)) + text(entry[1])
                    elif category == 2:
                        payload += text(entry[1])
                    else:
                        payload += u32(entry[1]) + text(entry[2])
                    endpoints.append(record(category + 2, payload,
                                            attributes.get(('endpoint', category, group, local), b'')))
                first += len(entries)
            counts[(category - 1) * 2 + 1] = first
    if minor is None:
        minor = 1 if names or attributes else 0
    type_bytes, catalog_bytes, endpoint_bytes = b''.join(types), b''.join(catalogs), b''.join(endpoints)
    total = 64 + len(type_bytes) + len(catalog_bytes) + len(endpoint_bytes)
    header = b'TDS3' + struct.pack('<4HIQ10I', 3, minor, 64, 0, total, 0, len(types), *counts,
                                 64, 64 + len(type_bytes), 64 + len(type_bytes) + len(catalog_bytes))
    result = bytearray(header + type_bytes + catalog_bytes + endpoint_bytes)
    struct.pack_into('<Q', result, 16, fingerprint(result))
    return bytes(result)


def rehash(data):
    data = bytearray(data)
    struct.pack_into('<Q', data, 16, fingerprint(data))
    return bytes(data)


def negative_cases():
    """One v3.1 rule broken per case; every decoder must refuse with the verdict.

    verdict is the C++ runtime::ParseError name; python and js are substrings
    of the Python InvalidDescriptor and JS TelemetryFormatError messages.
    """
    cases = []

    def add(name, data, verdict, python, js):
        cases.append(dict(name=name, verdict=verdict, python=python, js=js, hex=data.hex()))

    struct15 = {('type', 15): attribute(ADVISORY_A)}
    # Header minor and type names.
    add('minor-2', fixture('named', minor=2), 'Header', 'header version', 'Descriptor version')
    add('minor-1-without-name-or-attribute', fixture('mixed', minor=1), 'Header', 'header version',
        'Descriptor version')
    add('minor-0-with-name', fixture('named', minor=0), 'Reserved', 'reserved bytes', 'Nonzero reserved')
    add('named-void', fixture('named', extra={('type', 0): dict(name='Nothing')}), 'TypeShape',
        'type kind', 'Type name on')
    add('named-scalar', fixture('named', extra={('type', 2): dict(name='Byte')}), 'TypeShape',
        'type kind', 'Type name on')
    add('named-array', fixture('named', extra={('type', 13): dict(name='Samples')}), 'TypeShape',
        'type kind', 'Type name on')
    add('type-flag-bit-1', fixture('named', extra={('type', 14): dict(type_flags=2)}), 'Reserved',
        'reserved bytes', 'Nonzero reserved')
    add('type-flags-named-and-bit-1', fixture('named', extra={('type', 14): dict(type_flags=3, name='State2')}),
        'Reserved', 'reserved bytes', 'Nonzero reserved')
    add('duplicate-type-name', fixture('named', names={12: 'Mode', 14: 'Mode', 15: 'MotorConfig'}),
        'DuplicateName', 'duplicate type name', 'Duplicate type name')
    add('empty-type-name', fixture('named', names={12: '', 14: 'State', 15: 'MotorConfig'}), 'Limit',
        'string budget', 'String budget')
    add('nul-type-name', fixture('named', names={12: 'Mo\0de', 14: 'State', 15: 'MotorConfig'}), 'Utf8',
        'embedded NUL', 'Embedded NUL')
    add('invalid-utf8-type-name', fixture('named', names={12: b'Mo\xffde', 14: 'State', 15: 'MotorConfig'}),
        'Utf8', 'invalid UTF-8', 'Invalid UTF-8')
    # A Named flag whose name length runs past the end of its record.
    data = bytearray(fixture('named'))
    cursor = 64
    for _ in range(12):
        cursor += 8 + struct.unpack_from('<I', data, cursor + 4)[0]
    payload = struct.unpack_from('<I', data, cursor + 4)[0]
    struct.pack_into('<I', data, cursor + 8 + 12, payload)
    add('truncated-type-name', rehash(data), 'Record', 'truncated record', 'Truncated record')
    # Attribute area behind record header flags bit 0.
    add('attribute-flag-on-minor-0', fixture('mixed', attributes=struct15, minor=0), 'Reserved',
        'reserved bytes', 'Nonzero reserved')
    add('attribute-flag-without-attribute', fixture('named', extra={('type', 14): dict(flags=1)}), 'Record',
        'flag without attribute', 'Attribute flag without attribute')
    add('record-flag-bit-1', fixture('named', extra={('type', 14): dict(flags=2)}), 'Reserved',
        'reserved bytes', 'Nonzero reserved')
    add('record-flag-bit-15', fixture('named', attributes=struct15,
                                      extra={('type', 15): dict(flags=0x8001)}), 'Reserved',
        'reserved bytes', 'Nonzero reserved')
    add('attributes-on-void', fixture('named', attributes={('type', 0): attribute(ADVISORY_A)}),
        'TypeShape', 'attributes on Void/Scalar', 'Attributes on Void/Scalar')
    add('attributes-on-scalar', fixture('named', attributes={('type', 4): attribute(ADVISORY_A)}),
        'TypeShape', 'attributes on Void/Scalar', 'Attributes on Void/Scalar')
    add('attribute-tag-out-of-order', fixture('named', attributes={
        ('type', 15): attribute(ADVISORY_B) + attribute(ADVISORY_A)}), 'Record', 'out of order or repeated',
        'Attribute tag out of order or repeated')
    add('attribute-tag-repeated', fixture('named', attributes={
        ('type', 15): attribute(ADVISORY_A, b'a') + attribute(ADVISORY_A, b'b')}), 'Record',
        'out of order or repeated', 'Attribute tag out of order or repeated')
    data = bytearray(fixture('named', attributes={('endpoint', 3, 0, 0): attribute(ADVISORY_A, b'xy')}))
    struct.pack_into('<I', data, len(data) - 6, 3)  # the value now runs one byte past the record
    add('attribute-length-past-record', rehash(data), 'Record', 'value past the record',
        'Attribute value past the record')
    for left in (1, 2, 3, 4):
        # A complete attribute, then a header cut short with 1..4 bytes left.
        add(f'attribute-header-cut-short-{left}', fixture('named', attributes={
            ('endpoint', 2, 0, 0): attribute(ADVISORY_A) + bytes((ADVISORY_B, 0, 0, 0))[:left]}),
            'Record', 'header cut short', 'Attribute header cut short')
    for tag in (0x00, 0x80, 0xC0, 0xFF):
        add(f'reserved-tag-{tag:02x}', fixture('named', attributes={('type', 14): attribute(tag)}),
            'Reserved', 'reserved attribute tag', 'Reserved attribute tag')
    for tag in (0x81, 0xBF):
        add(f'unknown-critical-tag-{tag:02x}', fixture('named', attributes={('type', 14): attribute(tag)}),
            'Record', 'unknown critical attribute tag', 'Unknown critical attribute tag')
    return cases


def positive_cases():
    """v3.1 inputs every decoder must accept; stripped names the v3.0/v3.1 model each equals."""
    return [
        dict(name='named', hex=fixture('named').hex(), equals='named'),
        dict(name='attrs', hex=fixture('attrs').hex(), equals='named'),
        dict(name='attrs-without-names', hex=fixture('mixed', attributes=MIXED_ATTRIBUTES).hex(),
             equals='mixed'),
        dict(name='name-equals-endpoint-and-member', hex=fixture(
            'mixed', names={12: 'Mode', 14: 'volts', 15: 'Config'}).hex(), equals=None),
        dict(name='utf8-type-name', hex=fixture('mixed', names={14: 'Стан'}).hex(),
             equals=None),
        dict(name='empty-value-application-tag', hex=fixture(
            'mixed', attributes={('type', 15): attribute(APPLICATION)}).hex(), equals='mixed'),
        dict(name='every-tag-class-but-critical', hex=fixture(
            'mixed', attributes={('type', 15): attribute(0x04, b'r') + attribute(0x3F) + attribute(0x7F, b'z')}
        ).hex(), equals='mixed'),
    ]


def table():
    return dict(format='descriptor v3.1 decoder table', version=1,
                positive=positive_cases(), negative=negative_cases())


def write_table(path):
    result = table()
    path.write_text(json.dumps(result, indent=1, ensure_ascii=True) + '\n', encoding='utf-8', newline='\n')
    return result


if __name__ == '__main__':
    from pathlib import Path
    for name in ('mixed', 'edge', 'empty', 'named', 'attrs'):
        data = fixture(name)
        path = Path(__file__).with_name(name + '.hex')
        path.write_text('\n'.join(data[i:i + 32].hex(' ') for i in range(0, len(data), 32)) + '\n')
        print(name, len(data), f'{fingerprint(data):016x}')
    result = write_table(Path(__file__).with_name('v31_cases.json'))
    print('v3.1 table', len(result['positive']), 'positive,', len(result['negative']), 'negative')
