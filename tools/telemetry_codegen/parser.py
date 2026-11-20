"""Bounded production Descriptor v3.0/v3.1 parser; no C++ producer code is imported.

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
C++ producer goldens and the independent JS consumer validate this parser.
v3.1 adds an optional unique type name on Enum/Struct records and a skippable
attribute area after any record payload; attributes are validated and skipped,
so a descriptor with attributes yields the model of the same bytes without them.
"""
from dataclasses import dataclass
import struct


# Descriptor contract refusal raised by every bounded parser check.
class InvalidDescriptor(ValueError):
    pass


# Immutable parser ceilings checked before slicing strings or allocating record lists.
@dataclass(frozen=True)
class Limits:
    descriptor_bytes: int = 4194304
    types: int = 4096
    members: int = 256
    array_elements: int = 65536
    string_bytes: int = 4096
    enum_entries: int = 65536
    catalogs: int = 65536
    endpoints: int = 65536
    wire_bytes: int = 1048576
    depth: int = 32
    expanded_nodes: int = 262144


TYPE_NAMED = 1
RECORD_ATTRIBUTES = 1
ATTRIBUTE_HEADER_BYTES = 5  # u8 tag, u32 length


def require(condition, reason):
    if not condition:
        raise InvalidDescriptor(reason)


def fingerprint(data):
    result = 0xCBF29CE484222325
    for index, byte in enumerate(data):
        result = ((result ^ (0 if 16 <= index < 24 else byte)) * 0x100000001B3) & 0xffffffffffffffff
    return result


def type_name_shape_valid(name):
    """[scope "::"] name: at most one "::", each part nonempty, without ':' or / . [ ] \\."""
    parts = name.split('::', 1) if '::' in name else [name]
    return all(part and not any(c in part for c in ':/.[]\\') for part in parts)


def attribute_tag_class(tag):
    """Reserved, advisory (SDK), application or critical, per the v3.1 u8 tag table."""
    if 0x01 <= tag <= 0x3F:
        return 'advisory'
    if 0x40 <= tag <= 0x7F:
        return 'application'
    if 0x81 <= tag <= 0xBF:
        return 'critical'
    return 'reserved'  # 0x00, 0x80, 0xC0..0xFF


# Bounded cursor over one descriptor section or record payload.
# API: take/number consume bytes; zeros/text validate content; record creates a
# bounded child reader; done requires exact payload exhaustion or, when the
# record header flags an attribute area, a valid area that runs to its end.
class Reader:
    def __init__(self, data, begin, end, limits, minor=0):
        self.data, self.pos, self.end, self.limits, self.minor = data, begin, end, limits, minor
        self.attributes = False

    def take(self, count):
        require(0 <= count <= self.end - self.pos, 'truncated record')
        start = self.pos
        self.pos += count
        return self.data[start:self.pos]

    def number(self, size):
        return int.from_bytes(self.take(size), 'little')

    def zeros(self, size):
        require(all(x == 0 for x in self.take(size)), 'reserved bytes')

    def text(self):
        size = self.number(4)
        # Check before slicing/decoding/allocation of a Python string.
        require(0 < size <= self.limits.string_bytes, 'string budget')
        raw = self.take(size)
        require(0 not in raw, 'embedded NUL')
        try:
            return bytes(raw).decode('utf-8', errors='strict')
        except UnicodeDecodeError as error:
            raise InvalidDescriptor('invalid UTF-8') from error

    def record(self, kind):
        begin = self.pos
        require(self.number(1) == kind, 'record kind/order')
        require(self.number(1) == 1, 'record version')
        flags = self.number(2)
        # v3.0 reserves the record flags; v3.1 defines bit 0 only.
        require(flags == 0 or (self.minor >= 1 and flags == RECORD_ATTRIBUTES), 'reserved bytes')
        size = self.number(4)
        require(size <= self.end - self.pos, 'record length')
        result = Reader(self.data, self.pos, self.pos + size, self.limits, self.minor)
        result.attributes = flags == RECORD_ATTRIBUTES
        self.pos += size
        result.offset = begin
        return result

    def done(self):
        """End a record: exact payload, or a canonical attribute area to its end.

        Each attribute is u8 tag, u32 length, value; tags ascend strictly, so a
        tag appears at most once per record and its value describes the whole
        record. A value is skipped by its length; no tag is defined in v3.1.
        """
        if not self.attributes:
            require(self.pos == self.end, 'extra or missing payload bytes')
            return
        require(self.pos < self.end, 'attribute area: flag without attribute')
        previous = None
        while self.pos < self.end:
            require(self.end - self.pos >= ATTRIBUTE_HEADER_BYTES, 'attribute area: header cut short')
            tag, length = struct.unpack_from('<BI', self.data, self.pos)
            self.pos += ATTRIBUTE_HEADER_BYTES
            require(length <= self.end - self.pos, 'attribute area: value past the record')
            self.pos += length
            require(previous is None or previous < tag, 'attribute area: tag out of order or repeated')
            previous = tag
            category = attribute_tag_class(tag)
            require(category != 'reserved', 'reserved attribute tag')
            # No SDK tag is defined in v3.1: every critical tag is unknown here.
            require(category != 'critical', 'unknown critical attribute tag')


