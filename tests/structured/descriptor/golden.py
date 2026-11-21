"""Independent explicit fixtures for descriptor v3.0 and v3.1 (not producer-generated).

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
v3.0: mixed, edge, empty. v3.1: named (mixed with its three C++ type names),
attrs (named plus advisory and application attributes on every record kind)
and tree (the GroupTree mode: two branches, adc and metrology, in one shared
tree of group records).
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
MIXED_NAMES = {12: 'fixture::Mode', 14: 'fixture::State', 15: 'fixture::MotorConfig'}

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


# The group-tree fixture of tests/structured/group_tree: the producer's
# registry order (Fields in group pre-order, then Commands, then Services).
TREE_NAMES = {12: 'adc::Config', 14: 'adc::Status', 15: 'metrology::Window', 16: 'metrology::Config',
              17: 'adc::Reference', 18: 'adc::Offset'}

# Pre-order groups: name, depth, Fields (type, name, caps), Commands (request,
# name), Services (request, response, name).
TREE_GROUPS = [
    ('adc', 0, [(12, 'Config', 3), (14, 'Status', 1)], [(0, 'Reset')], [(17, 18, 'Calibrate')]),
    ('channel1', 1, [(10, 'Gain', 3)], [], []),
    ('channel2', 1, [(10, 'Gain', 3)], [], []),
    ('metrology', 0, [(16, 'Config', 3)], [], []),
]


def tree_fixture(groups=None, names=None, flags=1, minor=1, attributes=None, counts=None, raw=None):
    """One GroupTree descriptor: G group records, then the endpoints in group order.

    raw maps a group index to (category, reserved) bytes so one rule can be
    broken at a time; counts overrides the header's six catalog/endpoint counts.
    """
    groups = TREE_GROUPS if groups is None else groups
    names = TREE_NAMES if names is None else names
    attributes, raw = attributes or {}, raw or {}
    types = [type_record(0, 0, 0)]
    for code, size in enumerate((1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8), 1):
        types.append(type_record(code, 1, size, bytes((code, 0, 0, 0))))
    types += [struct_type(12, 7, [(10, 'gain'), (4, 'rateHz'), (1, 'enabled')], name=names.get(12)),
              type_record(13, 4, 4, u32(5, 2)),
              struct_type(14, 6, [(4, 'flags'), (13, 'raw')], name=names.get(14)),
              enum_type(15, 2, 1, [(0, 'Rectangular'), (1, 'Hann'), (2, 'FlatTop')], name=names.get(15)),
              struct_type(16, 5, [(6, 'cycles'), (15, 'window')], name=names.get(16)),
              struct_type(17, 4, [(10, 'volts')], name=names.get(17)),
              struct_type(18, 4, [(10, 'counts')], name=names.get(18))]
    catalogs, families = [], ([], [], [])
    for index, (name, depth, fields, commands, services) in enumerate(groups):
        category, reserved = raw.get(index, (0, 0))
        catalogs.append(record(2, bytes((category, reserved)) + struct.pack('<H', depth) +
                               u32(len(fields), len(commands), len(services)) + text(name),
                               attributes.get(('group', index), b'')))
        for local, (type_id, entry, caps) in enumerate(fields):
            families[0].append(record(3, u32(index << 16 | local, type_id) + bytes((caps, 0, 0, 0)) + text(entry)))
        for local, (request, entry) in enumerate(commands):
            families[1].append(record(4, u32(index << 16 | local, request) + text(entry)))
        for local, (request, response, entry) in enumerate(services):
            families[2].append(record(5, u32(index << 16 | local, request, response) + text(entry)))
    if counts is None:
        counts = [len(groups), len(families[0]), len(groups), len(families[1]), len(groups), len(families[2])]
    type_bytes, catalog_bytes = b''.join(types), b''.join(catalogs)
    endpoint_bytes = b''.join(b''.join(rows) for rows in families)
    total = 64 + len(type_bytes) + len(catalog_bytes) + len(endpoint_bytes)
    header = b'TDS3' + struct.pack('<4HIQ10I', 3, minor, 64, flags, total, 0, len(types), *counts,
                                 64, 64 + len(type_bytes), 64 + len(type_bytes) + len(catalog_bytes))
    result = bytearray(header + type_bytes + catalog_bytes + endpoint_bytes)
    struct.pack_into('<Q', result, 16, fingerprint(result))
    return bytes(result)


def with_group(index, **change):
    """TREE_GROUPS with one group's fields replaced (name, depth, fields, commands, services)."""
    keys = ('name', 'depth', 'fields', 'commands', 'services')
    groups = [list(group) for group in TREE_GROUPS]
    for key, value in change.items():
        groups[index][keys.index(key)] = value
    return [tuple(group) for group in groups]


