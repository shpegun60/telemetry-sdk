# API Map: Native, Encoded, Codec і Resource

[Репозиторій](../../README.md) · [Посібник](README.md) ·
[Перша інтеграція](GettingStarted.md) · [Шпаргалка](API-CHEATSHEET.md)

Оберіть маршрут за тим, що вже маєте у місці виклику. Це чотири способи
користуватися наявною бібліотекою; вони можуть співіснувати в одному застосунку.
Model/Definitions нижче — спільна основа для endpoints та їхнього опису,
а не обов'язковий runtime шар перед кожним викликом.

## Одна форма: дерево груп, шляхи й handles

Пристрій описують деревом груп. `ts::group(name, items...)` змішує підгрупи,
Fields, Commands і Services; `Model{branches...}` монтує гілки в одну модель.
Endpoint адресують шляхом `group/.../endpoint`, далі `.member` і `[i]`. Шлях
однаковий на пристрої, у Static C++ клієнті, у TS/JS і в Dynamic клієнті.
Handle — той самий шлях, розв'язаний один раз і збережений у змінній.

```cpp
namespace ts = telemetry;
// Файл гілки: один вираз, без таблиць і IDs; клас Adc нічого не знає про telemetry.
inline constexpr auto branch = ts::group("adc",
    ts::group("channel1", ts::field<&gain<0>, &setGain<0>>("Gain")),
    ts::field<&config, &setConfig>("Config"),
    ts::command<&reset>("Reset"),
    ts::service<&calibrate>("Calibrate"));

// Файл композиції: Model із гілок, потім handles; шлях перевіряється тут.
inline constexpr ts::Model model{adc::tlm::branch, metrology::tlm::branch};
inline constexpr auto gain1 = ts::endpoint<model, "adc/channel1/Gain">;
```

Повна програма з двома гілками, handles і descriptor —
[group_tree](../../examples/group_tree/README.md).

### Одна операція в чотирьох місцях

| Операція | Пристрій: код самої Model | Static C++ клієнт: згенерований `device` | TS/JS: згенерований `bindDevice(client)` | Dynamic клієнт (C++; JS так само) |
| --- | --- | --- | --- | --- |
| Прочитати Field | `ts::read<model, "adc/Config">()` | `device.read<"adc/Config">(cb)` | `await device.read('adc/Config')` | `client.endpoint("adc/Config").handle.read(cb)` |
| Записати Field | `ts::write<model, "adc/Config">(config)` | `device.write<"adc/Config">(config, cb)` | `await device.write('adc/Config', config)` | `client.endpoint("adc/Config").handle.write(bytes, cb)` |
| Прочитати член | `ts::read<model, "adc/Config.rateHz">()` | шляху члена немає: прочитайте Field, далі `r.value().rateHz` | шляху члена немає: `(await device.read('adc/Config')).value.rateHz` | `client.endpoint("adc/Config.rateHz").handle.read(cb)` |
| Викликати Command | `ts::call<model, "adc/Reset">()` | `device.call<"adc/Reset">()` | `await device.call('adc/Reset')` | `client.endpoint("adc/Reset").handle.call(cb)` |
| Викликати Service | `ts::call<model, "adc/Calibrate">(adc::Reference{1.25f})` | `device.call<"adc/Calibrate">(api::types::adc::Reference{1.25f}, cb)` | `await device.call('adc/Calibrate', {volts: 1.25})` | `client.endpoint("adc/Calibrate").handle.call(bytes, cb)` |
| Зберегти handle | `inline constexpr auto gain1 = ts::endpoint<model, "adc/channel1/Gain">;` далі `gain1.read()` | `auto gain1 = device.adc.channel1.Gain;` або `api::client::endpoint<"adc/channel1/Gain">(device)`; далі `gain1.read(cb)` | `const gain1 = device.adc.channel1.Gain;` далі `await gain1.read()` | `auto gain1 = client.endpoint("adc/channel1/Gain").handle;` далі `gain1.read(cb)` |
| ID для клієнта без SDK | `ts::idOf<model, "adc/channel1/Gain">()` або `gain1.id` | `gain1.id()` | `gain1.id` | `gain1.id()` |

- `cb` — `noexcept` callback із результатом: `telemetry::client::Result<T>` у
  Static C++, `DynamicResult` у Dynamic. Static C++ `write(value)` і `call()`
  без callback ігнорують результат. TS/JS повертає `Promise`.
