# Model: endpoint catalogs і structural types

Автори: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

[Посібник](README.md) · [API Overview](APIOverview.md) · [Таблиці й каталоги](TablesAndCatalogs.md) ·
[Codec і Workspace](CodecAndWorkspace.md) · [Descriptor і Values](DescriptorAndValues.md)

`Model` збирає Field, Command і Service catalogs та виводить із їхніх C++ типів
один `TypeRegistry`. Він дає runtime indexes, спільні TypeIds і межі buffer/scratch
для encoded operations. Значення пристрою залишаються у application owners;
модель позичає каталоги, а registry описує лише структуру типів.

Це canonical сторінка побудови Model, reflection/type metadata та
[encoded access](#encoded-api-catalog-index-або-modelview). Для локального
native доступу Model не створюють; encoded catalog indexes теж працюють
без Model. Standalone Codec і generic Resource — незалежні сценарії.

Поточна реалізація міститься у [Model.hpp](../../lib/telemetry/model/Model.hpp),
[Registry.hpp](../../lib/telemetry/type/Registry.hpp) та
[Descriptor.hpp](../../lib/telemetry/type/Descriptor.hpp). Ця сторінка описує
наявний C++20 API.

## Навігація

- [Ролі об'єктів](#ролі-обєктів)
- [Повний приклад](#повний-приклад)
- [Model API](#model-api)
- [Group tree: a Model from branch files](#group-tree-a-model-from-branch-files)
- [Typed access і runtime access](#typed-access-і-runtime-access)
- [Encoded API: catalog index або ModelView](#encoded-api-catalog-index-або-modelview)
- [Encoded результати й порядок перевірок](#encoded-результати-й-порядок-перевірок)
- [Headers, compiled sources та ABI](#headers-compiled-sources-та-abi)
- [ModelView](#modelview)
- [TypeRegistry і TypeDescriptor](#typeregistry-і-typedescriptor)
- [Positional roots і registry roots](#positional-roots-і-registry-roots)
- [Reflection та форма payload](#reflection-та-форма-payload)
- [Порядок IDs і зміни declaration](#порядок-ids-і-зміни-declaration)
- [Межі та порожні категорії](#межі-та-порожні-категорії)
- [Lifetime, synchronization і ABI](#lifetime-synchronization-і-abi)

## Ролі об'єктів

| Об'єкт | Що він містить | Для чого потрібний |
| --- | --- | --- |
| Application owner / callable / slot | Прикладний стан або callback target | Виконати getter, setter, Command чи Service |
| `FieldDefinition`, `CommandDefinition`, `ServiceDefinition` | Ім'я та native binding | Зберегти точну C++ сигнатуру і target |
| Local table | Власні definitions і похідні runtime entries | Typed position access та перевірений runtime selection |
| `TableGroup` / catalog table | Назву групи, позичені local tables і власні catalog rows | Об'єднати local positions у packed-ID простір |
| `Model` | Вказівники на три catalogs; тип `Registry` виведений із roots | Зіставити всі endpoint types та runtime families |
| `ModelView` | Позичені runtime indexes і паралельні TypeId catalogs | Передати повну модель до compiled adapter чи provider |
| `TypeRegistry` | Статичні descriptors точних C++ типів | Призначити TypeIds і описати залежності типів |
| `reflection` | Member, enum і callable facts | Перевірити payload shape та побудувати structural metadata |
| Codec | Canonical bytes конкретного native payload | Encode або decode у existing object / scoped Workspace object |

```mermaid
flowchart TD
    O[Application state and callbacks] --> D[Native endpoint definitions]
    D --> T[Local tables]
    T --> N[Native typed access]
    T --> C[Named catalogs]
    C --> N
    C --> I[Family runtime indexes]
    I --> E[Checked encoded calls]
    C --> M[Model borrows catalogs]
    M --> R[One static TypeRegistry]
    M --> V[ModelView]
    V --> E
    V --> P[Descriptor and Values providers]
    F[Reflection facts] --> R
    F --> K[Canonical codec]
    K --> E
    A[Independent supported C++ DTO] --> K
```

Native виклик можна виконати прямо через local table або catalog до створення
`Model`. Модель потрібна для спільного registry, TypeIds, `ModelView` і
зручних maxima всіх families; для одного family index вона необов'язкова.
Сам `Model` не має native
`read<...>()`, `write<...>()` чи `call<...>()`: ці операції належать tables і catalogs.

| Сценарій | Мінімально потрібні об'єкти |
| --- | --- |
| Native local position | Application owner/binding і відповідна local table |
| Native global packed ID | Ті самі local tables і catalog table відповідної family |
| Encoded catalog access | Catalog table, її `index()`, wire buffers та Workspace; Model не потрібна |
| Compiled encoded adapter | Узгоджений `ModelView`, wire buffers та Workspace; звичайний спосіб створення view — `model.view()` |
| Standalone Codec | Supported native T та bytes; Workspace лише для new-object decode |
| Generic Resource | Paths/providers і filesystem; telemetry tables та Model не потрібні |

## Positional roots і registry roots

Таблиці й каталоги мають два різні compile-time списки:

`RootTypes` і `RegistryRootTypes` — публічні контракти для метаданих,
призначені для авторів адаптерів і перевірок на етапі компіляції. Для читання, запису
й викликів використовуйте table/catalog API; для інформації про типи —
`model.types()` і `Model::typeId<T>()`. `RegistryRootTypes` не визначає
endpoint IDs, кількість рядків чи бізнес-логіку застосунку.

| Alias | Порядок і кількість | Споживач |
| --- | --- | --- |
| `RootTypes` | Усі declared roots; повтори зберігаються | Positional metadata, кількість Field tokens у `ValuesFile` |
| `RegistryRootTypes` | Унікальні normalized C++ типи; перше входження зберігається | Побудова `Model::Registry` |

Field має один value root, Command — один request root, Service — пару
request/response roots у порядку `Request0, Response0, Request1, Response1, ...`.
`Void` також залишається у positional списку. У каталозі списки йдуть у порядку
груп, а всередині груп — у порядку оголошення endpoints.

Наприклад, 100 Fields одного типу мають `RootTypes::size == 100` і
`RegistryRootTypes::size == 1`. Values зберігає всі 100 значень і 100 tokens
плюс EOF; Registry описує спільний тип один раз. Кількість endpoints,
packed IDs і positional TypeId arrays від дедуплікації не зменшуються.

Дедуплікація виконується в local table, потім між tables у каталозі.
`Model` передає Registry ці скорочені списки в порядку Fields → Commands →
Services. Registry, як і раніше, додає залежності перед типом-контейнером;
однаковий wire shape не об'єднує різні C++ типи. TypeIds, canonical descriptor
bytes і fingerprint тому не змінюються від самого скорочення повторених roots.

Catalog-like adapter із колишнім `RootTypes` без `RegistryRootTypes` також
приймається: Model виводить унікальні registry roots із його positional списку.
Для адаптера з власним `RegistryRootTypes` цей alias має дотримуватися того
самого порядку першого входження; він не може приховувати типи endpoints.

Це оптимізація template computation. Вона не змінює runtime entry layouts,
storage budget чи indexed dispatch і не гарантує побайтово однаковий ELF:
конкретна спеціалізація `Model::Registry` та її symbol names можуть змінитися.
Для великих проектів дивіться вимірювання й окремі compiler ceilings у
[Scalability](../Scalability.md). Прямий `TypeRegistry<Roots...>` і далі приймає
переданий користувачем список; hierarchical reduction стосується tables,
catalogs і Model.

## Повний приклад

Приклад показує одну Field-структуру, Command без request і Service з aggregate
request/response. Він використовує owning Service result та borrowed Field
getter. Усі declarations мають стабільні адреси, а application state живе довше
за таблиці.

```cpp
#include <telemetry/Telemetry.hpp>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

namespace ts = telemetry;

struct Config {
    float limit;
    bool enabled;
};

struct Query {
    std::uint16_t channel;
};

struct Response {
    float limit;
    std::uint16_t channel;
};

struct Device {
    Config config{230.0f, true};

    const Config& readConfig() const noexcept
    {
        return config;
    }

    ts::WriteResult writeConfig(const Config& next) noexcept
    {
        if (!std::isfinite(next.limit) || next.limit < 1.0f || next.limit > 1000.0f) {
            return ts::WriteResult::InvalidValue;
        }
        config = next;
        return ts::WriteResult::Applied;
    }

    ts::CommandResult restoreDefaults() noexcept
    {
        config = {230.0f, true};
        return ts::CommandResult::Executed;
    }

    ts::ServiceResult<Response> query(const Query& request) const noexcept
    {
        if (request.channel >= 3) {
            return ts::ServiceResult<Response>::failure(ts::ServiceStatus::InvalidArgument);
        }
        return ts::ServiceResult<Response>::success({config.limit, request.channel});
    }
};

inline Device device;
inline constexpr ts::FieldTable localFields{
    ts::field<&Device::readConfig, &Device::writeConfig>("Config", device),
};
inline constexpr ts::CommandTable localCommands{
    ts::command<&Device::restoreDefaults>("RestoreDefaults", device),
};
inline constexpr ts::ServiceTable localServices{
    ts::service<&Device::query>("Query", device),
};

inline constexpr ts::FieldCatalogTable fields{ts::group("settings", localFields)};
inline constexpr ts::CommandCatalogTable commands{ts::group("control", localCommands)};
inline constexpr ts::ServiceCatalogTable services{ts::group("queries", localServices)};
inline constexpr ts::Model model{fields, commands, services};

inline constexpr auto configId = ts::makeId<0, 0>();
inline constexpr auto queryId = ts::makeId<0, 0>();

int main()
{
    const auto native = fields.read<configId>();
    assert(native && native.valueOrNull() == &device.config);

    const auto nativeReply = services.call<queryId>(Query{2});
    assert(nativeReply.hasValue() && nativeReply.value().channel == 2);

    const auto view = model.view();
    assert(view.fieldTypeId(configId) == model.typeId<Config>());
    const auto serviceTypes = view.serviceTypeIds(queryId);
    assert(serviceTypes && serviceTypes->requestTypeId == model.typeId<Query>());
    assert(serviceTypes->responseTypeId == model.typeId<Response>());
    assert(model.types().find(model.typeId<Config>())->kind == ts::TypeKind::Struct);

    std::array<std::byte, model.maxScratch()> scratch{};
    ts::Workspace workspace{scratch};
    std::array<std::byte, ts::wireSize<Config>> configBytes{};
    const Config next{240.0f, true};
    assert(ts::encode(next, configBytes) == ts::CodecStatus::Ok);

    const auto written = ts::writeFieldEncoded(view, configId, configBytes, workspace);
    assert(written.dispatch == ts::DispatchStatus::Ok);
    assert(written.endpointStatus == ts::WriteResult::Applied);
    assert(device.config.limit == 240.0f);

    std::array<std::byte, ts::wireSize<Query>> requestBytes{};
    std::array<std::byte, ts::wireSize<Response>> responseBytes{};
    const Query request{1};
    assert(ts::encode(request, requestBytes) == ts::CodecStatus::Ok);
    const auto reply = ts::callServiceEncoded(view, queryId, requestBytes,
                                            responseBytes, workspace);
    assert(reply.dispatch == ts::DispatchStatus::Ok);
    assert(reply.endpointStatus == ts::ServiceStatus::Ok);
    assert(reply.written == responseBytes.size());
    assert(workspace.used() == 0);
}
```

Для compiled calls у прикладі потрібно лінкувати
[Adapter.cpp](../../lib/telemetry/model/Adapter.cpp) і
[StructuredAbi.cpp](../../lib/telemetry/abi/StructuredAbi.cpp).
[telemetry.pri](../../lib/telemetry/telemetry.pri) додає їх один раз у qmake build.
Manual C++20 build має include roots `lib`, `lib/boost_pfr/include` і `lib/magic_enum`.

Числові значення `configId` і `queryId` однакові: families мають окремі логічні
простори. Transport передає також вид операції, який вибирає потрібний index.

## Group tree: a Model from branch files

A device with many subsystems describes each one in its own branch file, one
expression with no tables and no IDs, and mounts the branches in one Model.
`ts::group(name, items...)` takes subgroups, Fields, Commands and Services in
any mix; a group may be empty and may mount another branch.

```cpp
// adc/AdcTelemetry.hpp - Adc.hpp knows nothing of telemetry
inline constexpr auto branch = ts::group("adc",
    ts::group("channel1", ts::field<&Adc::gain<0>, &Adc::setGain<0>>("Gain", instance)),
    ts::group("channel2", ts::field<&Adc::gain<1>, &Adc::setGain<1>>("Gain", instance)),
    ts::field<&Adc::config, &Adc::setConfig>("Config", instance),   // type adc::Config
    ts::command<&Adc::reset>("Reset", instance),
    ts::service<&Adc::calibrate>("Calibrate", instance));

// Telemetry.cpp - the one composition point
inline constexpr telemetry::Model model{adc::tlm::branch, metrology::tlm::branch};
inline constexpr telemetry::descriptor::Descriptor descriptor{model};   // v3.1, GroupTree

const auto gain = telemetry::read<model, "adc/channel1/Gain">();        // std::optional<float>
const auto rate = telemetry::read<model, "adc/Config.rateHz">();        // a member of the Field
const auto status = telemetry::write<model, "adc/channel2/Gain">(2.5f); // whole Fields only
const auto reset = telemetry::call<model, "adc/Reset">();               // Command or Service
static_assert(model.idOf<"adc/channel1/Gain">() == telemetry::makeId(1, 0));
```

- The tree is flattened in pre-order at compile time: the group, then its
  subgroups in declaration order. A group's position is the group part of
  every PackedId in all three families (`adc` 0, `adc/channel1` 1, ...); its
  entries keep declaration order inside each family. Each family keeps
  today's catalog array, now one row per group, empty where a group has none
  of that family, so dispatch stays O(1) through the unchanged `ModelView`
  and the structured ABI revision stays 6.
- The Model owns each group's tables and the three catalog tables
  (`model.fields()`, `commands()`, `services()`); it is neither copyable nor
  movable. `model.flat()` is the equivalent three-catalog Model.
- Names in a tree are path segments: no `/`, `.`, `[`, `]` or `\`, and unique
  among everything a group holds (subgroups and endpoints of all three
  families). Both are build errors at the `ts::group` that breaks them.
  Ceilings come from the descriptor's profile: `maxGroupDepth` 8 levels,
  `maxNameBytes` 64, `maxPathBytes` 256.
- `read<>` takes a Field or a member or element of it: it calls the whole
  Field's getter, returns the selected part in `std::optional`, and adds no
  string or table to the image. `write<>` takes whole Fields, because a
  member write would be a hidden read-modify-write; `call<>` takes Commands
  and Services. A path that names nothing is a build error whose trace shows
  the path as `EndpointPath<N>{"..."}`.
- C++20 cannot read an object's names inside its own member function at
  compile time, so the model is a template argument of `read`, `write`,
  `call` and `idOf`; `model.idOf<"...">()` is the one member form, a
  consteval call on a constexpr model.
- Today's `Model{fields, commands, services}` is unchanged and keeps v3.0
  bytes; three branches select the tree form.

## Model API

| Виклик | Результат / дія |
| --- | --- |
| `Model{fields, commands, services}` | Позичає саме ці lvalue catalogs; temporary catalogs відхиляються |
| `Model::Registry` | Тип повного registry, виведений із roots трьох families |
| `model.types()` | `TypeRegistryView` на immutable type metadata; метод також доступний статично |
| `model.typeId<T>()` | `consteval TypeId` точного нормалізованого C++ типу; відсутній тип дає compile error |
| `model.fieldIndex()` | Позичений `FieldIndex` на runtime rows |
| `model.commandIndex()` | Позичений `CommandIndex` на runtime rows |
| `model.serviceIndex()` | Позичений `ServiceIndex` на runtime rows |
| `model.view()` | Повний `ModelView` із indexes і matching TypeId catalogs |
| `model.maxFieldScratch()` | Максимум read/write scratch серед Fields |
| `model.maxCommandScratch()` | Максимум request scratch серед Commands |
| `model.maxServiceScratch()` | Максимум одночасного request/result scratch серед Services |
| `model.maxScratch()` | Максимум попередніх трьох меж для однієї encoded operation |
| `model.maxFieldWireSize()` | Найбільший Field payload у canonical bytes |
| `model.maxCommandRequestWireSize()` | Найбільший Command request у canonical bytes; no-request має 0 |
| `model.maxServiceRequestWireSize()` | Найбільший Service request у canonical bytes; no-request має 0 |
| `model.maxServiceResponseWireSize()` | Найбільший Service response payload у canonical bytes |

`maxScratch()` включає запас для вирівнювання byte span. Він достатній для
однієї operation на Workspace без активних outer leases. Nested calls потребують
суми всіх одночасно живих leases або окремих Workspaces. Межа не описує stack
callback, compiler spills чи transport packet headers.

Wire maxima є `constexpr` та рахують лише canonical payload bytes, без native
padding і framing. Порожня family або лише no-request endpoints дають 0 для
відповідної request межі.

## Typed access і runtime access

| Шлях | Приклад | Що перевіряється |
| --- | --- | --- |
| Local typed Field | `localFields.read<0>()`, `localFields.write<0>(next)` | Position і точний native type під час compilation |
| Global typed Field | `fields.read<configId>()`, `fields.write<configId>(next)` | Packed group/entry під час compilation |
| Explicit Field conversion | `fields.readAs<Config, configId>()`, `fields.writeAs<configId>(next)` | Exact structural type або checked numeric conversion |
| Native Command | `commands.call<ts::makeId<0, 0>()>()` | Request shape із declaration; callback повертає `CommandResult` |
| Native Service | `services.call<queryId>(Query{1})` | Exact aggregate request; native owning/borrowed result |
| Runtime typed selection | `fields.visit(id, visitor)` | Original integer width і catalog bounds перед borrowed definition callback |
| Runtime erased operation | `model.fieldIndex().readEncoded(id, output, workspace)` | ID, wire capacity та scratch rules |
| Compiled erased operation | `ts::readFieldEncoded(model.view(), id, output, workspace)` | Ті самі правила плюс exact ABI tag у linked symbol |

Для runtime Field conversions catalog має `readAs<To>(id)` та `writeAs(id, value)`.
Невідповідний structural type не викликає getter/setter: читання повертає
`nullopt`, запис — `InvalidValue`. Runtime numeric conversion завершується
до application mutation. Повні сигнатури й ordering шаблонних аргументів є у
[Fields](Fields.md) та [TablesAndCatalogs](TablesAndCatalogs.md).

`forEach` і `visit` позичають definitions; вони не матеріалізують endpoint values
автоматично. `visit` повертає `bool` вибору: `true` означає, що visitor отримав
definition. Return value visitor і status окремого endpoint не визначають цей `bool`.

Точні encoded calls та їхні results описані наступним розділом. Native
methods і conversions — у [Native API](NativeApi.md), [Fields](Fields.md),
[Commands](Commands.md) та [Services](Services.md).

## Encoded API: catalog index або ModelView

Encoded access працює з runtime endpoint ID та canonical payload bytes.
Callback залишається тим самим, що у native definition; другого endpoint
implementation або окремого runtime `EncodedApi` object немає. Framing,
operation envelope, correlation і delivery визначає застосунок.

### A. Catalog indexes без Model

Local `FieldTable`, `CommandTable` і `ServiceTable` **не мають `index()`**.
Цей метод належить відповідним **catalog tables**:

| Catalog table | Runtime view | Checked methods |
| --- | --- | --- |
| `FieldCatalogTable::index() const&` | `FieldIndex` | `readEncoded(id, output, workspace)`, `writeEncoded(id, input, workspace)` |
| `CommandCatalogTable::index() const&` | `CommandIndex` | `executeEncoded(id, input, workspace)` |
| `ServiceCatalogTable::index() const&` | `ServiceIndex` | `callEncoded(id, input, output, workspace)` |

```cpp
// Fragments: fields/commands/services are named catalog tables.
auto read = fields.index().readEncoded(id, output, workspace);
auto write = fields.index().writeEncoded(id, input, workspace);
auto command = commands.index().executeEncoded(id, input, workspace);
auto service = services.index().callEncoded(id, input, output, workspace);
```

Indexes — copyable borrowed views. Catalogs, local tables, names, owners і slots
живуть довше за їх використання. `index()` з temporary catalog відхиляється;
копія index не подовжує lifetime arrays. Для checked encoded access до одного
local row є `localFields[position].readEncoded(...)` / `writeEncoded(...)`,
`localCommands[position].executeEncoded(...)` і
`localServices[position].callEncoded(...)`. Тут caller спершу забезпечує
`position < localTable.size()`, бо `operator[]` не перевіряє bounds.

Index methods приймають integral ID та перевіряють original width до narrowing.
Від'ємний, ширший за `PackedId` або absent group/entry дає `NotFound`.
Сам lookup — bounds checks і пряме індексування; обходу назв немає.
Group і entry — дві zero-based 16-bit positions. Families мають окремі ID spaces.
TypeId описує тип і не використовується для вибору endpoint.

### B. ModelView і compiled adapter

`model.fieldIndex()` / `commandIndex()` / `serviceIndex()` повертають ті самі
family views. Якщо transport/module має повний `ModelView`, використовуйте
чотири public functions із [Adapter.hpp](../../lib/telemetry/model/Adapter.hpp):

| Function | Result |
| --- | --- |
| `readFieldEncoded(ModelView, PackedId, span<byte> output, Workspace&)` | `EncodedReadResult` |
| `writeFieldEncoded(ModelView, PackedId, span<const byte> input, Workspace&)` | `EncodedWriteResult` |
| `executeCommandEncoded(ModelView, PackedId, span<const byte> input, Workspace&)` | `EncodedCommandResult` |
| `callServiceEncoded(ModelView, PackedId, span<const byte> input, span<byte> output, Workspace&)` | `EncodedCallResult` |

```cpp
// Fragments: model, id and buffers are supplied by the caller.
auto read = ts::readFieldEncoded(model.view(), id, output, workspace);
auto write = ts::writeFieldEncoded(model.view(), id, input, workspace);
auto command = ts::executeCommandEncoded(model.view(), id, input, workspace);
auto service = ts::callServiceEncoded(model.view(), id, input, output, workspace);
```

Ці calls працюють із payload span. Збір повного UART/TCP frame, connection state,
request correlation та формування transport response виконує application layer;
див. [Transport walkthrough](TransportWalkthrough.md).

`ModelView` передається за значенням, але позичає arrays. Звичайний спосіб
отримати coherent view — `model.view()`. Вільні functions приймають уже
`PackedId` (`u32`): перед передаванням ширшого transport числа його треба перевірити
самостійно, щоб implicit narrowing не вибрав інший endpoint. Це відрізняється
від integral-template Index methods, які перевіряють original width самі.

## Encoded результати й порядок перевірок

Ці C++ result structs не є wire headers. Їхні names, fields і типи різні:

| Result | Fields | Як перевіряти |
| --- | --- | --- |
| `EncodedReadResult` | `DispatchStatus dispatch`, `uint32_t written` | `dispatch == Ok`; payload займає тільки prefix `[0, written)`; application status немає |
| `EncodedWriteResult` | `DispatchStatus dispatch`, `WriteResult endpointStatus` | Спершу dispatch, потім setter outcome; `written` немає |
| `EncodedCommandResult` | `DispatchStatus dispatch`, `CommandResult endpointStatus` | Спершу dispatch, потім callback outcome; `written` немає |
| `EncodedCallResult` | `DispatchStatus dispatch`, `ServiceStatus endpointStatus`, `uint32_t written` | Спершу dispatch, потім Service outcome і лише reported response prefix |

`endpointStatus` meaningful тільки при `dispatch == Ok`. Його initialized
success value при dispatch failure не означає успішний callback. Service
refusal може мати `dispatch == Ok`, `endpointStatus == Busy` і `written == 0`.
Successful void Service теж має `written == 0`, тому саме count не визначає success.

| `DispatchStatus` | Meaning |
| --- | --- |
| `Ok` | Encoded dispatch завершений; оцініть application status, якщо він є |
| `NotFound` | Integral ID invalid або selected group/entry відсутній |
| `InvalidPayload` | Невірна exact input length, bool representation або заборонений overlap |
| `BufferTooSmall` | Output менший за advertised successful wire payload |
| `WorkspaceTooSmall` | Required scratch/alignment reservation недоступні |
| `Unavailable` | Compatible getter/setter/callback binding або slot unavailable |
| `InternalError` | Невідомий Field/Command application status або порушення internal invariant |

| Operation | Input length | Output capacity |
| --- | --- | --- |
| Field read | Input відсутній | Не менше `FieldEntry::wireBytes`; решта span не належить reported payload |
| Writable Field write | Рівно `wireBytes` | Payload output відсутній |
| Command | Рівно `requestWireBytes`; void request — 0 | Payload output відсутній |
| Service | Рівно `requestWireBytes`; void request — 0 | Не менше `responseWireBytes`, навіть якщо callback пізніше відмовить |

Порядок refusal важливий для transport diagnostics:

1. Index спочатку вибирає endpoint: `NotFound` перед span/scratch checks.
2. Read-only Field write повертає `{Ok, ReadOnly}` до input validation.
   Writable Field перевіряє exact input; Field read — output capacity.
3. Command перевіряє exact input. Service перевіряє exact input, потім output
   capacity, потім required Workspace overlap і remaining scratch capacity.
4. Якщо decoded object leased, reservation capacity/alignment перевіряється
   до bool validation. Для local object representation перевіряється без reservation.
   Validation завершена до callback target resolution та invocation.
5. Empty binding дає dispatch `Unavailable`. Engaged setter/Command/Service,
   який сам повернув `Unavailable`, має dispatch `Ok` і application status
   `Unavailable`. Field/Command unknown numeric callback code дає `InternalError`.
6. Borrowed read/Service actual response address перевіряється проти output payload prefix
   після getter/callback. Така overlap refusal не відкочує його побічних ефектів.

Для scratch користуйтеся `FieldEntry::readScratchBytes` / `writeScratchBytes`,
`CommandEntry::scratchBytes` і `ServiceEntry::scratchBytes`, або Model maxima.
Declared capability і requirements не змінюються від empty slot. Model bound
достатній для однієї operation на fresh Workspace, не для всіх nested live objects.

Length/capacity checks виконуються на public checked boundary. Внутрішні thin
thunks уже знають exact extents і використовують shared codec kernel без
повторного standalone preflight. Не викликайте raw operation pointers у entries
як public API. Heap немає; local/Workspace storage policy та overlap rules
мають один canonical опис у [Codec and Workspace](CodecAndWorkspace.md#local-object-budget).
Fully local endpoint не звертається до Workspace, а використаний scratch має
бути disjoint від wire buffers. Service input/output overlap дозволений, бо
Request повністю decoded до response write.

## Headers, compiled sources та ABI

Простий public include — `<telemetry/Telemetry.hpp>`. Для вужчого catalog
access є `<telemetry/field/FieldCatalogs.hpp>`,
`<telemetry/command/CommandCatalogs.hpp>` і
`<telemetry/service/ServiceCatalogs.hpp>`. Compiled API оголошено у
`<telemetry/model/Adapter.hpp>`. C++20, `lib`, `lib/boost_pfr/include` і
`lib/magic_enum` — build settings поточного reflection/type backend.

| Access | Compiled source dependency |
| --- | --- |
| Catalog index / checked local entry | Header-defined operation; `Adapter.cpp` не потрібний |
| Чотири ModelView functions | `lib/telemetry/model/Adapter.cpp`; exact layout tag є частиною linked symbol |
| Явний `requireStructuredAbi()` | `lib/telemetry/abi/StructuredAbi.cpp`, який визначає current specialization |

`telemetry.pri` у стандартному compiled configuration додає обидва `.cpp`
для зручності. Це не означає, що direct index потребує compiled adapter чи
що кожний header-only TU автоматично посилається на ABI guard. Для власного
module boundary guard викликають один раз із живого коду, який linker не
видаляє; не додають його до кожного native/encoded hot call. Exact tag
adapter автоматично захищає саме цей compiled boundary. Revision, layout
і configuration macros мають збігатися між modules; див.
[Lifetime, synchronization і ABI](#lifetime-synchronization-і-abi).

Повна програма compiled/transport шляху —
[Encoded.cpp](../../examples/user_guide/Encoded.cpp). Catalog index operations
того самого набору declarations є у
[Native.cpp](../../examples/user_guide/Native.cpp). Framing приклади —
[Transport walkthrough](TransportWalkthrough.md).

## ModelView

`ModelView` — copyable erased view. Він містить runtime indexes і TypeId catalogs
для тих самих rows. Створюйте його через `model.view()`, щоб arrays і counts
відповідали одне одному. Public data members потрібні також для exact ABI layout
checks; library не перевіряє коректність довільно створених pointer/count pairs.

| API / member | Значення |
| --- | --- |
| `types` | `TypeRegistryView` із immutable descriptors |
| `fields`, `commands`, `services` | Runtime indexes відповідних families |
| `fieldTypes` / `fieldCatalogCount` | Паралельні Field value TypeId catalogs |
| `commandTypes` / `commandCatalogCount` | Паралельні Command request TypeId catalogs |
| `serviceTypes` / `serviceCatalogCount` | Паралельні request/response TypeId catalogs |
| `fieldTypeId(id)` | `optional<TypeId>` для Field value |
| `commandTypeId(id)` | `optional<TypeId>` для Command request; no-request Command має Void |
| `serviceTypeIds(id)` | `optional<ServiceTypePair>` для Service request/response |

TypeId lookup приймає integral packed ID, перевіряє його до narrowing і повертає
`nullopt` для невідомих positions. Lookup metadata не викликає application callback.
Копія `ModelView` позичає ті самі arrays і owners, не створюючи snapshot live state.

Runtime indexes також мають `forEachEntry(visitor)` і
`forEachEntryWhile(visitor)`. Вони передають packed ID, group name та
`const FieldEntry&`/`const CommandEntry&`/`const ServiceEntry&` у declaration
order; callback не потребує concrete table type. `forEachEntry` ігнорує result,
а `forEachEntryWhile` вимагає саме `bool`: false зупиняє прохід та повертає
false; повний або порожній прохід повертає true. Ці виклики позичають rows
та не виконують endpoint callbacks.

## TypeRegistry і TypeDescriptor

`TypeRegistry<Roots...>` можна використати окремо. Для metadata, яку передають
разом із endpoints повного пристрою, використовуйте `Model::Registry`: local table
не знає всіх roots і не може самостійно призначити matching global TypeIds.

| Registry API | Результат |
| --- | --- |
| `Registry::typeCount` | Кількість descriptors включно з builtins |
| `Registry::recordsBytes` | Сумарний розмір wire type records для descriptor |
| `Registry::typeId<T>()` | Compile-time position точного `remove_cvref_t<T>` |
| `Registry::descriptor<Id>()` | Посилання на descriptor; неправильний Id відхиляється під час compilation |
| `Registry::view()` | Позичений `TypeRegistryView` на static arrays |
| `view.find(id)` | Pointer на descriptor або `nullptr` для невідомого TypeId |

`TypeRegistryView` має `data`, `count` і `recordsBytes`. `data` позичає immutable
array із `count` descriptors; `recordsBytes` рахує type records, а не повний
Descriptor file з його header, catalogs та endpoint records. `find` приймає вже
типізований `TypeId` (`uint32_t`); якщо external число ширше, перевірте його
діапазон до conversion у цей тип.

Built-in positions мають фіксований порядок:
`void`, `bool`, U8, S8, U16, S16, U32, S32, U64, S64, F32, F64.
Власні roots додаються після них. Enum спершу додає underlying type, array —
element type, struct — member types у declaration order, потім додається сам type.
Повторний точний C++ тип додається один раз. Два різні aggregate types із тією
самою формою залишаються двома registry entries.

```cpp
// Standalone metadata fragment; Config, Query and Response are declared above.
using Registry = ts::TypeRegistry<Config, Query, Response>;
constexpr auto configType = Registry::typeId<Config>();
constexpr const auto& descriptor = Registry::descriptor<configType>();
static_assert(descriptor.kind == ts::TypeKind::Struct);
static_assert(descriptor.wireBytes == ts::wireSize<Config>);
```

| `TypeDescriptor` field | Для якого kind / що означає |
| --- | --- |
| `id`, `kind` | Для всіх types: registry position та structural kind |
| `wireBytes` | Canonical value extent без native padding; Void має 0 |
| `recordBytes` | Розмір descriptor type record разом із v3 record header |
| `scalarCode` | Scalar: Bool, U8/S8 … F32/F64 |
| `relatedTypeId` | Enum: underlying type; Array: element type |
| `elementCount` | Array: fixed element count |
| `memberData`, `memberCount` | Struct: declaration-order type/name records |
| `enumData`, `enumCount` | Enum: numeric-code/name dictionary |
| `member(index)` | Checked member pointer або `nullptr` |
| `enumEntry(index)` | Checked enum-entry pointer або `nullptr` |

`MemberDescriptor` містить `typeId` і позичений `name`. `EnumEntryDescriptor`
містить `codeBits` і позичений `name`. Kind-specific counters і pointers
описують лише відповідний kind; наприклад, scalar не має struct members.

`EnumEntryDescriptor::codeBits` зберігає unsigned bit pattern underlying value
у молодших `wireBytes` bytes, включно з negative signed codes.
Descriptors не містять native field offsets, owner addresses, units, defaults
чи допустимі application ranges. Їхня задача — structural interpretation bytes.

## Reflection та форма payload

| Facade | Поточні public facts | Роль |
| --- | --- | --- |
| [`Aggregate.hpp`](../../lib/telemetry/reflection/Aggregate.hpp) | `memberCount<T>`, `MemberType<I,T>`, `memberName<I,T>()`, `get<I>(object)` | Declaration-order aggregate members; `get` приймає lvalue та зберігає cv/ref |
| [`Callable.hpp`](../../lib/telemetry/reflection/Callable.hpp) | `Function<Signature>`, `EndpointTraits<Kind,Signature>` | Exact result/arguments, arity, owner qualifiers, noexcept і owning/borrowed shape |
| [`Enum.hpp`](../../lib/telemetry/reflection/Enum.hpp) | `Enum<E>`, `EnumReflection<E>`, `enumEntry`, `enumCodes`, `enumEntries` | Automatic або explicit dictionary normalized за underlying value |
| [`Traits.hpp`](../../lib/telemetry/type/Traits.hpp) | `Type<T>`, `typeKind<T>`, `wireSize<T>`, `expandedNodes<T>` | Recursive representation, size, depth та traversal checks |

`Type<T>` нормалізує `T` через `remove_cvref_t`. У кожного supported type є
`kind`, `wireSize`, `depth` і `expandedNodes`; variable templates вище дають
короткий доступ до kind, wire size та node count. `void` має zero bytes, zero
depth і zero nodes. Scalar та enum мають depth zero; array/struct додають
один рівень до найбільшої вкладеної depth.

| Shape-specific `Type<T>` facts | Значення |
| --- | --- |
| Scalar: `code` | `ScalarCode` canonical representation |
| Enum: `EnumType`, `Underlying` | Reflected dictionary facade та exact underlying integer type |
| Array: `Element`, `count` | Exact element type та fixed extent |
| Struct: `memberCount`, `Member<I>` | Declaration-order count та compile-time facts одного member |
| Struct member: `Native`, `name`, `bytes`, `nodes`, `depth` | Member type, reflected name, wire extent і recursive bounds |

`reflection::Function<Signature>` описує exact function type, function/member
pointer, однозначний callable object або slot signature. Воно не обирає live
slot target і не перевіряє його availability.

| `Function<Signature>` fact | Значення |
| --- | --- |
| `Result`, `Arguments` | Exact result type і `std::tuple` declared argument types зі збереженням cv/ref |
| `Owner` | Declaring class для member pointer; `void` для free function/signature |
| `arity`, `isMember` | Кількість declared arguments та наявність declaring owner |
| `isConst`, `isVolatile` | Owner method qualifiers |
| `refQualifier` | `RefQualifier::None`, `LValue` або `RValue` |
| `isNoexcept`, `isVariadic` | Declared invocation qualifiers |

`reflection::EndpointTraits<Kind, Signature>` додає normalized endpoint shape.
Public aliases — `Callable`, `Result`, `Arguments`, `RequestShape`, `Request`
і `Response`; `Response` є `void` для Field/Command. Public facts — `kind`,
`wrapsServiceResult`, `wrapsBorrowedServiceResult`, `borrowsResult` і
`borrowsResponse`. RequestShape містить `type` та `supported`. Цей facade
перевіряє спільні qualifier/arity правила, а Field/Command/Service factory
додатково перевіряє свій status type і aggregate contract. Успішний
`EndpointTraits` сам по собі не заміняє construction відповідної definition.

Для `reflection::Enum<E>` public alias — `Underlying`, public count —
`entryCount`. `entryValue<I>()` повертає exact enum
code, `entryName<I>()` — `string_view` його normalized name; неправильний static
index відхиляється. `enumEntry(value, name)` створює один named entry;
`enumEntries(...)` збирає словник із цих entries, а `enumCodes<...>()` використовує
імена явно вибраних named enumerators. `enumEntries<E>()` задає explicit empty
dictionary. Усі ці factories є `consteval`.

```cpp
// Reflection fragment; Config and Device are declared in the full example.
using GetterFacts = ts::reflection::Function<decltype(&Device::readConfig)>;
static_assert(GetterFacts::isNoexcept && GetterFacts::isConst);
static_assert(GetterFacts::arity == 0);
static_assert(std::is_same_v<typename GetterFacts::Result, const Config&>);
static_assert(ts::reflection::memberCount<Config> == 2);
static_assert(ts::reflection::memberName<0, Config>() == "limit");
static_assert(ts::Type<Config>::memberCount == 2);
static_assert(ts::Type<Config>::depth == 1);
```

Підтримуються fixed-width binary integers, `bool`, IEEE binary32/binary64,
scoped enums із supported integer underlying type, `std::array` і
standard-layout trivially copyable/destructible aggregates. Struct members
мають бути mutable values; raw arrays, pointers, references, volatile members
і packed member alignment не входять до підтриманого shape.

Field getter повертає exact native `T` або `const T&`; setter — `WriteResult`.
Command має нуль або один aggregate request і повертає `CommandResult`.
Service request/response — aggregate або void; Service може повернути
`ServiceResult<T>`, `BorrowedServiceResult<T>` чи exact `const T&`.
Callbacks мають бути `noexcept`. Generic/overloaded callbacks можна адаптувати
через slot із exact signature; [slot guide](../../lib/telemetry/slot/README.md)
описує правила збереження mutable references.

Automatic member/enum names перевіряються як ASCII identifiers. Explicit endpoint
і enum labels мають бути valid nonempty UTF-8 без embedded NUL.
`EnumReflection<E>` потрібно specialize до першого використання `Enum<E>`.
Навіть explicit empty dictionary залишає всі representable underlying codes
допустимими для value codec: dictionary описує назви, а application validator
визначає, які режими пристрій може прийняти.

## Порядок IDs і зміни declaration

Endpoint ID містить group у старших 16 bits і local entry у молодших 16 bits.
Group/entry order та TypeId order є positional. У Model roots проходяться
послідовно: Fields, Commands, Services; для Service request перед response.

Перестановка groups, entries або reflected members змінює structural meaning,
навіть коли names не змінилися. Додавання раніше використаного нового type може
зрушити наступні user TypeIds. Не зберігайте TypeId як незалежний permanent type
номер: consumer має використовувати descriptor саме цієї моделі.

Для descriptor agreement і fingerprint використовуйте
[Descriptor і Values](DescriptorAndValues.md) та
[Bind/Exchange example](../../examples/structured_protocol/README.md).
Binding/reset slot не змінює declared capability чи structural shape.

## Межі та порожні категорії

| `Limits` member | Поточне значення | Meaning / де перевіряється |
| --- | ---: | --- |
| `maxTypeCount` | 4 096 | Type count включно з builtins; `TypeRegistry` |
| `maxTypeDepth` | 32 | Type nesting depth; recursive `Type<T>` |
| `maxStructMembers` | 256 | Struct members; `Type<T>` і reflection shape |
| `maxArrayElements` | 65 536 | Array elements; `Type<T>` |
| `maxValueWireBytes` | 1 048 576 | Value wire bytes; `Type<T>` |
| `maxExpandedValueNodes` | 262 144 | Expanded value nodes; `Type<T>` |
| `maxDescriptorBytes` | 4 194 304 | Descriptor bytes; TypeRegistry type-record sum і serialized Descriptor validation |
| `maxEnumEntriesTotal` | 65 536 | Загальна кількість enum entries; `TypeRegistry` |
| `maxStringBytes` | 4 096 | Name string bytes; Name / reflection validation |
| `maxCatalogCountTotal` | 65 536 | Загальна кількість catalogs у serialized descriptor; Descriptor validation |
| `maxEndpointCountTotal` | 65 536 | Загальна кількість endpoints у serialized descriptor; Descriptor validation |

Local entries або catalog groups однієї family мають окрему positional межу
65 536: її перевіряють table/catalog static assertions, оскільки кожний component
packed ID має 16 bits.

Це structural ceilings з [Limits](../../lib/telemetry/type/Traits.hpp).
Descriptor providers також перевіряють власні aggregate endpoint/catalog і
serialized-size limits; їхній `valid()` не замінює application value validation.
Конкретний compile-time reflection backend та compiler мають підтримувати
обраний shape.

У pinned C++20 Boost.PFR backend практична межа одного aggregate — **200 direct
members**, хоча normalized ceiling `maxStructMembers` дорівнює 256. Nested DTOs
можуть розділити більшу прикладну структуру. Compile-time вартість також залежить
від кількості roots, exact DTO types і local table size; дивіться окремі
[вимірювання масштабування](../Scalability.md). Acceptance ceilings не обіцяють
успішну збірку за default compiler budgets чи вміщення у Flash кожного MCU.

Для порожніх catalogs можна оголосити:

```cpp
inline constexpr ts::FieldCatalogTable<> noFields{};
inline constexpr ts::CommandCatalogTable<> noCommands{};
inline constexpr ts::ServiceCatalogTable<> noServices{};
inline constexpr ts::Model emptyModel{noFields, noCommands, noServices};
```

Fields/Commands також мають ready-made `ts::emptyFields` і `ts::emptyCommands`;
Services передають як `ServiceCatalogTable<>`. Порожня модель усе одно має
built-in TypeRegistry, нуль runtime rows і `maxScratch() == 0`.

## Lifetime, synchronization і ABI

Local tables та catalog tables noncopyable/nonmovable: erased contexts або
runtime views можуть посилатися на їхні внутрішні arrays. `Model` позичає catalogs,
а його копія позичає ті самі catalogs. Names, owners, callable lvalues, slots
і всі transitive references живуть довше за кожний виклик через ці views.
Static registry arrays існують незалежно від lifetime конкретної копії Model.

Application layer забезпечує serialization bind/reset із calls і coherence
live values. `ModelView` не робить atomic snapshot кількох Fields. Getter може
повернути application snapshot або позичити стабільний aggregate; borrowed view
не захищає aggregate від паралельного запису іншою task.

Compiled adapters включають
[CurrentStructuredAbiTag](../../lib/telemetry/abi/StructuredAbi.hpp) у symbol name.
Tag охоплює revision, layout і `TELEMETRY_STRUCTURED_LOCAL_BYTES`. Невідповідні
separately built modules не зможуть залінкувати саме цей boundary. Для власного
header-only module boundary із in-memory views можна явно викликати
`ts::requireStructuredAbi()` та лінкувати `StructuredAbi.cpp`.
Значення storage macro має бути однаковим у всіх translation units.

Далі: [Codec і Workspace](CodecAndWorkspace.md) пояснює exact spans, payload
construction і status handling; [Descriptor і Values](DescriptorAndValues.md)
показує, як передати structural metadata та live Field values consumer.