def fixture(which, names=None, attributes=None, minor=None, extra=None, motor='motor'):
    """Build one descriptor; names/attributes/minor/extra exist for v3.1 and negative cases.

    extra maps ('type', id) to keyword overrides of that record (name, flags,
    type_flags, attributes) so one rule can be broken at a time.
    """
    if which == 'named':
        return fixture('mixed', MIXED_NAMES if names is None else names, attributes, minor, extra)
    if which == 'attrs':
        return fixture('mixed', MIXED_NAMES if names is None else names,
                       MIXED_ATTRIBUTES if attributes is None else attributes, minor, extra)
    if which == 'tree':
        return tree_fixture()
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
        field_groups = [(motor, [(1, 'Enabled', 1), (4, 'RPM', 1), (10, 'Temperature', 1),
                                  (11, 'Precise', 1), (12, 'Mode', 1), (13, 'Samples', 1),
                                  (14, 'State', 1), (15, 'Config', 3)])]
        command_groups = [(motor, [(15, 'Configure'), (0, 'Reset')])]
        service_groups = [(motor, [(15, 15, 'Echo')])]
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
    add('duplicate-type-name', fixture('named', names={12: 'adc::Mode', 14: 'adc::Mode', 15: 'Config'}),
        'DuplicateName', 'duplicate type name', 'Duplicate type name')
    for label, bad in (('two-scopes', 'a::b::c'), ('empty-scope', '::Config'), ('empty-name', 'adc::'),
                       ('single-colon', 'adc:Config'), ('triple-colon', 'adc:::Config'),
                       ('slash', 'adc/Config'), ('dot', 'adc.Config'), ('bracket', 'Config[0]'),
                       ('backslash', 'adc\\Config')):
        add(f'type-name-shape-{label}', fixture('named', names={12: bad, 14: 'State', 15: 'Config'}),
            'TypeShape', 'type name shape', 'Type name shape')
    add('empty-type-name', fixture('named', names={12: '', 14: 'State', 15: 'Config'}), 'Limit',
        'string budget', 'String budget')
    add('nul-type-name', fixture('named', names={12: 'Mo\0de', 14: 'State', 15: 'Config'}), 'Utf8',
        'embedded NUL', 'Embedded NUL')
    add('invalid-utf8-type-name', fixture('named', names={12: b'Mo\xffde', 14: 'State', 15: 'Config'}),
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
    # GroupTree: the header flag, group records and the tree's name rules.
    add('group-tree-on-minor-0', tree_fixture(minor=0), 'Header', 'header version', 'Header size/flags')
    add('header-flag-bit-1', tree_fixture(flags=2), 'Header', 'header version', 'Header size/flags')
    add('header-flags-tree-and-bit-1', tree_fixture(flags=3), 'Header', 'header version', 'Header size/flags')
    add('tree-catalog-counts-differ', tree_fixture(counts=[4, 5, 3, 1, 4, 1]), 'Header',
        'group tree catalog counts differ', 'Group tree catalog counts differ')
    add('group-record-outside-tree-mode', tree_fixture(flags=0, minor=1, counts=[4, 5, 0, 1, 0, 1]), 'Catalog',
        'catalog category/order', 'Catalog category/order')
    add('family-catalog-in-tree-mode', tree_fixture(raw={1: (1, 0)}), 'Catalog', 'catalog category/order',
        'Catalog category/order')
    add('group-record-reserved-byte', tree_fixture(raw={2: (0, 1)}), 'Reserved', 'reserved bytes',
        'Nonzero reserved')
    add('tree-first-depth-not-zero', tree_fixture(groups=with_group(0, depth=1)), 'Catalog',
        'group depth outline', 'Group depth outline')
    add('tree-depth-jump', tree_fixture(groups=with_group(1, depth=2)), 'Catalog', 'group depth outline',
        'Group depth outline')
    add('tree-depth-over-ceiling', tree_fixture(groups=with_group(0, depth=255)), 'Limit',
        'group depth budget', 'Group depth budget')
    long_a, long_b = 'a' * 2100, 'b' * 2100
    add('tree-path-over-ceiling', tree_fixture(groups=with_group(0, name=long_a)[:1] + [
        (long_b, 1, [(10, 'Gain', 3)], [], [])] + with_group(0)[3:]), 'Limit', 'name budget', 'Name budget')
    add('tree-count-sum-differs', tree_fixture(counts=[4, 6, 4, 1, 4, 1]), 'Catalog', 'catalog total entries',
        'Catalog total entries')
    add('tree-group-name-slash', tree_fixture(groups=with_group(1, name='channel/1')), 'Catalog',
        'tree name character', 'Tree name character')
    add('tree-group-name-backslash', tree_fixture(groups=with_group(3, name='metro\\logy')), 'Catalog',
        'tree name character', 'Tree name character')
    add('tree-endpoint-name-dot', tree_fixture(groups=with_group(2, fields=[(10, 'Gain.x', 3)])), 'Endpoint',
        'tree name character', 'Tree name character')
    add('tree-endpoint-name-bracket', tree_fixture(groups=with_group(0, commands=[(0, 'Reset[0]')])),
        'Endpoint', 'tree name character', 'Tree name character')
    add('tree-duplicate-across-families', tree_fixture(groups=with_group(0, commands=[(0, 'Status')])),
        'DuplicateName', 'duplicate sibling name', 'Duplicate sibling name')
    add('tree-duplicate-subgroup-and-endpoint',
        tree_fixture(groups=with_group(1, name='Config')), 'DuplicateName', 'duplicate sibling name',
        'Duplicate sibling name')
    add('tree-duplicate-root-groups', tree_fixture(groups=with_group(3, name='adc')), 'DuplicateName',
        'duplicate sibling name', 'Duplicate sibling name')
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
        dict(name='scoped-utf8-type-name', hex=fixture(
            'mixed', names={12: 'adc::Mode', 14: 'Вимір::Стан'}).hex(), equals=None),
        dict(name='empty-value-application-tag', hex=fixture(
            'mixed', attributes={('type', 15): attribute(APPLICATION)}).hex(), equals='mixed'),
        dict(name='every-tag-class-but-critical', hex=fixture(
            'mixed', attributes={('type', 15): attribute(0x04, b'r') + attribute(0x3F) + attribute(0x7F, b'z')}
        ).hex(), equals='mixed'),
        # GroupTree: the tree, without type names (still minor 1), with an
        # advisory attribute on a group record, and an empty folder and a deep
        # chain (depths 0..7, the producer's default ceiling).
        dict(name='tree', hex=tree_fixture().hex(), equals='tree'),
        dict(name='tree-without-type-names', hex=tree_fixture(names={}).hex(), equals=None),
        dict(name='tree-group-attribute', hex=tree_fixture(attributes={('group', 1): attribute(ADVISORY_A, b'g')}
                                                           ).hex(), equals='tree'),
        dict(name='tree-empty-folder-and-deep-chain', hex=tree_fixture(groups=TREE_GROUPS + [
            ('empty', 0, [], [], [])] + [(f'level{depth}', depth, [], [], []) for depth in range(8)]
        ).hex(), equals=None),
        dict(name='tree-same-name-in-two-groups', hex=tree_fixture(groups=with_group(
            3, fields=[(16, 'Config', 3), (10, 'Gain', 3)])).hex(), equals=None),
        # The flat form keeps any UTF-8 in a group name, path characters included.
        dict(name='flat-name-with-path-characters', hex=fixture('mixed', motor='a/b.c[0]\\d').hex(),
             equals=None),
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
    for name in ('mixed', 'edge', 'empty', 'named', 'attrs', 'tree'):
        data = fixture(name)
        path = Path(__file__).with_name(name + '.hex')
        path.write_text('\n'.join(data[i:i + 32].hex(' ') for i in range(0, len(data), 32)) + '\n')
        print(name, len(data), f'{fingerprint(data):016x}')
    result = write_table(Path(__file__).with_name('v31_cases.json'))
    print('v3.1 table', len(result['positive']), 'positive,', len(result['negative']), 'negative')
