# Descriptor і Values: формат v3.0 і v3.1

Автори: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

[Індекс](README.md) · [Архітектура](Architecture.md) ·
[V3 providers](../lib/integration/telemetry/v3/README.md) ·
[Transport guide](user/TransportAndResources.md)

Це чинний binary contract `descriptor.bin` і `values.bin`. Його constants,
dependency pins та незалежні golden fixtures зафіксовані у
[freeze manifest](../tests/structured/freeze/contract.json) та
[compile-time contract](../tests/structured/freeze/Contract.cpp).
In-memory C++ ABI revision 6 має окремий номер; canonical wire лишається 3.0.
Descriptor v3.1 adds an optional type name, a skippable attribute area and
one shared tree of groups; a descriptor without them is byte-identical v3.0,
which stays frozen as the stripped form. See
[Descriptor v3.1](#descriptor-v31-type-names-and-attributes) and
[Group tree](#group-tree).
Transport framing і optional Bind/Exchange описані окремо.

## Навігація

- [Загальні правила](#загальні-правила)
- [Descriptor header](#descriptor-header)
- [Record header](#record-header)
- [Type records](#type-records)
- [Catalog і endpoint records](#catalog-і-endpoint-records)
- [Fingerprint](#fingerprint)
- [Descriptor v3.1: type names and attributes](#descriptor-v31-type-names-and-attributes)
- [Default technical ceilings](#default-technical-ceilings)
- [Values header і tokens](#values-header-і-tokens)
- [Читання Values порціями](#читання-values-порціями)
- [Providers і клієнт](#providers-і-клієнт)

## Загальні правила

- Багатобайтові integers — little-endian; counts, offsets, lengths і TypeId
  — u32, якщо явно не зазначено інше. Fingerprint — u64.
- String — `u32 byteLength` та рівно стільки UTF-8 bytes без NUL terminator.
  Embedded NUL і некоректний UTF-8 відхиляються.
- Між records немає padding. Native pointers, `size_t`, C++ object layout,
  alignment і struct padding не експортуються.
- Nonzero reserved bits/bytes, невідомі record kinds/flags/version та
  непідтримувана version header відхиляються клієнтом v3.0. A v3.1 client
  accepts minor 0 and 1; under minor 1 only the Type prefix `typeFlags` bit 0
  and the record header flags bit 0 are defined, every other reserved bit
  and byte is still refused.
- Endpoint/group names непорожні. Endpoint names у catalog унікальні;
  catalog names унікальні в межах категорії. Різні категорії можуть мати
  однакові імена.
- Counts, offsets, record lengths і type references перевіряються до
  використання чи allocation. Розмір повного файла має відповідати header.

## Descriptor header

64 bytes:

| Offset | Тип | Значення |
| --- | --- | --- |
| 0 | 4 bytes | ASCII `TDS3` |
| 4 | u16 | major = 3 |
| 6 | u16 | minor = 0; v3.1: 1 when a type is named or a record has attributes |
| 8 | u16 | headerBytes = 64 |
| 10 | u16 | flags = 0; v3.1: bit 0 GroupTree |
| 12 | u32 | totalBytes |
| 16 | u64 | descriptorFingerprint |
| 24 | u32 | typeCount, включно з Void і builtins |
| 28 | u32 | fieldCatalogCount |
| 32 | u32 | fieldCount |
| 36 | u32 | commandCatalogCount |
| 40 | u32 | commandCount |
| 44 | u32 | serviceCatalogCount |
| 48 | u32 | serviceCount |
| 52 | u32 | typesOffset |
| 56 | u32 | catalogsOffset |
| 60 | u32 | endpointsOffset |

Canonical order: Header → Types → Catalogs → Endpoints. Усередині Catalogs
і Endpoints порядок Field → Command → Service, далі group/entry position.
Порожні sections можуть мати однаковий start offset; sections не
перекриваються й закінчуються на totalBytes.

## Record header

8 bytes перед кожним descriptor payload:

| Offset | Тип | Значення |
| --- | --- | --- |
| 0 | u8 | kind: Type=1, Catalog=2, Field=3, Command=4, Service=5 |
| 1 | u8 | recordVersion = 1 |
| 2 | u16 | flags = 0; v3.1: bit 0 Attributes, bits 1..15 zero |
| 4 | u32 | payloadBytes, без 8-byte header |

Length визначає межу record. Воно не дозволяє мовчки прийняти невідому
семантику. Правила нового kind/minor потребують окремого version contract.
v3.1 is that contract for minor 1: a flagged attribute area runs from the end
of the payload to `payloadBytes` and is validated and skipped by tag class;
there is no new record kind.

## Type records

Спільний 12-byte prefix:

```text
u32 typeId
u8  typeKind       // Void=0, Scalar=1, Enum=2, Struct=3, Array=4
u8  typeFlags      // v3.0: zero; v3.1: bit 0 Named
u8  reserved[2]    // zero
u32 wireBytes
[string name]      // v3.1, present iff Named; Enum and Struct only
```

| Kind | Payload після prefix |
| --- | --- |
| Void | Немає; typeId=0, wireBytes=0 |
| Scalar | `u8 scalarCode`, 3 reserved zero bytes |
| Enum | `u32 underlyingTypeId`, `u32 entryCount`, далі `{code bytes, name string}` |
| Struct | `u32 memberCount`, далі `{u32 memberTypeId, memberName string}` |
| Array | `u32 elementTypeId`, `u32 elementCount` |

Scalar codes:

| Код | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Тип | Bool | U8 | S8 | U16 | S16 | U32 | S32 | U64 | S64 | F32 | F64 |

Bool payload — один byte 0 або 1. Integer payload займає 1/2/4/8 bytes,
signed representation — two's complement; floating payload — IEEE
binary32/binary64. Enum code займає розмір integer underlying type й
зберігає його signed representation. Representable code не зобов'язаний
бути членом словника: application validation визначає допустимі states.

TypeIds щільні й у postorder: складний type посилається на вже описані
менші TypeIds. Немає cycle, forward reference, duplicate/missing TypeId
або implicit alias record. Enum underlying — integer; Array element і
Struct member не можуть бути Void. WireBytes повторно обчислюється з
графа й порівнюється із записаним значенням.

v3.0 has no type name; a consumer uses endpoint and member names. v3.1 adds
an optional, unique name on Enum and Struct records; arrays, scalars and
Void are never named.
Structural identity на wire та exact C++ identity для native API — різні
контракти; два C++ structs однакової форми не стають exact native type.

## Catalog і endpoint records

Catalog payload:

```text
u8  category      // Field=1, Command=2, Service=3
u8  reserved[3]   // zero
u32 groupIndex
u32 firstEntry    // ordinal у відповідній категорії endpoint records
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

PackedId: high 16 bits — group position, low 16 bits — entry position.
Категорії мають окремі ID spaces. In v3.0 every endpoint record ends after
its name. In v3.1 any record may continue with an attribute area flagged in
its header; no tag defines units, defaults, limits or metadata policy yet,
and a tag enters the area only through its own reviewed contract.
Request/Response members описує TypeRegistry. Порожні tables дозволені;
endpoint holes чи reserved placeholder entries не вводяться.
Late-bound slot state не змінює descriptor або fingerprint.

## Fingerprint

FNV-1a-64 над усіма canonical descriptor bytes, із bytes 16..23 власного
fingerprint заміненими на нулі. Basis — `0xcbf29ce484222325`, prime —
`0x100000001b3`; unsigned arithmetic modulo 2^64.

Hash включає headers, names, enum dictionary, type shapes, endpoint order
і Field capabilities; in v3.1 also type names and attribute areas, the
skipped ones included. Поточні values, owner/callback addresses та slot
state не входять. Descriptor кешує fingerprint один раз; Values header
копіює його. Значення не перехешовуються після кожного READ/write/call.

Це identity опису, а не checksum live payload або доказ відсутності
колізій. Transport integrity, access policy та момент порівняння
fingerprint визначає application. Для offline export збережіть обидва
файли: `values.bin` сам не описує своїх типів.

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

Ці 14 default constants у `telemetry::Limits` обмежують складність та
обсяг type model/parser. Вони не визначають допустимі прикладні values,
не записуються в endpoint records і не змінюють fingerprint. Зміна
default contract потребує явного перегляду compatibility та freeze gates.

| Constant | Default | Що обмежує |
| --- | ---: | --- |
| `maxTypeDepth` | 32 | Вкладені Struct/Array; scalar/enum depth 0 |
| `maxTypeCount` | 4096 | Усі types, включно з Void/builtins |
| `maxStructMembers` | 256 | Members одного struct |
| `maxArrayElements` | 65536 | Elements одного fixed array |
| `maxDescriptorBytes` | 4194304 | Повний descriptor |
| `maxStringBytes` | 4096 | Один string |
| `maxEnumEntriesTotal` | 65536 | Сума enum dictionary entries |
| `maxCatalogCountTotal` | 65536 | Catalogs усіх категорій разом |
| `maxEndpointCountTotal` | 65536 | Endpoints усіх категорій разом |
| `maxValueWireBytes` | 1048576 | Один payload type; transport capacity окрема |
| `maxExpandedValueNodes` | 262144 | Розгорнута складність одного value |
| `maxGroupDepth` | 8 | Levels of a group tree: depths 0..7 |
| `maxNameBytes` | 64 | One group, endpoint or member name of a tree; one part of a type name |
| `maxPathBytes` | 256 | `group/.../endpoint` of a tree; a whole type name |

The three group-tree ceilings are a producer's: it is strict so that new
devices stay tidy, and checks them at compile time (the flat form keeps its
names). Hosts read with `ParseLimits` `groupDepth = 255`, `nameBytes = 4096`
and `pathBytes = 4096`, so every device reads.

Expanded nodes: scalar/enum=1, Void=0, Struct=1+сума member nodes,
Array=1+N×element nodes. Overflow і ceiling перевіряються перед
додаванням/множенням, включно з zero-wire-size aggregate types.

Descriptor і JS client підтримують явні profiles у своїх documented
interfaces. Profile не дозволяє unsupported wire types; client з нижчими
ceilings явно відхиляє завелику модель. Деталі профілів — у
[provider guide](../lib/integration/telemetry/v3/README.md#validation-and-limits).

## Values header і tokens

24-byte header:

| Offset | Тип | Значення |
| --- | --- | --- |
| 0 | 4 bytes | ASCII `TVL3` |
| 4 | u16 | major = 3 |
| 6 | u16 | minor = 0 |
| 8 | u32 | fieldCount |
| 12 | u32 | totalBytes |
| 16 | u64 | descriptorFingerprint |

Далі Fields у descriptor group/entry order:

```text
u8 readStatus
wireSize<T> payload bytes
```

ReadStatus: Ok=0, Unavailable=1; інші коди відхиляються. Відсутній
slot/target дає Unavailable. Getter повертає `T` або `const T&`; окремого
Field failure wrapper немає. При Unavailable payload того самого розміру
заповнений нулями й не інтерпретується як value.

Загальний розмір: `24 + sum(1 + wireSize<T_i>)`. Empty Field catalogs дають
тільки header; zero-wire-size struct усе одно має status byte.
Consumer звіряє fingerprint із descriptor перед decode, а не вгадує
схему за fieldCount чи розміром файла.

## Читання Values порціями

До getter provider перевіряє місце для цілого token і потрібний scratch.
Getter викликається один раз; payload повністю кодується до просування
cursor. Якщо token не вміщується, він не починається: getter не
викликається й cursor цього token не рухається. Уже завершений prefix
повертається з Ok; без progress — BufferTooSmall.

Owning getter дає snapshot одного `T`; borrowed getter позичає object
на час цілого encode. Application гарантує lifetime і стабільність
referent. Whole-token transfer не створює одночасний snapshot усіх Fields.

Допустимий Values cursor — позиція всередині immutable 24-byte header,
початок повного token або EOF. Cursor усередині payload — InvalidCursor.
Header можна розрізати; tokens не розрізаються. Header і наступні повні
tokens можуть бути видані одним READ. EOF дає 0 bytes та eof=true.
`size()`, STAT і побудова offset index не викликають getters.

`requiredWorkspace()` враховує scratch/alignment; `maxTokenSize()` задає
мінімальну READ payload capacity для найбільшого Field. Недостатній
Workspace — InternalError. Якщо є завершений prefix, READ повертає його
й повторення від nextCursor дістається до помилки без replay getters.
Output overlap із Workspace-backed Values повертає InvalidData до запису
header. Повністю local файл Workspace не використовує.

## Providers і клієнт

DescriptorFile читається з будь-якого byte offset, включно з серединою
record/string. Immutable metadata й names живуть протягом усіх reads.
ValuesFile копіює Field index і fingerprint із Descriptor, але позичає
underlying tables/names та Workspace. Обидва providers read-only.
Parallel reads, зокрема через копії provider, потребують окремого
Workspace або зовнішньої серіалізації.

Чинні includes, приклад `packDescriptor` та file registration наведені
у [provider guide](../lib/integration/telemetry/v3/README.md).
[JS client](../tests/structured/client/README.md) незалежно перевіряє
descriptor graph, canonical sizes, fingerprint і Values tokens.
Transport framing, resource packet header та контрольні запити — окремі
формати в [resource protocol](../lib/resource/protocol/README.md) і
[Bind/Exchange example](../examples/structured_protocol/README.md).
