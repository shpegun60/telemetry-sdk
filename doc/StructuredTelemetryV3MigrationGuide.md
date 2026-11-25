# Перенести consumer на поточний Telemetry v3

Автори: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

[Індекс](README.md) · [Посібник](user/README.md) · [Архітектура](Architecture.md) ·
[API](../lib/telemetry/README.md) · [Wire v3.0](WireV3.md)

Це чинна карта перенесення consumer. Поточний repository має один C++20
`telemetry`, exact native values, owning/borrowed outputs та binary wire
v3.0 з адитивним descriptor v3.1 ([нижче](#descriptor-v31-імена-типів-і-дерево-груп)).
Source, in-memory ABI і wire compatibility з попереднім Scalar/v2.1
API не надаються. Нове підключення починайте з
[GettingStarted](user/GettingStarted.md).

## Навігація

- [Includes, namespace і build](#includes-namespace-і-build)
- [Declarations і callback signatures](#declarations-і-callback-signatures)
- [Native access та lifetimes](#native-access-та-lifetimes)
- [Encoded transport і resources](#encoded-transport-і-resources)
- [One way: the group tree, paths and handles](#one-way-the-group-tree-paths-and-handles)
- [Descriptor v3.1: імена типів і дерево груп](#descriptor-v31-імена-типів-і-дерево-груп)
- [Перевірка перенесення](#перевірка-перенесення)
- [Походження попередніх результатів](#походження-попередніх-результатів)

## Includes, namespace і build

| Попередня назва/шлях | Чинна інтеграція |
| --- | --- |
| `lib/telemetry_structured/Structured.hpp` | `<telemetry/Telemetry.hpp>` із `lib` в include paths |
| `telemetry::structured` | `telemetry`; `namespace ts = telemetry` може бути application alias |
| `structured.pri` | `lib/telemetry/telemetry.pri` |
| `lib/resource/structured/` | `<integration/telemetry/v3/...>` |
| `resource::structured` | `resource::telemetry::v3` |
| `resource_structured`, `resource_telemetry_v3` | Явний `CONFIG += resource_telemetry` перед `lib/integration/integration.pri` |
| `telemetry_no_json`, старий JSON/v2 backend | Configuration прибрана; consumer використовує native API чи v3 codec |
| `web/telemetryStructured.js` | `web/telemetry.js` |

Потрібні C++20 і include paths для `lib`, `lib/boost_pfr/include` та
`lib/magic_enum`. Compiled telemetry sources —
`lib/telemetry/abi/StructuredAbi.cpp` і `lib/telemetry/model/Adapter.cpp`.
Resource packet layer додає `lib/resource/protocol/Protocol.cpp`; v3 Values
provider — `lib/integration/telemetry/v3/detail/Values.cpp`.

Integration qmake manifest вибирає provider sources та їхні Telemetry/Resource
dependencies; generic resource manifest не підключає telemetry назад. Optional
[protocol.pri](../examples/structured_protocol/protocol.pri) додається явно.
Повні portable/qmake/CMake кроки й готовий project наведені у
[GettingStarted](user/GettingStarted.md#крок-2-підключіть-source-та-include-paths)
та [device example](../examples/device_integration/README.md).

Не змішуйте старі та нові object files, namespace definitions або headers
в одному executable. Перезберіть усі TU з однаковими configuration macros.
Чинна structured ABI revision — 6; default local-object budget — 32 B.
Exact layout tag на compiled boundary виявляє ABI mismatch при linking.
Wire version 3.0 від цього має незалежну identity.

### Оновлення result accessor headers

Після lifetime update `5d2bb93` зробіть повну clean/rebuild consumer,
включно з dependent libraries та precompiled headers. Lvalue ref-qualifiers
`valueOrNull()` у трьох owning wrappers і `FieldReadResult::operator->()`
змінюють pointer-to-member-function types та, для out-of-line methods,
C++ mangled symbols. Layout чи незмінні firmware `.bin` не гарантують
сумісності зі старими precompiled objects.
Збережіть result у named lvalue перед pointer access; `value() &&` для
move-out і borrowed lifetime contract залишаються чинними.
Конкретні типи accessor та межі сумісності описані у
[release note](ReleaseNotes.md#повна-чиста-перебудова-consumer).

## Declarations і callback signatures

| Попередня поведінка | Чинне рішення |
| --- | --- |
| Getter/setter через універсальний Scalar container | Getter exact `T` або `const T&`; setter exact `T`/`const T&` → `WriteResult` |
| Неявні numeric conversions | Exact typed доступ або явні checked `readAs`/`writeAs` |
| Кілька positional Command arguments | Один supported aggregate Request або відсутній Request |
| Service з прикладним output | Нуль/один aggregate Request; aggregate Response або void; owning/borrowed result |
| Unit/default/limits/flags у declaration | Declaration тільки name + binding; перевірки й UI semantics у application |
| Enum metadata/default | Scoped enum з supported underlying integer; за потреби explicit `EnumReflection` dictionary |
| Late-bound callable/owner | Чинні FunctionSlot/ContextFunctionSlot/DelegateRefSlot/DelegateSlot/OwnerSlot |

Callbacks `noexcept`. Command повертає `CommandResult`; `Accepted` означає
прийняту дію, а не гарантоване завершення. Service повертає
`ServiceResult<Response>` або `BorrowedServiceResult<Response>`; status і
payload перевіряються разом. Getter `const T&` дає `BorrowedValue<T>`.

Units, display labels, defaults, допустимі enum codes та business limits
перенесіть у callback/UI/application storage, якщо consumer їх потребує.
Flag на старій declaration сам по собі не був реалізацією persistence.
Явно збережіть реальну application behavior й refusal-before-mutation.

`TypeKind::Scalar` і `ScalarCode` лишаються чинними leaf type categories
codec. Це не колишній Scalar value container. Повні supported types і
binding forms — у [Native API](user/NativeApi.md).

## Native access та lifetimes

Збережіть local table ordering та packed global IDs, якщо вони потрібні
consumer. Local Position вибирає entry; PackedId містить 16-bit group та
16-bit entry. Для зовнішніх значень перевіряйте ширину до cast: runtime-ID
методи `model.fields()`, `commands()` і `services()` роблять це самі й
перевіряють actual catalog bounds.

Compile-time `read/write/call`, typed `forEach`, runtime `visit` та erased
encoded API — різні шляхи до тих самих endpoints.
`forEach`, range-for і `get` самі не виконують callbacks.
Numeric `As` conversions checked; struct/array access потребує exact C++ T.

Owners, names, callable lvalues, slots, tables і views мають стабільні
адреси та достатній lifetime. Після переприв'язування slot external
synchronization лишається application responsibility.
Borrowed results не подовжують referent lifetime й не утворюють snapshot.
`readAs<T>` явно повертає owning copy. Lock для borrowed/encoded доступу
охоплює весь період використання const object, а не тільки getter.

[Application integration](user/ApplicationIntegration.md) і
[borrowed contract](BorrowedNativeValues.md) показують практичні lifetime,
snapshot та synchronization рішення.

## Encoded transport і resources

Віддалений consumer спочатку отримує complete packet. Encoded Model
adapter приймає endpoint ID, payload bytes, response span і caller-owned
Workspace. Core не володіє connections, Ready, requestId, retries або
descriptor agreement state.

Descriptor/Values paths обирає application; готовий приклад використовує
`/telemetry/descriptor.bin` і `/telemetry/values.bin`. Замість старих
schema/commands files consumer читає один descriptor і v3 Values.
File indexes — позиції саме поточного filesystem; знаходьте path через
LIST, якщо layout не закріплений вашим application contract.

Перевірте receive/reply sizes, `model.maxScratch()` та для Values
`requiredWorkspace()`/`maxTokenSize()`. Local-object budget не є packet
capacity. Один Values token неподільний; READ payload має вміщати
найбільший Field. Resource cursor — provider-owned state; продовжуйте
операцію з повернутого `next`, а не з самостійно вгаданого offset.

Descriptor/Values fingerprint та canonical bytes лишаються wire v3.0
після додавання borrowed outputs. In-memory ABI змінюється окремо.
Зберігайте дві перевірки: dispatch та endpoint status; віддавайте рівно
committed payload bytes. Деталі — у [wire reference](WireV3.md),
[transport walkthrough](user/TransportWalkthrough.md) та
[optional protocol example](../examples/structured_protocol/README.md).

## One way: the group tree, paths and handles

One way to write a device: the group tree, paths and handles, on the device,
the generated Static clients and the Dynamic clients. The flat Model, the
public `makeId` and the per-family generated symbols are removed. No device
used the SDK in its released form yet, so nothing is kept for compatibility;
hosts still read v3.0 descriptors from older devices.

### What was removed and what replaces it

| Removed | Use instead |
| --- | --- |
| `Model{fields, commands, services}` over `FieldCatalogTable`, `CommandCatalogTable`, `ServiceCatalogTable` | `Model{branches...}`, each branch a `ts::group(name, items...)` |
| `ts::group(name, table)` over a local table, `emptyFields`, `emptyCommands` | `ts::group(name, items...)` holding the definitions, subgroups and nothing for an absent family |
| `ts::makeId<group, entry>()`, `ts::makeId(group, entry)`, `ts::tryMakeId` | `ts::idOf<model, "path">()`, `model.idOf<"path">()`, `handle.id`; `groupOf`/`indexOf` still split a PackedId |
| Typed access by a compile-time ID: `fields.read<id>()`, `write<id>(v)`, `get<id>()`, `readAs<T, id>()`, `commands.call<id>()`, `services.call<id>()` | A handle, `ts::endpoint<model, "path">`, with `.read()`, `.write(v)`, `.call(args...)` and its `Definition`; or `ts::read/write/call<model, "path">()` |
| `model.flat()`, a catalog's `index()` without a Model | `model.view()`, `model.fieldIndex()`, `commandIndex()`, `serviceIndex()` |
| Generated `api::motor::fields::RPM`, `motor::commands::Reset`, TS `motor.fields.RPM` | `api::motor::RPM`, `api::motor::Reset`, TS `motor.RPM`; `device.read<"motor/RPM">(callback)`, `await device.read('motor/RPM')` |

Runtime IDs stay: an ID from a packet goes through `model.fields()`,
`model.commands()` and `model.services()` (`readAs<T>(id)`, `writeAs(id, v)`,
`visit(id, visitor)`, `call(id[, request])`, `callAs<R>(id[, request])`) or the
encoded functions over `model.view()`. Local tables (`FieldTable`,
`CommandTable`, `ServiceTable`) stay for native access without a Model. The
catalog engine under the tree is internal (`telemetry::detail`); the
testing-only `telemetry/testing/V30Model.hpp` names it for v3.0 checks and is
neither installed nor included by `Telemetry.hpp`.

### Before and after

Before, the flat form:

```cpp
inline constexpr ts::FieldTable motorFields{
    ts::field<&Motor::rpm>("RPM", motor),
    ts::field<&Motor::config, &Motor::setConfig>("Config", motor)};
inline constexpr ts::CommandTable motorCommands{ts::command<&Motor::reset>("Reset", motor)};
inline constexpr ts::FieldCatalogTable fields{ts::group("motor", motorFields)};
inline constexpr ts::CommandCatalogTable commands{ts::group("motor", motorCommands)};
inline constexpr ts::ServiceCatalogTable<> services{};
inline constexpr ts::Model model{fields, commands, services};

const auto rpm = fields.read<ts::makeId<0, 0>()>();
const auto reset = commands.call<ts::makeId<0, 0>()>();
const auto converted = fields.readAs<double>(runtimeId);
```

After, the group tree:

```cpp
inline constexpr auto motorBranch = ts::group("motor",
    ts::field<&Motor::rpm>("RPM", motor),
    ts::field<&Motor::config, &Motor::setConfig>("Config", motor),
    ts::command<&Motor::reset>("Reset", motor));
inline constexpr ts::Model model{motorBranch};
inline constexpr auto rpmHandle = ts::endpoint<model, "motor/RPM">;

const auto rpm = rpmHandle.read();                     // or ts::read<model, "motor/RPM">()
const auto reset = ts::call<model, "motor/Reset">();
const auto converted = model.fields().readAs<double>(runtimeId);
static_assert(rpmHandle.id == ts::idOf<model, "motor/RPM">());
```

Generated clients, before and after:

```cpp
client.read(api::motor::fields::RPM, callback);   // before
client.read(api::motor::RPM, callback);           // after: the group's constant
device.read<"motor/RPM">(callback);               // after: the path form
```

### What changes on the wire

- **IDs.** A group's position is its pre-order position in the tree, shared by
  the three families, so moving a flat model to a tree can renumber the IDs
  of the second and later groups. Paths do not change; a hand-written client
  that stores an ID stores it with the descriptor's fingerprint
  ([WireV3: PackedId](WireV3.md#packedid)).
- **Descriptor.** A tree always carries the GroupTree flag, so its descriptor
  is v3.1 even with `TELEMETRY_TYPE_NAMES 0`. Hosts built with this SDK read
  v3.0 and v3.1 alike; a host older than descriptor v3.1 needs rebuilding.
- **v3.0 input to the generator.** A v3.0 descriptor generates the same group
  form: its catalogs of one name are one root group of the three families, and
  a Field and a Command sharing a name there get a kind suffix
  (`control::Reset_Field`, `control::Reset_Command`) and no path form.

## Descriptor v3.1: імена типів і дерево груп

Descriptor v3.1 з'явився адитивно (2026-11-20); тепер пристрій описують лише
деревом груп ([вище](#one-way-the-group-tree-paths-and-handles)), тож його
descriptor завжди v3.1. Імена типів увімкнені за замовчуванням: пристрій з
enum чи struct надсилає їх і має відповідний fingerprint.

| Ситуація | Що зробити |
| --- | --- |
| Host зібраний з цим SDK (Dynamic client, UX, Inspector) | Нічого: hosts читають v3.0 і v3.1 |
| Host старший за цей SDK | Він відмовляє descriptor v3.1: перезберіть host, або вимкніть імена на пристрої |
| Static SDK, згенерований зі старого descriptor | Перегенеруйте його; per-family symbols (`fields::`) замініть символами групи ([вище](#one-way-the-group-tree-paths-and-handles)) |
| Пристрій без імен типів | `#define TELEMETRY_TYPE_NAMES 0` у `telemetry_config.hpp` пристрою на include path, або `-DTELEMETRY_TYPE_NAMES=0` для кожного translation unit; дерево лишається v3.1 |
| Два типи з однаковим ім'ям (`DuplicateName`) | `TypeReflection<T>` з `typeName("...")` для одного з них або `noTypeName()` |
| Записані сесії, прив'язані до fingerprint | Новий fingerprint відкриває нову сесію; старі записи відкриваються як раніше |

Значення `TELEMETRY_TYPE_NAMES` входить у structured ABI tag, як і
`TELEMETRY_STRUCTURED_LOCAL_BYTES`: перезберіть усі translation units і
object files SDK з однаковими перемикачами, інакше compiled SDK sources не
злінкуються. Translation unit, у якого на include path немає
`telemetry_config.hpp`, мовчки бере значення за замовчуванням.

Дерево груп — єдина форма. `Model{branches...}` дає шляхи (`adc/channel1/Gain`)
і named handles (`telemetry::endpoint<model, "adc/channel1/Gain">`) замість
позиційних IDs. Перенесена модель може отримати інші IDs, бо в дереві позицію
групи рахують у pre-order, спільно для всіх трьох families; шляхи від цього не
змінюються. [Model: group tree](user/Model.md#group-tree-модель-із-файлів-гілок),
[release note](ReleaseNotes.md#сумісність-старі-hosts-і-новий-пристрій).

## Перевірка перенесення

1. Зберіть і виконайте [QuickStart](../examples/user_guide/QuickStart.cpp)
   потрібним compiler; перезберіть весь consumer із current headers/sources.
2. Перевірте ваші getter/setter/Command/Service: exact types, status,
   refusal-before-mutation та required side effects.
3. Перевірте IDs, lifetime, borrowed data stability, synchronized reads,
   повний frame receive/reply та capacities.
4. Для custom UI перевірте збереження units/defaults/labels/persistence;
   descriptor передає форму даних, ці semantics належать consumer.
5. Виконайте відповідні [maintained checks](../tests/README.md) та власну
   application integration перевірку. Host чи ARM compile evidence не
   означає runtime перевірки вашої MCU прошивки.

## Походження попередніх результатів

[Stage 15 freeze checkpoint](StructuredTelemetryV3FreezeQualification.md)
і [Stage 20 qualification](StructuredTelemetryV3FinalQualification.md)
описують свої recorded source identities, measured evidence та CI.
Їхні результати не є автоматичним підтвердженням нового consumer чи commit.

Попередній великий stage-by-stage migration document збережено byte for
byte у зовнішньому архіві перед заміною цією current API картою.
Inventory і перевірку архіву описує
[Repository maintenance](RepositoryMaintenance.md). Receipts не
перепозначаються під новий source SHA.
