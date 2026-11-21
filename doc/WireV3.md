# Descriptor, Values and frames: formats v3.0 and v3.1

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

[Index](README.md) · [Architecture](Architecture.md) ·
[V3 providers](../lib/integration/telemetry/v3/README.md) ·
[Transport guide](user/TransportAndResources.md) ·
[Reading a Field without the SDK](user/ReadingAFieldByHand.md)

This is the current binary contract of `descriptor.bin` and `values.bin`. Its
constants, dependency pins and independent golden fixtures are fixed in the
[freeze manifest](../tests/structured/freeze/contract.json) and the
[compile-time contract](../tests/structured/freeze/Contract.cpp).
The in-memory C++ ABI revision 6 has its own number; the canonical wire stays 3.0.
Descriptor v3.1 adds an optional type name, a skippable attribute area and
one shared tree of groups; a descriptor without them is byte-identical v3.0,
which stays frozen as the stripped form. See
[Descriptor v3.1](#descriptor-v31-type-names-and-attributes) and
[Group tree](#group-tree).

The page also holds what a client written without the SDK needs: how an
endpoint is addressed ([PackedId](#packedid)), how a client learns the IDs
([IDs from the descriptor](#ids-from-the-descriptor-and-the-fingerprint)),
how values are encoded ([Value encoding](#value-encoding)) and the request
and reply frames that carry reads, writes and calls
([TEQ1](#request-and-reply-frames-teq1-v1)). The
[walkthrough](user/ReadingAFieldByHand.md) reads one Field byte by byte.
Transport framing below a complete frame (UART, COBS) and the optional
Bind/Exchange are described separately.

<a id="навігація"></a>

## Contents

- [General rules](#general-rules)
- [Descriptor header](#descriptor-header)
- [Record header](#record-header)
- [Type records](#type-records)
- [Catalog and endpoint records](#catalog-and-endpoint-records)
- [PackedId](#packedid)
- [Fingerprint](#fingerprint)
- [IDs from the descriptor, and the fingerprint](#ids-from-the-descriptor-and-the-fingerprint)
- [Descriptor v3.1: type names and attributes](#descriptor-v31-type-names-and-attributes)
- [Default technical ceilings](#default-technical-ceilings)
- [Value encoding](#value-encoding)
- [Values header and tokens](#values-header-and-tokens)
- [Reading Values in portions](#reading-values-in-portions)
- [Request and reply frames (TEQ1 v1)](#request-and-reply-frames-teq1-v1)
- [Providers and the client](#providers-and-the-client)

<a id="загальні-правила"></a>

## General rules

- Multi-byte integers are little-endian; counts, offsets, lengths and TypeIds
  are u32 unless stated otherwise. The fingerprint is u64.
- A string is `u32 byteLength` followed by exactly that many UTF-8 bytes,
  without a NUL terminator. An embedded NUL and invalid UTF-8 are refused.
- There is no padding between records. Native pointers, `size_t`, C++ object
  layout, alignment and struct padding are not exported.
- Nonzero reserved bits or bytes, unknown record kinds, flags or versions and
  an unsupported header version are refused by a v3.0 client. A v3.1 client
  accepts minor 0 and 1; under minor 1 only the Type prefix `typeFlags` bit 0
  and the record header flags bit 0 are defined, every other reserved bit
  and byte is still refused.
- Endpoint and group names are nonempty. Endpoint names are unique within a
  catalog; catalog names are unique within a category. Different categories
  may have the same names. A group tree has its own, stricter
  [name rules](#group-tree).
- Counts, offsets, record lengths and type references are checked before use
  or allocation. The size of the whole file must match the header.

## Descriptor header

64 bytes:

| Offset | Type | Value |
| --- | --- | --- |
| 0 | 4 bytes | ASCII `TDS3` |
| 4 | u16 | major = 3 |
| 6 | u16 | minor = 0; v3.1: 1 when a type is named or a record has attributes |
| 8 | u16 | headerBytes = 64 |
| 10 | u16 | flags = 0; v3.1: bit 0 GroupTree |
| 12 | u32 | totalBytes |
| 16 | u64 | descriptorFingerprint |
| 24 | u32 | typeCount, including Void and the builtins |
| 28 | u32 | fieldCatalogCount |
| 32 | u32 | fieldCount |
| 36 | u32 | commandCatalogCount |
| 40 | u32 | commandCount |
| 44 | u32 | serviceCatalogCount |
| 48 | u32 | serviceCount |
| 52 | u32 | typesOffset |
| 56 | u32 | catalogsOffset |
| 60 | u32 | endpointsOffset |

Canonical order: Header → Types → Catalogs → Endpoints. Within Catalogs and
Endpoints the order is Field → Command → Service, then group and entry
position. Empty sections may share one start offset; sections do not overlap
and end at totalBytes.

## Record header

8 bytes before every descriptor payload:

| Offset | Type | Value |
| --- | --- | --- |
| 0 | u8 | kind: Type=1, Catalog=2, Field=3, Command=4, Service=5 |
| 1 | u8 | recordVersion = 1 |
| 2 | u16 | flags = 0; v3.1: bit 0 Attributes, bits 1..15 zero |
| 4 | u32 | payloadBytes, without the 8-byte header |

The length bounds the record. It does not let unknown semantics be accepted
silently: the rules of a new kind or minor need their own version contract.
v3.1 is that contract for minor 1: a flagged attribute area runs from the end
of the payload to `payloadBytes` and is validated and skipped by tag class;
there is no new record kind.

## Type records

A common 12-byte prefix:

```text
u32 typeId
u8  typeKind       // Void=0, Scalar=1, Enum=2, Struct=3, Array=4
u8  typeFlags      // v3.0: zero; v3.1: bit 0 Named
u8  reserved[2]    // zero
u32 wireBytes
[string name]      // v3.1, present iff Named; Enum and Struct only
```

| Kind | Payload after the prefix |
| --- | --- |
| Void | None; typeId=0, wireBytes=0 |
| Scalar | `u8 scalarCode`, 3 reserved zero bytes |
| Enum | `u32 underlyingTypeId`, `u32 entryCount`, then `{code bytes, name string}` |
| Struct | `u32 memberCount`, then `{u32 memberTypeId, memberName string}` |
| Array | `u32 elementTypeId`, `u32 elementCount` |

Scalar codes:

| Code | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Type | Bool | U8 | S8 | U16 | S16 | U32 | S32 | U64 | S64 | F32 | F64 |

A Bool payload is one byte, 0 or 1. An integer payload takes 1, 2, 4 or 8
bytes; signed values are two's complement. A floating payload is IEEE
binary32 or binary64. An enum code takes the size of its integer underlying
type and keeps its signed representation. A representable code need not be a
member of the dictionary: application validation decides which states are
allowed.

TypeIds are dense and in postorder: a composite type refers to smaller TypeIds
already described. There is no cycle, forward reference, duplicate or missing
TypeId, and no implicit alias record. An enum's underlying type is an integer;
an Array element and a Struct member cannot be Void. WireBytes is recomputed
from the graph and compared with the recorded value. This SDK's producer
always emits Void and the eleven scalars first, as TypeIds 0..11 in code
order (`f32` is 10); the format does not require it, so a client looks every
type up by its TypeId.

v3.0 has no type name; a consumer uses endpoint and member names. v3.1 adds
an optional, unique name on Enum and Struct records; arrays, scalars and
Void are never named.
Structural identity on the wire and exact C++ identity for the native API are
different contracts: two C++ structs of the same shape do not become one
exact native type.

<a id="catalog-і-endpoint-records"></a>

## Catalog and endpoint records

Catalog payload:

```text
u8  category      // Field=1, Command=2, Service=3
u8  reserved[3]   // zero
u32 groupIndex
u32 firstEntry    // ordinal among the endpoint records of this category
u32 entryCount
string name
```

Field payload:

```text
u32 packedId
u32 valueTypeId
u8  capabilities // Readable=1, Writable=2
u8  reserved[3]  // zero
string name
```

Command payload:

```text
u32 packedId
u32 requestTypeId // 0 for a zero-argument command
string name
```

Service payload:

```text
u32 packedId
u32 requestTypeId
u32 responseTypeId
string name
```

PackedId: the high 16 bits are the group position, the low 16 bits the entry
position ([PackedId](#packedid)). The categories have separate ID spaces. A
Field's capabilities are 1 (read-only) or 3 (readable and writable). In v3.0
every endpoint record ends after its name. In v3.1 any record may continue
with an attribute area flagged in its header; no tag defines units, defaults,
limits or metadata policy yet, and a tag enters the area only through its own
reviewed contract. Request and Response members are described by the type
records, which the producer's TypeRegistry emits. Empty tables are allowed; endpoint holes and reserved placeholder
entries are not introduced. Late-bound slot state changes neither the
descriptor nor the fingerprint.

## PackedId

An endpoint is addressed on the wire by a u32 PackedId together with the
operation, which selects its family:

```text
bits 31..16  group   the group's position
bits 15..0   entry   the endpoint's position among that group's endpoints of its family
```

- The family is not part of the ID; it comes from the operation (FieldRead
  and FieldWrite: Field; Command; Service). Field `0x00010000`, Command
  `0x00010000` and Service `0x00010000` are three different endpoints when
  all three exist.
- Entry numbers count per family. In a group with two Fields and one Command,
  the Fields are entries 0 and 1 and the Command is entry 0.
- In a group tree (header flag `GroupTree`) the group part is the group's
  pre-order position in the one tree that all three families share: the same
  group has the same number in every family. In a flat descriptor the group
  part is the catalog's position within its own family, so one group name can
  have different numbers in different families.
- Clients show an ID as `group:entry` in decimal: `1:0` is PackedId
  `0x00010000`, whose bytes on the wire are `00 00 01 00`.
- Every endpoint record carries its PackedId, and decoders refuse a record
  whose PackedId is not `group << 16 | entry` for its position
  (`ParseError::Endpoint`). The ID repeats the order and is checked against it.
- Both parts follow the declaration order, so adding, removing or moving a
  group or an endpoint can change the IDs after it. An ID belongs to one
  descriptor ([below](#ids-from-the-descriptor-and-the-fingerprint)).

## Fingerprint

FNV-1a-64 over all canonical descriptor bytes, with bytes 16..23 (its own
fingerprint) replaced by zeros. The basis is `0xcbf29ce484222325`, the prime
`0x100000001b3`; unsigned arithmetic modulo 2^64.

The hash covers the headers, names, the enum dictionary, type shapes, endpoint
order and Field capabilities; in v3.1 also type names and attribute areas, the
skipped ones included. Current values, owner and callback addresses and slot
state are not included. The Descriptor caches the fingerprint once; the Values
header copies it. Values are not rehashed after each READ, write or call.

It is the identity of the description, not a checksum of the live payload and
not a proof that collisions are absent. Transport integrity, access policy and
the moment of the fingerprint comparison are the application's. For an
offline export keep both files: `values.bin` does not describe its own types.

## IDs from the descriptor, and the fingerprint

A client does not compute IDs from names; it reads them from the device's
descriptor:

1. Read `/telemetry/descriptor.bin` with Resource LIST, STAT and READ
   ([file conventions](DeviceFileConventions.md)) and validate it.
2. In a group tree, read the G group records in pre-order. A record's depth
   gives its parent, and the names give each group's path (`adc/channel1`).
   A family's first entry in a group is the sum of the earlier groups'
   counts in that family.
3. Walk the endpoint records in canonical order: Field, Command, Service, then
   group and entry. A record gives the PackedId, the value, request and
   response TypeIds and the name; the endpoint's path is its group's path, `/`
   and the name ([Paths](#paths)).
4. Look each TypeId up for the value's size (`wireBytes`) and shape.

The [walkthrough](user/ReadingAFieldByHand.md#3-find-the-fields-id-and-size)
does this for one Field, byte by byte.

An ID is a position in one descriptor. Keep the fingerprint (header bytes
16..23, copied into the Values header) with every ID a client stores, a log
records or a person copies. Before using a stored ID, compare its fingerprint
with the device's: if they differ, read the descriptor again and find the
endpoint by its path. Device code takes an ID from a path at compile time
(`telemetry::idOf<model, "adc/channel1/Gain">()` or a handle's `id`), and the
generated and Dynamic clients resolve paths against the admitted descriptor,
so none of them has to store a raw ID.

## Descriptor v3.1: type names and attributes

v3.1 is an additive contract on top of the frozen v3.0 bytes. It adds two
optional parts and nothing else: a type name on Enum and Struct records, and
a skippable attribute area after any record's payload. A descriptor with
neither is byte-identical v3.0 (same bytes, same fingerprint): v3.0 stays
frozen as the stripped form. Values (`TVL3`) stays 3.0; it pairs with its
descriptor by fingerprint, not by version.

### Header minor

The header minor is 0 or 1. It is 1 exactly when at least one type is named
or at least one record has an attribute area; a nameless model without
attributes is always minor 0, so every model has one canonical encoding.
Header flags stay 0, `recordVersion` stays 1 and there is no new record kind.
A v3.1 host reads v3.0 and v3.1; a v3.0 host refuses v3.1 at the header, as
it refuses any unknown minor, which is a clear refusal and never a misread.

| Descriptor | v3.0 host | v3.1 host |
| --- | --- | --- |
| v3.0 (no names and attributes, or names stripped) | reads it | reads it; every type name is empty |
| v3.1 | refuses it at the header | reads it |

### Type names

```text
u32 typeId
u8  typeKind        // unchanged
u8  typeFlags       // v3.0: 0. v3.1: bit 0 Named, bits 1..7 zero
u8  reserved[2]     // zero
u32 wireBytes
[string name]       // present iff Named; Enum and Struct only
kind payload        // unchanged
```

- Under minor 0 every `typeFlags` is 0, exactly as in v3.0.
- Under minor 1 `typeFlags` is 0 or 1, and `Named` is allowed only on Enum
  (2) and Struct (3). Void, Scalar and Array records are never named: a host
  keeps its builtin labels and composes `Mode[4]` or `Config[2]`.
- The name is an ordinary descriptor string (1..`maxStringBytes` bytes of
  valid UTF-8, no NUL). It is unique among the descriptor's type names, by
  byte comparison; it may equal an endpoint, catalog or member name, which
  live in other namespaces.
- The name has the shape `[scope "::"] name`: at most one `::`, and each
  part nonempty and free of `:` and of the path characters `/ . [ ] \`
  (`adc::Config`, `Config`, `Вимір::Стан`; not `a::b::c`, `::Config`,
  `adc:Config` or `adc/Config`). A host can therefore split a type name and
  place it in a path without escaping.
- Hosts must not assume an identifier: an explicit name may be any valid
  UTF-8 of that shape. Automatic names are ASCII identifiers by construction:
  the type's innermost enclosing namespace or class and its own name
  (`adc::Config`), or the name alone at global or anonymous-namespace scope.

### Attribute area

```text
record header  u8 kind, u8 recordVersion = 1, u16 flags, u32 payloadBytes
               flags: bit 0 Attributes, bits 1..15 zero
payload        unchanged
[attribute]*   present iff flags bit 0; runs exactly to the end of payloadBytes:
               u8 tag, u32 length, u8 value[length]
```

- Under minor 0 the record flags are 0. The flag means at least one
  attribute; there is no area length word, because `payloadBytes` bounds the
  area and each attribute's own length makes it skippable. The header is 5
  bytes; the length stays u32, like every length in the format, because a
  value may carry whole wire values.
- Tags ascend strictly, so each tag appears at most once per record, and an
  attribute's value describes the whole record. The last attribute ends
  exactly at the record end.
- In a Struct Type record a tag that describes members has one shape: a
  presence bitmap of `ceil(memberCount / 8)` bytes (bit i is member i, at
  most 32 bytes), then one item per set bit, in member order. Command and
  Service arguments are structs, so an argument's attribute rides on its
  request struct. The shape is defined here; no tag is.
- Void and Scalar Type records never carry an area: builtins keep fixed bytes.

| Tag (u8) | Meaning | A host that does not know the tag |
| --- | --- | --- |
| `0x00`, `0x80`, `0xC0..0xFF` | reserved | refuses the descriptor |
| `0x01..0x3F` | SDK advisory, registered here; `0x01` Range, `0x02` Unit and `0x03` Default are reserved, not yet defined | skips it |
| `0x40..0x7F` | application advisory; the SDK never assigns them | skips it |
| `0x81..0xBF` | SDK critical (must be understood); none is defined in v3.1 | refuses the descriptor |

To skip an attribute is to validate its header (order, bounds) and pass its
value by its length. The fingerprint still covers it, so hosts of every age agree
on the model's identity. An addition that hosts must not ignore takes a
critical tag: it refuses only the descriptors that use it, where a minor bump
would refuse them all. v3.1 defines no tag; a tag enters the area only through
its own reviewed contract. No producer emits attributes yet, and decoders
store nothing for a skipped attribute: a descriptor decodes to the same model
as the same bytes without its attributes.

### Refusals

| Rule broken | C++ `ParseError` |
| --- | --- |
| minor other than 0 or 1; minor 1 without any name, attribute or tree; header flags other than 0 (minor 0) or GroupTree (minor 1); a tree whose three catalog counts differ | `Header` |
| `typeFlags` or record flags nonzero under minor 0; stray flag bits; a reserved tag | `Reserved` |
| `Named` on Void, Scalar or Array; an area on Void or Scalar; a type name not of the shape `[scope "::"] name` | `TypeShape` |
| two equal type names | `DuplicateName` |
| empty or overlong name | `Limit` |
| name with NUL or invalid UTF-8 | `Utf8` |
| name or attribute past its record; flag without an attribute; an attribute header cut short (1-4 bytes left); a tag out of order or repeated; an unknown critical tag | `Record` |

The JS and Python decoders refuse the same descriptors; the shared table is
[v31_cases.json](../tests/structured/descriptor/v31_cases.json), checked by the
[type-names suite](../tests/structured/type_names/README.md).

### Producer

The C++ producer emits names when the project switch `TELEMETRY_TYPE_NAMES` in
[Config.hpp](../lib/telemetry/Config.hpp) is 1 (the default); 0 compiles no
name into the image and the bytes are v3.0's. Names come from
`telemetry::reflection::typeName<T>()`: the compiler's function signature for
enums and aggregates alike, cut to the innermost scope and the type's own name
(`adc::Config`, `Adc::Mode`; the name alone at global or anonymous-namespace
scope). A template specialization, a class nested in one, or an unnamed struct
gets no name; a class declared inside a function is a build error asking for
an explicit name, because a function is no stable scope. GCC spells an inline
namespace (`v2::Status`) and Clang does not (`adc::Status`), so a model built
with both names such a type explicitly; Clang also prints a local class
without its function, so only GCC builds see that error. An explicit
`TypeReflection<T>` with `typeName("...")` or `noTypeName()` overrides the
automatic name, and `typeName("...")` refuses a name of the wrong shape at
compile time. A repeated name is `DescriptorError::DuplicateName`; one
innermost scope used twice (`left::io::Status`, `right::io::Status`) is such
a clash. `withoutTypeNames(model)` strips names
from one descriptor for a program that must emit both forms. The producer
emits no attribute.

### Group tree

A device built from branch files describes one tree of groups shared by the
three families, as a folder holds folders and files. The header flag bit 0
`GroupTree` (minor 1 only) selects it; the three catalog counts of the header
then all equal G, the number of groups, and the Catalogs section holds G
group records in pre-order:

```text
record header   kind 2 (Catalog), version 1, flags (bit 0 Attributes), payloadBytes
u8  category    0 = Group (family catalogs keep 1..3)
u8  reserved    0
u16 depth       the first is 0, each at most the previous one plus 1
u32 fieldCount, commandCount, serviceCount
string name
```

- A group's parent is the nearest earlier group one level up; a host builds
  the parent list in O(G). Its first endpoint in each family is the sum of
  the earlier groups' counts in that family, and each family's counts sum to
  the header's endpoint count.
- PackedId stays 16|16. The group part is the group's pre-order position,
  shared by all three families; the entry part is its position among the
  group's endpoints of that family. Endpoint records keep their v3.0 layout
  and order (Field, Command, Service, then group and entry).
- Names in a tree are path segments: a group or endpoint name holds no `/`,
  `.`, `[`, `]` or `\`, and names are unique among everything one group
  holds, its subgroups and the endpoints of all three families. Root groups
  are siblings too. A flat (v3.0) descriptor keeps any UTF-8 name.
- A group record costs 28 + n bytes, as a family catalog record does, but one
  per group instead of one per family that uses it. A group may be empty.

| Tree rule broken | C++ `ParseError` |
| --- | --- |
| category 0 outside tree mode, or 1..3 in it; first depth not 0; a depth jump; counts that do not sum to the header's | `Catalog` |
| `/ . [ ] \` in a group name / in an endpoint name of a tree | `Catalog` / `Endpoint` |
| two equal names in one group (subgroups and endpoints of any family) | `DuplicateName` |
| depth, name or path over the host's `ParseLimits` | `Limit` |

### Paths

One grammar names a group, an endpoint, a member and an element for every
client and for device code: groups and the endpoint joined by `/`, then
`.member` and `[i]` as needed, for example `adc/channel1/Gain`,
`adc/Config.gain` or `adc/Status.raw[1]`. Group rows carry no trailing `/`.
In a tree the path of an endpoint is unique. In a flat descriptor the group
is one segment and a name may contain the path characters, so a path writes
`\/`, `\.`, `\[`, `\]` and `\\` for them, and a Field and a Command of
the same group and name are told apart by their kind.
`RuntimeSchema::findEndpoint(path)`, `findGroup` and `resolvePath`,
`findEndpoint`/`findGroup` in telemetry.js and the Python parser, and the
device's `telemetry::read<model, path>()` read this grammar.

## Default technical ceilings

These 14 default constants in `telemetry::Limits` bound the complexity and
size of the type model and of the parser. They do not define allowed
application values, are not written in endpoint records and do not change the
fingerprint. Changing a default contract requires an explicit review of
compatibility and of the freeze gates.

| Constant | Default | What it bounds |
| --- | ---: | --- |
| `maxTypeDepth` | 32 | Nested Struct/Array; scalar and enum depth 0 |
| `maxTypeCount` | 4096 | All types, including Void and the builtins |
| `maxStructMembers` | 256 | Members of one struct |
| `maxArrayElements` | 65536 | Elements of one fixed array |
| `maxDescriptorBytes` | 4194304 | The whole descriptor |
| `maxStringBytes` | 4096 | One string |
| `maxEnumEntriesTotal` | 65536 | Sum of the enum dictionary entries |
| `maxCatalogCountTotal` | 65536 | Catalogs of all categories together |
| `maxEndpointCountTotal` | 65536 | Endpoints of all categories together |
| `maxValueWireBytes` | 1048576 | One payload type; transport capacity is separate |
| `maxExpandedValueNodes` | 262144 | Expanded complexity of one value |
| `maxGroupDepth` | 8 | Levels of a group tree: depths 0..7 |
| `maxNameBytes` | 64 | One group, endpoint or member name of a tree; one part of a type name |
| `maxPathBytes` | 256 | `group/.../endpoint` of a tree; a whole type name |

The three group-tree ceilings are a producer's: it is strict so that new
devices stay tidy, and checks them at compile time (the flat form keeps its
names). Hosts read with `ParseLimits` `groupDepth = 255`, `nameBytes = 4096`
and `pathBytes = 4096`, so every device reads.

Expanded nodes: scalar or enum = 1, Void = 0, Struct = 1 + the sum of its
member nodes, Array = 1 + N × element nodes. Overflow and ceilings are checked
before adding or multiplying, including zero-wire-size aggregate types.

The Descriptor and the JS client support explicit profiles in their documented
interfaces. A profile does not allow unsupported wire types; a client with
lower ceilings explicitly refuses a model that is too large. Profile details
are in the [provider guide](../lib/integration/telemetry/v3/README.md#validation-and-limits).

## Value encoding

Every value on the wire uses one canonical encoding, fixed by its type record:
a Field's value, a Command's request, a Service's request and response, and the
payload of a Values token.

- Exactly `wireBytes` bytes: no tags, no lengths, no padding, no alignment.
- Bool: one byte, 0 or 1; a decoder refuses any other byte.
- Integers: 1, 2, 4 or 8 bytes, little-endian; signed values in two's complement.
- `f32` and `f64`: the IEEE 754 binary32 or binary64 bit pattern, little-endian.
- Enum: the bytes of its integer underlying type. A code missing from the
  dictionary is still a valid value on the wire.
- Array: the elements in index order, each in its own encoding.
- Struct: the members in the order of the type record, which is their
  declaration order, each in its own encoding.
- Void, which is a Command or Service without a request or a Service without
  a response: zero bytes.

For example, `adc::Config { u16 rateHz; adc::Mode mode; bool enabled; }`, where
`adc::Mode` is an enum over `u8`, holding `{1000, Triggered (2), true}`. The
C++ object may carry padding; its four wire bytes do not:

<!-- bytes:config-value -->
```text
e8 03                      ; rateHz 1000 (u16)
02                         ; mode 2 = Triggered (u8)
01                         ; enabled true
```

The C++ codec is `telemetry::encode` and `telemetry::decode`
([Codec and Workspace](user/CodecAndWorkspace.md)); hosts without the SDK
follow the rules above. [tests/field_by_hand](../tests/field_by_hand/README.md)
checks this example against the codec.

<a id="values-header-і-tokens"></a>

## Values header and tokens

24-byte header:

| Offset | Type | Value |
| --- | --- | --- |
| 0 | 4 bytes | ASCII `TVL3` |
| 4 | u16 | major = 3 |
| 6 | u16 | minor = 0 |
| 8 | u32 | fieldCount |
| 12 | u32 | totalBytes |
| 16 | u64 | descriptorFingerprint |

Then the Fields in descriptor group and entry order:

```text
u8 readStatus
wireSize<T> payload bytes
```

ReadStatus: Ok=0, Unavailable=1; other codes are refused. An absent slot or
target gives Unavailable. A getter returns `T` or `const T&`; there is no
separate Field failure wrapper. On Unavailable the payload of the same size is
filled with zeros and is not interpreted as a value.

Total size: `24 + sum(1 + wireSize<T_i>)`. Empty Field catalogs give only the
header; a zero-wire-size struct still has its status byte. A consumer compares
the fingerprint with the descriptor before decoding instead of guessing the
schema from fieldCount or from the file size.

<a id="читання-values-порціями"></a>

## Reading Values in portions

Before calling the getter, the provider checks that there is room for the
whole token and the scratch it needs. The getter is called once; the payload
is fully encoded before the cursor advances. If a token does not fit, it does
not start: the getter is not called and the cursor of that token does not
move. An already completed prefix is returned with Ok; without progress, the
result is BufferTooSmall.

An owning getter gives a snapshot of one `T`; a borrowed getter lends the
object for the whole encode. The application guarantees the referent's
lifetime and stability. Whole-token transfer does not create a simultaneous
snapshot of all Fields.

A valid Values cursor is a position inside the immutable 24-byte header, the
start of a whole token or EOF. A cursor inside a payload is InvalidCursor. The
header can be split; tokens cannot. The header and the whole tokens after it
can be returned by one READ. EOF gives 0 bytes and eof=true. `size()`, STAT
and building the offset index do not call getters.

`requiredWorkspace()` accounts for scratch and alignment; `maxTokenSize()` sets
the minimum READ payload capacity for the largest Field. An insufficient
Workspace is InternalError. If there is a completed prefix, READ returns it,
and repeating from nextCursor reaches the error without replaying getters.
Output that overlaps the Workspace-backed Values returns InvalidData before
the header is written. A fully local file does not use the Workspace.

## Request and reply frames (TEQ1 v1)

Reads, writes and calls travel as TEQ1 frames: a 32-byte header and exactly
`payloadBytes` of payload, one request and one reply per invocation. The
contract is [EncodedWire.hpp](../lib/wire/EncodedWire.hpp) (`parse`, `write`,
`validate`); [Dispatcher.hpp](../lib/integration/encoded/Dispatcher.hpp) turns
a request into a reply. The [walkthrough](user/ReadingAFieldByHand.md#5-read-the-field)
shows one FieldRead and its reply byte by byte.

### Header

| Offset | Type | Field | Request | Reply |
| --- | --- | --- | --- | --- |
| 0 | 4 bytes | magic | ASCII `TEQ1`: `54 45 51 31` | the same |
| 4 | u8 | version | 1 | 1 |
| 5 | u8 | flags | 0 | 1 (Reply); any other value is refused |
| 6 | u8 | operation | 1 FieldWrite, 2 Command, 3 Service, 4 FieldRead, 5 ReadMany, 6 Batch | echoed |
| 7 | u8 | dispatch | 0 | `DispatchStatus`, [below](#dispatch) |
| 8 | u32 | session | nonzero | echoed |
| 12 | u32 | requestId | nonzero | echoed |
| 16 | u32 | endpoint | the [PackedId](#packedid); 0 for ReadMany and Batch | echoed |
| 20 | u32 | payloadBytes | the bytes after the header | the bytes after the header |
| 24 | u32 | expectedResponseBytes | the response size the client expects | echoed |
| 28 | u8 | endpointStatus | 0 | the application's status when dispatch is 0 |
| 29 | 3 bytes | reserved | zero | zero |

Operation 7 is reserved and never valid in a frame: the client's queue uses it
for raw Resource requests, which are sent without a TEQ1 header.

### The four endpoint operations

| Operation | Request payload | `expectedResponseBytes` | Reply payload when dispatch is 0 | `endpointStatus` |
| --- | --- | --- | --- | --- |
| 4 FieldRead | none | the Field's `wireBytes` | the value, exactly `expectedResponseBytes` | 0 |
| 1 FieldWrite | the value, the Field's `wireBytes` | 0 | none | `WriteResult`: 0 Applied, 1 NotFound, 2 ReadOnly, 3 InvalidValue, 4 Busy, 5 Unavailable |
| 2 Command | the request, its `wireBytes`; none without a request | 0 | none | `CommandResult`: 0 Executed, 1 Accepted, 2 NotFound, 3 Unavailable, 4 ArgumentCountMismatch, 5 InvalidValue, 6 Busy, 7 Failed |
| 3 Service | the request, its `wireBytes`; none without a request | the response's `wireBytes` | the response when `endpointStatus` is 0; otherwise none | `ServiceStatus`: 0 Ok, 1 InvalidArgument, 2 Unavailable, 3 Busy, 4 Failed |

A write to a read-only Field runs as dispatch 0 with `endpointStatus` 2,
ReadOnly. `Accepted` reports that the application queued the Command; it is
not a completion. Operations 5 ReadMany and 6 Batch are optional capabilities
with their own bodies and a response budget in `expectedResponseBytes`
(10..514 and 14..65535): see [ReadMany](ReadManyEncoded.md) and
[Mixed Batch](MixedBatchEncoded.md).

### Dispatch

| `dispatch` | Name | Meaning |
| ---: | --- | --- |
| 0 | Ok | The endpoint ran; read `endpointStatus` |
| 4 | NotFound | No endpoint of the operation's family has this PackedId |
| 5 | InvalidPayload | `payloadBytes` or `expectedResponseBytes` differs from the type's `wireBytes`, or the payload does not decode |
| 6 | BufferTooSmall | The reply does not fit the device's buffer |
| 7 | WorkspaceTooSmall | The device's scratch is too small for the operation |
| 8 | InternalError | The device could not produce a consistent result |
| 9 | Unavailable | The endpoint's late-bound owner is not bound |

A reply whose dispatch is not 0 has `endpointStatus` 0 and no payload.
Dispatch 0 means only that the endpoint ran: whether the application accepted
a write, a Command or a Service is in `endpointStatus`.

### Rules

- A request has flags 0, dispatch 0, endpointStatus 0 and a nonzero session
  and requestId. A FieldRead request has no payload; a FieldWrite or Command
  request has `expectedResponseBytes` 0. A frame that breaks a rule, or whose
  length is not 32 + `payloadBytes`, is malformed: the device runs nothing and
  sends no reply.
- The device checks the sizes against the descriptor: `payloadBytes` must
  equal the `wireBytes` of the request, or of the value for a FieldWrite, and
  `expectedResponseBytes` those of the response, or of the value for a
  FieldRead. Otherwise the reply carries dispatch 5.
- The reply echoes the operation, session, requestId, endpoint and
  `expectedResponseBytes`, so a client matches it to its request.
- There are no retries, no polling and no duplicate suppression in this
  contract: a request delivered twice runs twice.

### Over TCP

- TCP carries Resource packets and TEQ1 frames unchanged, with no further
  envelope ([TCP server](../lib/integration/tcp/README.md)). A frame whose
  first byte is `0x54` is TEQ1, and its length is 32 bytes plus the u32
  `payloadBytes` at offset 20; first bytes 1..5 are Resource LIST, STAT, READ,
  WRITE and SAVE.
- The server answers a request before it takes the next one: send one, read
  its whole reply, then send the next.
- The session binds to the connection. A device with the TCP profile lists
  `/telemetry/session.bin`; the client first writes its session there (four
  bytes, a little-endian nonzero u32, cursor 0, final), and every TEQ1 request
  carries it. Without the profile, the first valid TEQ1 request binds its
  session.
- A request with another session, or with a requestId not greater than the
  previous one, closes the connection without running anything. This binds a
  connection; it is not authentication.
- Over UART the board's framing (COBS, for example) delimits and checks
  frames; TEQ1 has no CRC ([async client](AsyncClient.md)).

<a id="providers-і-клієнт"></a>

## Providers and the client

DescriptorFile reads from any byte offset, including the middle of a record or
string. Immutable metadata and names live through all reads. ValuesFile copies
the Field index and the fingerprint from the Descriptor, but borrows the
underlying tables, names and Workspace. Both providers are read-only.
Parallel reads, including reads through copies of a provider, need a separate
Workspace or external serialization.

The current includes, a `packDescriptor` example and file registration are in
the [provider guide](../lib/integration/telemetry/v3/README.md).
The [JS client](../tests/structured/client/README.md) independently checks the
descriptor graph, canonical sizes, the fingerprint and Values tokens.
Transport framing, the resource packet header and control requests are
separate formats in the [resource protocol](../lib/resource/protocol/README.md)
and the [Bind/Exchange example](../examples/structured_protocol/README.md).
