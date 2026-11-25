# Model: дерево груп і structural types

Автори: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

[Посібник](README.md) · [API Overview](APIOverview.md) · [Таблиці й каталоги](TablesAndCatalogs.md) ·
[Codec і Workspace](CodecAndWorkspace.md) · [Descriptor і Values](DescriptorAndValues.md)

`Model` збирає дерево груп із Fields, Commands і Services та виводить із їхніх
C++ типів один `TypeRegistry`. Він дає runtime indexes, спільні TypeIds і межі buffer/scratch
для encoded operations. Значення пристрою залишаються у application owners;
модель позичає каталоги, а registry описує лише структуру типів.

Це canonical сторінка побудови Model, reflection/type metadata та
[encoded access](#encoded-api-catalog-index-або-modelview). Для локального
native доступу до однієї local table Model не створюють. Standalone Codec і
generic Resource — незалежні сценарії.

**Одна форма — дерево груп.** `ts::group(name, items...)` і
`Model{branches...}` описують пристрій, шляхи й handles адресують endpoints
([нижче](#group-tree-модель-із-файлів-гілок)). Це єдиний шлях: плоску
`Model{fields, commands, services}`, catalog tables, публічний `makeId` і
typed доступ за ID прибрано ([міграція](../StructuredTelemetryV3MigrationGuide.md#one-way-the-group-tree-paths-and-handles)). Catalog engine під
деревом внутрішній ([Tables and catalogs](TablesAndCatalogs.md)).

Поточна реалізація міститься у [GroupTree.hpp](../../lib/telemetry/model/GroupTree.hpp),
[TreePath.hpp](../../lib/telemetry/model/TreePath.hpp), [Model.hpp](../../lib/telemetry/model/Model.hpp),
[Registry.hpp](../../lib/telemetry/type/Registry.hpp) та
[Descriptor.hpp](../../lib/telemetry/type/Descriptor.hpp). Ця сторінка описує
наявний C++20 API.

## Навігація

- [Group tree: модель із файлів гілок](#group-tree-модель-із-файлів-гілок)
- [Ролі об'єктів](#ролі-обєктів)
- [Model API](#model-api)
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

## Group tree: модель із файлів гілок

Пристрій із багатьма підсистемами описує кожну в окремому файлі гілки: один
вираз без таблиць і IDs. Гілки монтуються в одну Model.
`ts::group(name, items...)` приймає subgroups, Fields, Commands і Services у
будь-якій суміші; група може бути порожньою і може монтувати іншу гілку.
Повний приклад такого компонування, з класами, які нічого не знають про
telemetry, — [group_tree](../../examples/group_tree/README.md).

```cpp
// adc/AdcTelemetry.hpp - сам Adc.hpp нічого не знає про telemetry
inline constexpr auto branch = ts::group("adc",
    ts::group("channel1", ts::field<&Adc::gain<0>, &Adc::setGain<0>>("Gain", instance)),
    ts::group("channel2", ts::field<&Adc::gain<1>, &Adc::setGain<1>>("Gain", instance)),
    ts::field<&Adc::config, &Adc::setConfig>("Config", instance),   // тип adc::Config
    ts::command<&Adc::reset>("Reset", instance),
    ts::service<&Adc::calibrate>("Calibrate", instance));

// Telemetry.hpp - єдина точка композиції
inline constexpr telemetry::Model model{adc::tlm::branch, metrology::tlm::branch};
inline constexpr telemetry::descriptor::Descriptor descriptor{model};   // v3.1, GroupTree

// Named handles: шлях перевіряється тут, один раз.
inline constexpr auto gain1 = telemetry::endpoint<model, "adc/channel1/Gain">;
inline constexpr auto rate = telemetry::endpoint<model, "adc/Config.rateHz">; // лише читання
inline constexpr auto reset = telemetry::endpoint<model, "adc/Reset">;
inline constexpr auto calibrate = telemetry::endpoint<model, "adc/Calibrate">;

gain1.read();                            // std::optional<float>
gain1.write(2.5f);                       // WriteResult
rate.read();                             // std::optional<std::uint16_t>
reset.call();                            // CommandResult
calibrate.call(adc::Reference{1.25f});   // ServiceResult<adc::Offset>
```

- Дерево розгортається в pre-order під час компіляції: група, потім її
  subgroups у порядку оголошення. Позиція групи є group-частиною кожного
  PackedId у всіх трьох families (`adc` 0, `adc/channel1` 1, ...); entries
  зберігають порядок оголошення всередині своєї family. Кожна family має
  звичний catalog array, тепер по одному рядку на групу, порожньому там, де
  група не має цієї family. Dispatch лишається O(1) через незмінний
  `ModelView`, а structured ABI revision лишається 6.
- Model володіє tables кожної групи й трьома внутрішніми catalogs; її не можна
  копіювати чи переміщувати. `model.fields()`, `commands()` і `services()` дають
  runtime-ID доступ і traversal ([нижче](#typed-access-і-runtime-access)).
- Імена в дереві є сегментами шляху: без `/`, `.`, `[`, `]` і `\`, унікальні
  серед усього, що містить група (subgroups і endpoints усіх трьох families).
  Порушення будь-якого правила — build error у тому `ts::group`, що його
  порушує. Стелі задає profile descriptor: `maxGroupDepth` 8 рівнів,
  `maxNameBytes` 64, `maxPathBytes` 256.

### Шлях до endpoint

Один запис шляху для всіх: групи та endpoint через `/`, далі `.member` і
`[i]` за потреби: `adc/channel1/Gain`, `adc/Config.rateHz`,
`adc/Status.raw[1]`, `metrology/Config`. Рядок групи не має кінцевого `/`.
Той самий текст приймають ключі Dynamic clients, `RuntimeSchema::findEndpoint(path)`,
`inspect` генератора і compile-time доступ пристрою нижче. У дереві імена не
містять службових символів, тому шлях однозначний; плоска v3.0 модель зберігає
будь-який UTF-8 в іменах, і host екранує такі символи в шляху через `\`.

### Named handles

`telemetry::endpoint<model, "path">` розв'язує шлях один раз, там, де handle
оголошено, і далі працює через крапку: `gain1.read()`, `gain1.write(2.5f)`,
`reset.call()`. Model має бути `constexpr` зі static storage, тому handles
оголошують після неї, у файлі композиції; файл гілки моделі не знає.

| Член handle | Значення |
| --- | --- |
| `id` | `PackedId` endpoint, той самий, що дає `idOf<model, path>()` |
| `kind` | `reflection::EndpointKind::Field`, `Command` або `Service` |
| `member` | `true` для handle члена чи елемента Field |
| `Value` | Тип, який вибирає `read()`: увесь Field або член; `void` для Command/Service |
| `Definition` | Конкретна `FieldDefinition`, `CommandDefinition` чи `ServiceDefinition` із її `Value`, `Request`, `Response` |
| `read()` | Field: те саме, що `read<model, path>()` |
| `write(value)` | Лише цілий Field; тип значення перевіряється, як у `write<model, path>()` |
| `call(args...)` | Command або Service з їхніми native arguments |

- Шлях, що нічого не називає, називає групу, член не-struct або елемент за
  межами масиву, — build error в рядку оголошення handle; trace показує шлях
  як `EndpointPath<N>{"..."}`.
- Handle — порожній object: PackedId і тип значення є частиною його типу.
  Виклик компілюється в той самий код, що `read<model, path>()` і внутрішній
  typed доступ engine за ID, і текст шляху не потрапляє в image. [Group-tree suite](../../tests/structured/group_tree/README.md)
  порівнює ARM-код усіх трьох форм.
- Handle члена (`"adc/Config.rateHz"`) лише читає: getter виконується для всього
  Field, повертається вибрана частина в `std::optional`. `write()` на ньому —
  build error, бо запис члена був би прихованим read-modify-write, а wire
  записує цілі Fields. `read()` на Command, `write()` на Command чи Service і
  `call()` на Field — так само build errors із поясненням.
- Field без setter приймає `write()` і повертає `WriteResult::ReadOnly`, як і
  `write<model, path>()`.

### Вільні функції шляху

Для разового доступу без handle лишаються вільні функції з тим самим шляхом:

```cpp
const auto gain = telemetry::read<model, "adc/channel1/Gain">();        // std::optional<float>
const auto rate = telemetry::read<model, "adc/Config.rateHz">();        // член Field
const auto status = telemetry::write<model, "adc/channel2/Gain">(2.5f); // лише цілі Fields
const auto reset = telemetry::call<model, "adc/Reset">();               // Command або Service
static_assert(model.idOf<"adc/channel1/Gain">() == 0x00010000u); // група 1, позиція 0
```

- `read<>` приймає Field, його член або елемент: виконує getter усього Field і
  повертає вибрану частину в `std::optional`, не додаючи в image ні рядків,
  ні таблиць. `write<>` приймає цілі Fields, `call<>` — Commands і Services.
- C++20 не може прочитати імена об'єкта у його власній member function під
  час компіляції, тому Model — template argument у `read`, `write`, `call`,
  `idOf` та `endpoint`; `model.idOf<"...">()` — єдина member форма, consteval
  виклик на constexpr model.
- Публічного `makeId` і typed доступу за compile-time ID немає: ID відомого
  endpoint дає `idOf<model, path>()` або `handle.id`. ID — позиція в одному descriptor;
  клієнт зберігає його разом із fingerprint
  ([WireV3: PackedId](../WireV3.md#packedid)).

### Імена типів у дереві

Кожен enum і struct отримує ім'я автоматично: найближчий scope і власне ім'я
типу. Тому два `Config` із різних гілок стають `adc::Config` і
`metrology::Config`, без жодного рядка в файлах гілок. Override через
`TypeReflection` пишуть у файлі гілки, якій належить тип, перед її group
expression. Правила, override і вимкнення імен —
[Fields: імена типів у descriptor](Fields.md#8-імена-типів-у-descriptor).

### Ті самі шляхи в клієнтах

Static C++ клієнт (`device.read<"adc/Config">(cb)`, `device.adc.Config`),
TS/JS (`await device.read('adc/Config')`) і Dynamic клієнт
(`client.endpoint("adc/Config")`) приймають той самий шлях:
[одна операція в чотирьох місцях](APIOverview.md#одна-операція-в-чотирьох-місцях).

## Ролі об'єктів

| Об'єкт | Що він містить | Для чого потрібний |
| --- | --- | --- |
| Application owner / callable / slot | Прикладний стан або callback target | Виконати getter, setter, Command чи Service |
| `FieldDefinition`, `CommandDefinition`, `ServiceDefinition` | Ім'я та native binding | Зберегти точну C++ сигнатуру і target |
| `ts::group(name, items...)` | Назву групи, definitions і підгрупи | Гілка дерева; шлях `group/.../endpoint` |
| `Model{branches...}` | Розгорнуте дерево: tables кожної групи, три внутрішні catalogs і тип `Registry` | Зіставити всі endpoint types та runtime families; шляхи й handles |
| `endpoint<model, "path">` | PackedId, family і тип значення в типі handle | Native `read()`, `write()`, `call()` без тексту шляху в image |
| `model.fields()`, `commands()`, `services()` | Позичений доступ до catalogs моделі | Runtime-ID доступ (`visit`, `readAs<T>(id)`, `writeAs(id, v)`, `call(id)`, `callAs`) і traversal |
| Local table (`FieldTable`, ...) | Власні definitions і похідні runtime entries без Model | Typed position access та перевірений runtime selection |
| `ModelView` | Позичені runtime indexes і паралельні TypeId catalogs | Передати повну модель до compiled adapter чи provider |
| `TypeRegistry` | Статичні descriptors точних C++ типів | Призначити TypeIds і описати залежності типів |
| `reflection` | Member, enum і callable facts | Перевірити payload shape та побудувати structural metadata |
| Codec | Canonical bytes конкретного native payload | Encode або decode у existing object / scoped Workspace object |

```mermaid
flowchart TD
    O[Application state and callbacks] --> D[Native endpoint definitions]
    D --> G[ts::group branches]
    G --> M[Model owns group tables and internal catalogs]
    M --> N[Native access by path and handle]
    M --> I[Family runtime indexes]
    I --> E[Checked encoded calls]
    M --> R[One static TypeRegistry]
    M --> V[ModelView]
    V --> E
    V --> P[Descriptor and Values providers]
    F[Reflection facts] --> R
    F --> K[Canonical codec]
    K --> E
    A[Independent supported C++ DTO] --> K
```

У дереві native виклик іде через handle або вільні функції шляху:
`ts::endpoint<model, path>`, `ts::read/write/call<model, path>()`. Сам `Model`
не має member `read<...>()`, `write<...>()` чи `call<...>()`, бо C++20 не може
взяти constexpr model зі свого `this`; `model.idOf<"...">()` — єдина member
форма.

| Сценарій | Мінімально потрібні об'єкти |
| --- | --- |
| Native доступ за шляхом | Application owner/binding, `ts::group` і constexpr `Model` зі static storage |
| Native runtime ID | Constexpr `Model`; `model.fields()`, `commands()` чи `services()` |
| Native local position | Application owner/binding і local table, без Model |
| Compiled encoded adapter | Узгоджений `ModelView`, wire buffers та Workspace; звичайний спосіб створення view — `model.view()` |
| Standalone Codec | Supported native T та bytes; Workspace лише для new-object decode |
| Generic Resource | Paths/providers і filesystem; telemetry tables та Model не потрібні |

## Positional roots і registry roots

Таблиці й каталоги мають два різні compile-time списки:

`RootTypes` і `RegistryRootTypes` — публічні контракти для метаданих,
призначені для авторів адаптерів і перевірок на етапі компіляції. Для читання, запису
й викликів використовуйте шляхи й handles; для інформації про типи —
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

## Model API

| Виклик | Результат / дія |
| --- | --- |
| `Model{branches...}` | Розгортає гілки `ts::group` під час компіляції й володіє їхніми tables; не копіюється й не переміщується |
| `model.idOf<"path">()` | `consteval PackedId` endpoint за шляхом; хибний шлях дає compile error |
| `model.fields()`, `model.commands()`, `model.services()` | Runtime-ID доступ і traversal: `visit(id, visitor)`, `readAs<T>(id)`, `writeAs(id, value)`, `call(id[, request])`, `callAs`, `forEach` |
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
| Handle або шлях | `gain1.read()`, `ts::read<model, "adc/Config.rateHz">()`, `ts::call<model, "adc/Reset">()` | Шлях, сімейство, тип значення й аргументи під час compilation |
| Local typed Field | `localFields.read<0>()`, `localFields.write<0>(next)` | Position і точний native type під час compilation |
| Runtime conversion | `model.fields().readAs<Config>(id)`, `model.fields().writeAs(id, next)` | Original integer width, bounds, exact structural type або checked numeric conversion |
| Runtime Command і Service | `model.commands().call(id)`, `model.services().callAs<Response>(id, request)` | Bounds і request shape; flat outcome ([FlatNative](FlatNative.md)) |
| Runtime typed selection | `model.fields().visit(id, visitor)` | Original integer width і catalog bounds перед borrowed definition callback |
| Runtime erased operation | `model.fieldIndex().readEncoded(id, output, workspace)` | ID, wire capacity та scratch rules |
| Compiled erased operation | `ts::readFieldEncoded(model.view(), id, output, workspace)` | Ті самі правила плюс exact ABI tag у linked symbol |

Для runtime Field conversions `model.fields()` має `readAs<To>(id)` та `writeAs(id, value)`.
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

### A. Runtime indexes моделі

`model.fieldIndex()`, `commandIndex()` і `serviceIndex()` (а також
`model.view().fields`, `.commands`, `.services`) — copyable borrowed views:

| Runtime view | Checked methods |
| --- | --- |
| `FieldIndex` | `readEncoded(id, output, workspace)`, `writeEncoded(id, input, workspace)` |
| `CommandIndex` | `executeEncoded(id, input, workspace)` |
| `ServiceIndex` | `callEncoded(id, input, output, workspace)` |

Index methods приймають integral ID та перевіряють original width до narrowing.
Від'ємний, ширший за `PackedId` або absent group/entry дає `NotFound`.
Сам lookup — bounds checks і пряме індексування; обходу назв немає.
Group і entry — дві zero-based 16-bit positions. Families мають окремі ID spaces.
TypeId описує тип і не використовується для вибору endpoint. Для checked
encoded access до одного row local table без Model є
`localFields[position].readEncoded(...)` / `writeEncoded(...)`,
`localCommands[position].executeEncoded(...)` і
`localServices[position].callEncoded(...)`; caller спершу забезпечує
`position < localTable.size()`, бо `operator[]` не перевіряє bounds.

### B. ModelView і compiled adapter

Якщо transport/module має повний `ModelView`, використовуйте
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

Простий public include — `<telemetry/Telemetry.hpp>`. Compiled API оголошено у
`<telemetry/model/Adapter.hpp>`. C++20, `lib`, `lib/boost_pfr/include` і
`lib/magic_enum` — build settings поточного reflection/type backend.

| Access | Compiled source dependency |
| --- | --- |
| Runtime index / checked local entry | Header-defined operation; `Adapter.cpp` не потрібний |
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
[Encoded.cpp](../../examples/user_guide/Encoded.cpp). Runtime-ID операції
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

Endpoint ID містить group у старших 16 bits і local entry у молодших 16 bits
([WireV3: PackedId](../WireV3.md#packedid)). У дереві group — позиція групи в
pre-order, спільна для трьох families.
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

Порожня група в дереві дозволена: `ts::group("spare")`; family, якої в
групі немає, нічого не потребує. `Model{ts::group("spare")}` усе одно має
built-in TypeRegistry, нуль runtime rows і `maxScratch() == 0`.

## Lifetime, synchronization і ABI

Local tables і `Model` noncopyable/nonmovable: erased contexts або runtime
views можуть посилатися на їхні внутрішні arrays. Names, owners, callable lvalues, slots
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