- `config` — `adc::Config` на пристрої, `api::types::adc::Config` у Static C++
  (`namespace api = device_api;`) і звичайний object у TS/JS. Dynamic C++
  передає canonical bytes (`bytes`), Dynamic JS — звичайне значення.
- Член читає лише цілий Field: getter виконується для всього Field. Запису
  члена немає ніде, бо wire записує цілі Fields. Static клієнт не має шляху
  члена; Dynamic клієнт і пристрій мають.
- Dynamic JS: ті самі імена з `await`, наприклад
  `await client.endpoint('adc/Config.rateHz').read()`, `.write(value)`,
  `.call(request)`.
- Хибний шлях на пристрої та в Static C++ — build error, що показує шлях; у
  TS — помилка типу; у JS і Dynamic — відмова з текстом
  `No endpoint at path "..."`.
- ID — позиція в одному descriptor. Зберігайте його разом із fingerprint:
  [Reading a Field without the SDK](ReadingAFieldByHand.md),
  [WireV3: PackedId](../WireV3.md#packedid).

Довідники кожного місця: [Model: group tree](Model.md#group-tree-модель-із-файлів-гілок),
[handles згенерованих клієнтів і Dynamic `endpoint(path)`](../GeneratedNamesAndHandles.md),
[DeviceClient](../AsyncClient.md), [DynamicClient](../DynamicClientUsage.md).
Прибрані форми та їхні заміни — у розділі [Прибрано](#прибрано).

## Вибір за хвилину

| У вас є / потрібно | API | Необхідні об'єкти | Канонічний довідник | Повна програма |
| --- | --- | --- | --- | --- |
| C++ значення; читати Field, виконати Command або викликати Service | **Native** | Owner/binding + definitions; local tables за потреби, catalogs для global IDs | [Native API](NativeApi.md) | [NativeOnly.cpp](../../examples/user_guide/NativeOnly.cpp) |
| Runtime ID + canonical payload bytes | **Encoded** | Catalog index **або** ModelView; input/output buffers і Workspace | [Encoded access](Model.md#encoded-api-catalog-index-або-modelview) | [Encoded.cpp](../../examples/user_guide/Encoded.cpp) |
| Самостійно перетворити DTO ↔ bytes, без endpoints | **Codec** | C++ object і byte span; Workspace лише для construction у scratch | [Codec and Workspace](CodecAndWorkspace.md) | [Codec.cpp](../../examples/user_guide/Codec.cpp) |
| Названі файли, порційний READ/WRITE або LIST/STAT | **Resource** | Stable providers/paths + FileSystem; protocol за потреби | [Resource files](Resources.md) | [Resources.cpp](../../examples/user_guide/Resources.cpp) |
| Зібрати спільний structural registry і опис endpoints | **Model / Definitions** | Owner → definitions у `ts::group` → `Model{branches...}` → шляхи й handles | [Model](Model.md#group-tree-модель-із-файлів-гілок) | [QuickStart.cpp](../../examples/user_guide/QuickStart.cpp), [group_tree](../../examples/group_tree/README.md) |

Перший приклад Native не створює Model, Workspace чи files. Standalone Codec
не потребує навіть Field/Command/Service. Generic Resource працює без telemetry;
Descriptor/Values — окремі providers, які додаються за потреби.

Для віддаленого доступу оберіть клієнт над цими API:

| Сценарій | Точка входу |
| --- | --- |
| Відомі DTO та Codec, асинхронні виклики через Encoded або RPC | [DeviceClient](../AsyncClient.md); Generated SDK необов'язковий |
| Типи й endpoints отримано з Descriptor під час роботи | [DynamicClient](../DynamicClientUsage.md), [RuntimeSchema](../RuntimeSchema.md) |
| Готовий read-only GUI для невідомого пристрою | [Qt/QML Inspector](https://github.com/shpegun60/telemetry-ux/blob/main/examples/inspector/README.md) |
| Потрібен лише низькорівневий RPC packet engine | [RpcClient](../RpcClientV1.md) |

На сервері [RpcSessionPool](../RpcSessionsV1.md) керує N окремими mailbox-ами.
Application передає повні frames і час, підтверджує handoff та зіставляє replies.
Descriptor описує структуру; live значення можна читати через Encoded FieldRead.
Resource Values provider є окремим варіантом, а не обов'язковим каналом даних.

## Залежності та точки входу

```mermaid
flowchart TD
    A[Application owner / callable / slot] --> B[Definitions]
    B --> N[Native C++ calls]
    B --> T[Local tables]
    T --> N[Native C++ calls]
    T --> C[Catalog tables]
    C --> N
    C --> I[Borrowed catalog indexes]
    I --> E[Encoded calls]
    C --> M[Model + structural TypeRegistry]
    M --> V[ModelView + compiled adapter]
    V --> E
    M --> D[DescriptorFile / ValuesFile]
    P[Custom file providers] --> F[Flat FileSystem]
    D --> F
    F --> R[Generic Resource protocol]
    U[Application framing: complete packet] --> E
    U --> R
    X[Ordinary supported C++ DTO] --> K[Standalone Codec]
    K --> Y[Canonical bytes]
```

Catalogs об'єднують local tables. Вони дають global PackedId та `index()`.
Local `FieldTable`, `CommandTable` і `ServiceTable` не мають `index()`;
їхні erased rows доступні через `operator[]`, а native definition — через
`get`/`visit`. Model збирає один structural TypeRegistry і runtime views.
Вона не є owner даних і не замінює native API таблиць/каталогів.

Нижче короткі fragments викликів. Змінні `fields`, `commands`, `services`
означають відповідні local tables у Native розділі та catalog tables в Encoded
розділі. Повні declarations і `main()` є у linked runnable programs.

## Native: C++ значення без byte buffers

```cpp
const auto value = fields.read<0>();             // Compile-time local position.
const auto write = fields.write<0>(newValue);    // Exact native value.
const auto action = commands.call<0>();
const auto reply = services.call<0>(request);

const auto converted = fields.readAs<std::uint32_t>(position);
const auto applied = fields.writeAs(position, newValue);
const auto called = commands.call(position, request);
const auto response = services.callAs<Reply>(position, request);
```

Зіставте request із реальною сигнатурою callback: Command/Service має нуль
або один aggregate Request. Runtime Command використовує `call(position)`
або `call(position, request)`. Runtime Service — `callAs<Reply>(position)`
або `callAs<Reply>(position, request)`; для borrowed response є `callBorrowed`.

- Static positions/IDs зберігають exact definition та native result type.
- Runtime Field `readAs`/`writeAs` підтримують визначені **checked conversions**;
  struct/array access вимагає exact type. Для причини read відмови є
  `readAsResult<T>`.
- Runtime Command/Service вимагає **exact request/response signature**.
  Однаковий layout двох DTO не робить їх тим самим C++ типом.
- Borrowed `const T&` Field читається без копії через `read()` або
  `readBorrowed<T>`; Service має `callBorrowed<T>`. `readAs<T>` повертає owning copy.
- `get<Position>()`, typed `forEach` та runtime `visit` працюють із definitions;
  traversal сам не запускає getter/callback. Range-for повертає erased metadata.

Результат перевіряйте перед доступом до payload. Зберігайте owning result
в іменованій змінній, поки користуєтеся його reference/pointer. Borrowed result
не подовжує lifetime application object і не гарантує snapshot чи синхронізацію.

**Деталі:** [Fields](Fields.md), [Commands](Commands.md), [Services](Services.md),
[Tables/Catalogs та iteration](TablesAndCatalogs.md),
[результати Native](NativeApi.md#14-status-результат-власника-та-dispatch--різні-рівні),
[flat runtime outcomes](FlatNative.md).

## Encoded: ID + payload

Два чинні входи використовують ті самі checked endpoint boundaries.

**ModelView, для Model із дерева груп і compiled forwarding:**

```cpp
auto read = telemetry::readFieldEncoded(view, fieldId, output, workspace);
auto write = telemetry::writeFieldEncoded(view, fieldId, input, workspace);
auto execute = telemetry::executeCommandEncoded(view, commandId, input, workspace);
auto call = telemetry::callServiceEncoded(view, serviceId, input, output, workspace);
```

`view` зазвичай отримують через `model.view()`. ModelView позичає indexes і
metadata; transport може отримувати цю маленьку view через application facade.
ID приходить із пакета; код пристрою, якому потрібен ID відомого endpoint,
бере його зі шляху: `ts::idOf<model, "adc/Config">()` або `handle.id`.

**Runtime indexes моделі** (ті самі перевірки без compiled adapter):

```cpp
auto fieldIndex = model.fieldIndex();       // або model.view().fields
auto commandIndex = model.commandIndex();
auto serviceIndex = model.serviceIndex();

auto read = fieldIndex.readEncoded(fieldId, output, workspace);
auto write = fieldIndex.writeEncoded(fieldId, input, workspace);
auto execute = commandIndex.executeEncoded(commandId, input, workspace);
auto call = serviceIndex.callEncoded(serviceId, input, output, workspace);
```
Runtime PackedId — u32: high 16 bits група (catalog), low 16 bits entry
([WireV3: PackedId](../WireV3.md#packedid)). Адресні простори
Field/Command/Service окремі. Lookup перевіряє межі й прямо індексує масиви.

Спочатку перевірте `dispatch`, потім application outcome там, де він існує:

| Result | Фактичні поля |
| --- | --- |
| `EncodedReadResult` | `dispatch`, `written` |
| `EncodedWriteResult` | `dispatch`, `endpointStatus` типу `WriteResult` |
| `EncodedCommandResult` | `dispatch`, `endpointStatus` типу `CommandResult` |
| `EncodedCallResult` | `dispatch`, `endpointStatus` типу `ServiceStatus`, `written` |

Input має exact wire length; output є capacity і може бути більшим за payload.
`dispatch == Ok` не означає application success. Wire decode не виконує
прикладну перевірку допустимості напруги, періоду чи режиму — це callback.
Для local-storage endpoint Workspace не використовується; інтерфейс усе одно
приймає його. Scratch sizing, status precedence й overlap описані в
[канонічному Encoded довіднику](Model.md#encoded-api-catalog-index-або-modelview).

Encoded не встановлює UART/TCP framing, reply envelope, retries чи delivery.
Передайте вже складений payload; використовуйте
[transport walkthrough](TransportWalkthrough.md) та
[повний receiver example](../../examples/device_integration/README.md).

## Codec: самостійні DTO ↔ bytes

```cpp
auto encoded = telemetry::encode(object, bytes);
auto populated = telemetry::decode(bytes, existingObject);
auto decoded = telemetry::decode<Payload>(bytes, workspace);
auto constructed = telemetry::decode(bytes, lease, pointer);
```

Перші два повертають `CodecStatus`: ви вже маєте object. Третій повертає
scope-bound `Decoded<Payload>`: він конструює object у caller Workspace і
тримає його Lease живим. Четвертий повертає `CodecStatus`; `lease` — вже
зарезервований `Workspace::Lease<Payload>`, а `pointer` — output pointer.
Повні declarations усіх трьох decode форм є в [Codec.cpp](../../examples/user_guide/Codec.cpp).

`wireSize<T>` — довжина canonical payload, не `sizeof(T)`. Members/array
elements йдуть у declaration/element order, integers та float bits — little-endian,
native padding не передається. Reflection виконується при компіляції; можна
кодувати nested aggregates і `std::array` підтримуваних типів без Model чи registry.

Standalone encode/decode вимагають exact byte-span length. Decode перевіряє
representation (зокрема bool), а application перевіряє зміст. Existing-object
decode завершує validation до mutation. References із `Decoded<T>` живуть
лише разом із named result; leases звільняються LIFO.

**Деталі:** [Codec and Workspace](CodecAndWorkspace.md),
[supported wire types та enums](Model.md).

## Resource: файли з власними paths/providers

```cpp
auto fs = resource::filesystem(
    resource::file("/settings.bin", settings),
    resource::file("/log.bin", logger));

for (auto file : fs) {
    const auto path = file.path();
    const auto stat = file.stat(); // Explicit: iteration itself does not call size().
}
auto chunk = fs[0].read(cursor, output);
auto response = resource::protocol::process(fs.view(), packet, reply);
```

`settings` та `logger` — stable provider lvalues з `size()` і щонайменше одним
із `read()`/`write()`. `file()` створює FileEntry; public operations надає FileView.
FileSystem зберігає flat entries, а не application provider objects.
Providers і paths мають жити довше за використання entries/views.

Cursor — **opaque u64 provider state**. Продовжуйте з `next`, поверненого
provider; не додавайте кількість bytes самі, якщо контракт provider цього
не визначає. Byte offset є конкретною політикою BytesFile/інших providers.
Generic packet processor отримує один повний packet; частковий transfer файла
й частковий UART/TCP frame — різні рівні.

Telemetry integration додає [DescriptorFile / ValuesFile](DescriptorAndValues.md)
до того самого filesystem. Для generic settings/logger файлів Model не потрібна.
Fingerprint описує telemetry descriptor identity, не checksum live values
або ваших довільних файлів. `WRITE final` і persistence визначає provider.

**Деталі:** [Resource files](Resources.md), [packet envelopes](../../lib/resource/protocol/README.md),
[client builders/parsers](TransportAndResources.md#клієнт-зібрати-request-і-перевірити-response),
[COBS integration](COBSIntegration.md).

## Model / Definitions: спільна основа

```cpp
inline constexpr telemetry::Model model{adc::tlm::branch, metrology::tlm::branch};
auto view = model.view();
auto types = model.types();
constexpr auto typeId = model.typeId<adc::Config>();
constexpr auto gainId = model.idOf<"adc/channel1/Gain">();
```

Definition прив'язує назву до getter/setter/callable; `ts::group` збирає
definitions і підгрупи; Model розгортає дерево під час компіляції, призначає
позиції й об'єднує типи в один registry. Всі ці borrowed зв'язки мають
зберігати lifetime своїх sources.

TypeRegistry описує structural shape, а не units, limits, defaults чи persistence.
`RootTypes` зберігає positional roots із повторами; deduplicated registry roots
не змінюють кількість Fields або Values tokens. Для application API
користуйтеся шляхами й handles, `model.types()` і `model.typeId<T>()`.

Endpoint адресують шляхом, а не позицією:
`telemetry::endpoint<model, "adc/channel1/Gain">` оголошує handle
(`gain1.read()`, `gain1.write(2.5f)`), розв'язаний під час компіляції в
PackedId. Кожен enum і struct у descriptor має ім'я типу з коду
(`adc::Config`); `TELEMETRY_TYPE_NAMES 0` прибирає імена. Плоску
`Model{fields, commands, services}` із catalog tables [прибрано](#прибрано).

**Деталі:** [Model](Model.md), [Tables/Catalogs](TablesAndCatalogs.md),
[bindings/slots](NativeApi.md), [імена типів](Fields.md#8-імена-типів-у-descriptor),
[architecture](../Architecture.md).

## Headers і compiled sources

Вказуйте include roots `lib`, `lib/boost_pfr/include`, `lib/magic_enum` для
telemetry C++20 API. Generic Resource потребує лише `lib`.

| Сценарій | Public include | Compiled sources |
| --- | --- | --- |
| Native declarations/calls | `<telemetry/Telemetry.hpp>`; окремі `<telemetry/field/FieldTable.hpp>`, `<telemetry/command/CommandTable.hpp>`, `<telemetry/service/ServiceTable.hpp>` | Самі template calls header-only; explicit ABI guard потребує `lib/telemetry/abi/StructuredAbi.cpp` |
| Encoded через runtime index моделі | `<telemetry/Telemetry.hpp>` (`model.fieldIndex()`, ...) | Header-only index calls; compiled adapter не потрібний |
| Encoded через ModelView | `<telemetry/model/Adapter.hpp>`; Model declaration у `<telemetry/model/Model.hpp>` | `lib/telemetry/model/Adapter.cpp`; ABI guard — окрема залежність |
| Standalone Codec | `<telemetry/codec/Codec.hpp>` | Header-only |
| Generic files/providers | `<resource/Resource.hpp>` | Header-only |
| Resource packets | `<resource/protocol/Protocol.hpp>`; client `<resource/protocol/Packets.hpp>` | `lib/resource/protocol/Protocol.cpp` для `process()`; client helpers header-only |
| Telemetry files | `<integration/telemetry/v3/DescriptorFile.hpp>`, `<integration/telemetry/v3/ValuesFile.hpp>` | `lib/integration/telemetry/v3/detail/Values.cpp` для Values; Descriptor header-only |

Header-only тут описує compiled-source requirement. Native headers можуть
транзитивно включати codec/type/reflection headers; це не обіцянка автоматичного
зменшення Flash. Стандартний `telemetry.pri` включає обидва telemetry `.cpp`;
не додавайте їх двічі. ABI configuration має збігатися між translation units;
точний [ABI boundary contract](Model.md) не є перевіркою fingerprint у пакеті.

## API → довідник → приклад → перевірка

| API | Канонічний документ | Runnable example | Validation |
| --- | --- | --- | --- |
| Native | [NativeApi.md](NativeApi.md) | [NativeOnly.cpp](../../examples/user_guide/NativeOnly.cpp), [Native.cpp](../../examples/user_guide/Native.cpp) | `tests/docs/run.py`: execute обидва; NativeOnly має counted checks |
| Encoded | [Model.md](Model.md#encoded-api-catalog-index-або-modelview) | [Encoded.cpp](../../examples/user_guide/Encoded.cpp) | Той самий runner: callbacks, exact lengths, refusals, large/borrowed responses |
| Codec | [CodecAndWorkspace.md](CodecAndWorkspace.md) | [Codec.cpp](../../examples/user_guide/Codec.cpp) | Той самий runner: golden bytes, три decode forms, validation, lifetime/LIFO |
| Resource | [Resources.md](Resources.md) | [Resources.cpp](../../examples/user_guide/Resources.cpp), [ResourceClient.cpp](../../examples/user_guide/ResourceClient.cpp) | Той самий runner: provider operations, packet processing, client parsing |
| Model / Definitions | [Model.md](Model.md) | [QuickStart.cpp](../../examples/user_guide/QuickStart.cpp), [group_tree](../../examples/group_tree/main.cpp) | Той самий runner: declarations, handles і шляхи + exact README/source parity |
| Клієнт без SDK | [WireV3.md](../WireV3.md), [Reading a Field without the SDK](ReadingAFieldByHand.md) | [field_by_hand](../../tests/field_by_hand/main.cpp) | `tests/field_by_hand/run.py`: кожен hex-блок walkthrough дорівнює байтам справжнього server |

Команда з кореня репозиторію:

```sh
python tests/docs/run.py --cxx g++ --build-dir /tmp/telemetry-doc-examples
```

[Runner documentation](../../tests/docs/README.md) містить фактичні перевірені
toolchain modes, результати та межі доказу. Host execution, sanitizer execution
і ARM compile/link — різні перевірки; ARM docs runner не запускає плату.

## Прибрано

Одна форма — дерево груп, шляхи й handles. Ці форми прибрано з public API
(ROADMAP API-1); before/after для наявного коду — у
[migration guide](../StructuredTelemetryV3MigrationGuide.md#one-way-the-group-tree-paths-and-handles).

| Прибрано | Замість цього |
| --- | --- |
| Плоска `Model{fields, commands, services}`: `FieldCatalogTable`, `CommandCatalogTable`, `ServiceCatalogTable`, `ts::group(name, table)`, `emptyFields`, `emptyCommands` | `Model{branches...}` із `ts::group(name, items...)` |
| Публічний `ts::makeId<group, entry>()`, `tryMakeId` і typed доступ за compile-time ID: `fields.read<id>()`, `write<id>()`, `get<id>()`, `commands.call<id>()`, `services.call<id>()` | `ts::endpoint<model, "path">` і `ts::read/write/call<model, "path">()`; ID — `ts::idOf<model, "path">()` або `handle.id` |
| Catalog indexes `fields.index()` без Model, `model.flat()` | `model.view()`, `model.fieldIndex()`, compiled adapter |
| Згенеровані символи за сімействами: `api::device::fields::Config`, `device.motor.fields.Config` | `api::device::Config`, `device.read<"motor/Config">(cb)`, `api::client::endpoint<"motor/Config">(device)`, `device.motor.Config` |

Runtime-ID доступ лишається: `model.fields().readAs<T>(id)`, `visit(id, ...)`,
`model.commands().call(id)`, `model.services().callAs<R>(id, request)`. Catalog
engine під деревом внутрішній (`telemetry::detail`,
[Tables and catalogs](TablesAndCatalogs.md)); PackedId лишається у wire та в descriptor:
[WireV3: PackedId](../WireV3.md#packedid). Hosts і далі читають descriptors v3.0.