def parse(data, limits=Limits()):
    # Generation profiles may tighten the frozen v3.0 ceilings, never widen them.
    require(isinstance(limits, Limits), 'invalid parser limits')
    for name, ceiling in vars(Limits()).items():
        value = getattr(limits, name)
        require(type(value) is int and 0 <= value <= ceiling, 'invalid parser limits')
    # Reject untrusted sizes/counts before creating record lists. memoryview
    # borrows input storage and does not duplicate a whole descriptor.
    require(64 <= len(data) <= limits.descriptor_bytes, 'descriptor budget')
    data = memoryview(data)
    require(bytes(data[:4]) == b'TDS3', 'magic')
    major, minor, header, flags, total, stored_hash, *tail = struct.unpack_from('<4HIQ10I', data, 4)
    require(major == 3 and minor in (0, 1) and (header, flags) == (64, 0), 'header version/flags')
    require(total == len(data), 'total length')
    type_count, fc, fe, cc, ce, sc, se, type_start, cat_start, ep_start = tail
    cat_counts, ep_counts = (fc, cc, sc), (fe, ce, se)
    require(12 <= type_count <= limits.types, 'type budget')
    require(sum(cat_counts) <= limits.catalogs, 'catalog budget')
    require(sum(ep_counts) <= limits.endpoints, 'endpoint budget')
    require(all(c <= 65536 for c in cat_counts), 'group index capacity')
    require(type_start == 64 <= cat_start <= ep_start <= total, 'section offsets')
    require(type_count * 20 <= cat_start - type_start, 'type count/bytes')
    require(sum(cat_counts) * 29 <= ep_start - cat_start, 'catalog count/bytes')
    require(sum(ep_counts) * 21 <= total - ep_start, 'endpoint count/bytes')
    require(stored_hash == fingerprint(data), 'fingerprint')

    reader = Reader(data, type_start, cat_start, limits, minor)
    types = []
    type_names = set()
    extended = False  # minor 1 requires at least one name or attribute area
    enum_total = 0
    scalar_sizes = (0, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8)

    def reference(type_id, current, nonvoid=True):
        require(0 <= type_id < current and (type_id != 0 or not nonvoid), 'backward type reference')
        return types[type_id]

    for index in range(type_count):
        row = reader.record(1)
        extended |= row.attributes
        require(row.number(4) == index, 'positional type ID')
        kind = row.number(1)
        type_flags = row.number(1)
        require(type_flags == 0 or (minor >= 1 and type_flags == TYPE_NAMED), 'reserved bytes')
        row.zeros(2)
        wire = row.number(4)
        require(wire <= limits.wire_bytes, 'value wire budget')
        require((index == 0 and kind == 0) or (1 <= index <= 11 and kind == 1) or
                (index >= 12 and kind in (2, 3, 4)), 'type kind/builtin position')
        require(not row.attributes or kind >= 2, 'type kind: attributes on Void/Scalar')
        item = dict(id=index, kind=kind, wire=wire, offset=row.offset, depth=0, nodes=0)
        if type_flags == TYPE_NAMED:
            require(kind in (2, 3), 'type kind: name on a type other than Enum/Struct')
            name = row.text()
            require(type_name_shape_valid(name), 'type kind: type name shape')
            require(name not in type_names, 'duplicate type name')
            type_names.add(name)
            item['name'] = name
            extended = True
        expected_wire = 0
        if kind == 1:
            code = row.number(1)
            row.zeros(3)
            require(code == index, 'builtin scalar code')
            expected_wire = scalar_sizes[code]
            item.update(code=code, nodes=1)
        elif kind == 2:
            underlying_id = row.number(4)
            underlying = reference(underlying_id, index)
            require(underlying['kind'] == 1 and 2 <= underlying['code'] <= 9, 'enum underlying integer')
            count = row.number(4)
            enum_total += count
            require(enum_total <= limits.enum_entries, 'enum budget')
            expected_wire = underlying['wire']
            require(count * (expected_wire + 5) <= row.end - row.pos, 'enum count/bytes')
            entries, previous = [], None
            signed = underlying['code'] in (3, 5, 7, 9)
            for _ in range(count):
                value = int.from_bytes(row.take(expected_wire), 'little', signed=signed)
                require(previous is None or previous < value, 'enum code order/duplicate')
                previous = value
                entries.append((value, row.text()))
            item.update(underlying=underlying_id, entries=entries, nodes=1)
        elif kind == 3:
            count = row.number(4)
            require(count <= limits.members, 'struct member budget')
            require(count * 9 <= row.end - row.pos, 'member count/bytes')
            members, names = [], set()
            depth, nodes = 1, 1
            for _ in range(count):
                child_id = row.number(4)
                child = reference(child_id, index)
                name = row.text()
                require(name not in names, 'duplicate member name')
                names.add(name)
                members.append((child_id, name))
                expected_wire += child['wire']
                nodes += child['nodes']
                depth = max(depth, 1 + child['depth'])
            item.update(members=members, depth=depth, nodes=nodes)
        elif kind == 4:
            child_id = row.number(4)
            child = reference(child_id, index)
            count = row.number(4)
            require(count <= limits.array_elements, 'array element budget')
            expected_wire = count * child['wire']
            item.update(element=child_id, count=count, depth=1 + child['depth'],
                        nodes=1 + count * child['nodes'])
        require(wire == expected_wire, 'wire size mismatch')
        require(item['depth'] <= limits.depth, 'type depth budget')
        require(item['nodes'] <= limits.expanded_nodes, 'expanded node budget')
        row.done()
        types.append(item)
    reader.done()

    reader = Reader(data, cat_start, ep_start, limits, minor)
    catalogs = [[], [], []]
    for category, count in enumerate(cat_counts):
        first, names = 0, set()
        for group in range(count):
            row = reader.record(2)
            extended |= row.attributes
            require(row.number(1) == category + 1, 'catalog category/order')
            row.zeros(3)
            require(row.number(4) == group, 'catalog group index')
            require(row.number(4) == first, 'catalog first entry')
            entries = row.number(4)
            require(entries <= 65536 and entries <= ep_counts[category] - first, 'catalog entry count')
            name = row.text()
            require(name not in names, 'duplicate catalog name')
            names.add(name)
            catalogs[category].append(dict(group=group, first=first, count=entries, name=name, offset=row.offset))
            first += entries
            row.done()
        require(first == ep_counts[category], 'catalog total entries')
    reader.done()

    reader = Reader(data, ep_start, total, limits, minor)
    endpoints = [[], [], []]
    for category in range(3):
        for catalog in catalogs[category]:
            names = set()
            for local in range(catalog['count']):
                row = reader.record(category + 3)
                extended |= row.attributes
                packed = row.number(4)
                require(packed == catalog['group'] << 16 | local, 'endpoint positional ID')
                primary = row.number(4)
                require(primary < len(types), 'endpoint type reference')
                kind = types[primary]['kind']
                item = dict(id=packed, type=primary, offset=row.offset)
                if category == 0:
                    require(primary != 0, 'Void Field')
                    caps = row.number(1)
                    require(caps in (1, 3), 'Field capabilities')
                    row.zeros(3)
                    item['capabilities'] = caps
                else:
                    require(kind in (0, 3), 'request must be Void/Struct')
                    if category == 2:
                        response = row.number(4)
                        require(response < len(types) and types[response]['kind'] in (0, 3), 'response must be Void/Struct')
                        item['response'] = response
                name = row.text()
                require(name not in names, 'duplicate endpoint name')
                names.add(name)
                item['name'] = name
                row.done()  # only a flagged, skippable attribute area may follow the name
                endpoints[category].append(item)
    reader.done()
    # One canonical encoding per model: a descriptor without names and
    # attributes is v3.0, so minor 1 must carry at least one of them.
    require(minor == 0 or extended, 'header version: minor 1 without a type name or attribute')
    return dict(fingerprint=stored_hash, total=total, minor=minor, types=types, catalogs=catalogs,
                endpoints=endpoints, offsets=(type_start, cat_start, ep_start))
