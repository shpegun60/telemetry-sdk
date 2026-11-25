# API: швидка шпаргалка

[User guide](README.md) · [Documentation index](../README.md) · [API cheatsheet](API-CHEATSHEET.md)

[Fields](Fields.md) · [Commands](Commands.md) · [Services](Services.md) ·
[Tables and catalogs](TablesAndCatalogs.md) · [Model](Model.md) ·
[Codec and Workspace](CodecAndWorkspace.md) · [Resources](Resources.md) ·
[Descriptor and Values](DescriptorAndValues.md) · [Flat runtime API](FlatNative.md)

`namespace ts = telemetry;` далі лише скорочення. Фрагменти дерева груп
використовують named objects із [group_tree](../../examples/group_tree/README.md);
фрагменти розділу [Runtime ID](#runtime-id) беруть catalogs моделі;
[Native.cpp](../../examples/user_guide/Native.cpp) побудований на дереві груп. Усі callbacks — `noexcept`;
owners, names і slots живуть довше за Model.

Одна операція на пристрої, у Static C++, TS/JS і Dynamic клієнті зведена в
[таблицю API Map](APIOverview.md#одна-операція-в-чотирьох-місцях).

## Навігація

- [Створити: дерево груп](#створити-дерево-груп)
- [Шляхи й handles на пристрої](#шляхи-й-handles-на-пристрої)
- [Ті самі шляхи в клієнтах](#ті-самі-шляхи-в-клієнтах)
- [Імена типів](#імена-типів)
- [Runtime ID + bytes — transport/backend](#runtime-id--bytes--transportbackend)
- [Який обхід](#який-обхід)
- [Результати й пам'ять](#результати-й-память)
- [Standalone codec](#standalone-codec)
- [Files](#files)
- [Runtime ID: model.fields(), commands(), services()](#runtime-id)

<a id="створити"></a>

## Створити: дерево груп

```cpp
// Definition: назва + binding; її кладуть прямо в ts::group.
ts::field<&Device::readVoltage, &Device::writeVoltage>("Voltage", device);
ts::field<&Device::borrowSettings>("Settings", device); // const T& output
ts::command<&Device::reset>("Reset", device);
ts::service<&Device::query>("Query", device);

// Файл гілки: один вираз, без tables і IDs; групи вкладаються.
inline constexpr auto branch = ts::group("adc",
    ts::group("channel1", ts::field<&gain<0>, &setGain<0>>("Gain")),
    ts::field<&config, &setConfig>("Config"),
    ts::command<&reset>("Reset"),
    ts::service<&calibrate>("Calibrate"));

// Файл композиції: Model із гілок.
inline constexpr ts::Model model{adc::tlm::branch, metrology::tlm::branch};
```

Field Value: bool / number / enum / `std::array` / supported aggregate.
Command і Service Request: відсутній або один aggregate `T` / `const T&`.
Service Response: aggregate або void. Business limits перевіряє callback.
`ts::group(name, items...)` приймає підгрупи, Fields, Commands і Services у
будь-якій суміші та може монтувати іншу гілку.

<a id="group-tree-шляхи-й-handles"></a>

## Шляхи й handles на пристрої

```cpp
// Handles оголошують після Model, у файлі композиції; шлях перевіряється тут.
inline constexpr auto gain1 = ts::endpoint<model, "adc/channel1/Gain">;
inline constexpr auto rate = ts::endpoint<model, "adc/Config.rateHz">; // член: лише read()
inline constexpr auto reset = ts::endpoint<model, "adc/Reset">;

gain1.read();          // std::optional<float>
gain1.write(2.5f);     // WriteResult; лише цілий Field
rate.read();           // getter усього Field, повертає член
reset.call();          // Command / Service з native arguments
gain1.id;              // PackedId, те саме, що ts::idOf<model, "adc/channel1/Gain">()

ts::read<model, "adc/Status.raw[1]">();     // разовий доступ без handle
ts::write<model, "adc/Config">(newConfig);
ts::call<model, "adc/Calibrate">(Reference{1.25f});
model.idOf<"adc/channel1/Gain">();          // consteval PackedId
```

Шлях: `group/.../endpoint`, далі `.member` і `[i]`. Імена в дереві без `/ . [ ] \`,
унікальні в межах групи. Хибний шлях, write члена чи call на Field — build
error; handle порожній і компілюється в той самий код, що внутрішній typed
доступ за ID. Плоску `Model{fields, commands, services}` і публічний `makeId`
[прибрано](APIOverview.md#прибрано).
[Model: group tree](Model.md#group-tree-модель-із-файлів-гілок).

## Ті самі шляхи в клієнтах

```cpp
// Static C++: згенерований device над DeviceClient (ClientBindings.hpp).
device_api::client::Device<Client> device{client};
device.read<"adc/Config">(cb);                        // cb: noexcept, Result<T>
device.write<"adc/Config">(config, cb);               // або write(config) без callback
device.call<"adc/Reset">();
device.call<"adc/Calibrate">(device_api::types::adc::Reference{1.25f}, cb);
auto gain1 = device.adc.channel1.Gain;                // handle; те саме, що
auto same = device_api::client::endpoint<"adc/channel1/Gain">(device);
gain1.read(cb);                                       // gain1.id() — PackedId

// Dynamic C++: шлях розв'язується за descriptor під час роботи.
auto gain = client.endpoint("adc/channel1/Gain").handle;
gain.read(cb);                                        // cb: DynamicResult
client.endpoint("adc/Config.rateHz").handle.read(cb); // член: читається цілий Field
client.endpoint("adc/Calibrate").handle.call(requestBytes, cb);
```

```ts
// TS/JS: згенерований bindDevice(client); TS перевіряє шлях і тип значення.
const device = bindDevice(client);
const config = (await device.read('adc/Config')).value;
await device.write('adc/Config', {...config, rateHz: 2000});
await device.call('adc/Reset');
const gain1 = device.adc.channel1.Gain;               // те саме, що device.endpoint('adc/channel1/Gain')
// Dynamic JS: ті самі імена.
await dynamicClient.endpoint('adc/Config.rateHz').read();
```

Static клієнт не має шляху члена: прочитайте Field і візьміть член. Хибний
шлях — build error (C++), помилка типу (TS) або відмова
`No endpoint at path "..."` (JS, Dynamic).
[Handles згенерованих і Dynamic клієнтів](../GeneratedNamesAndHandles.md),
[клієнт без SDK](ReadingAFieldByHand.md).

## Імена типів

```cpp
namespace adc { struct Config { /* ... */ }; }   // у descriptor: adc::Config

template<>
struct telemetry::reflection::TypeReflection<Spectrum<4>> {
    static constexpr auto name = telemetry::reflection::typeName("metrology::Spectrum4");
    // або noTypeName(): прибрати ім'я
};

// telemetry_config.hpp проєкту: без імен; плоска модель дає побайтово v3.0.
#define TELEMETRY_TYPE_NAMES 0
```

Ім'я — найближчий scope і ім'я типу; унікальне в descriptor.
Hosts, старші за v3.1, відмовляють іменованому descriptor: перезберіть host,
перегенеруйте static SDK або вимкніть імена.
[Fields: імена типів](Fields.md#8-імена-типів-у-descriptor).

<a id="d-runtime-id--bytes--transportbackend"></a>

## Runtime ID + bytes — transport/backend

```cpp
ts::readFieldEncoded(model.view(), id, output, workspace);
ts::writeFieldEncoded(model.view(), id, input, workspace);
ts::executeCommandEncoded(model.view(), id, input, workspace);
ts::callServiceEncoded(model.view(), id, input, output, workspace);
```

ID приходить із пакета; ID відомого endpoint дає шлях:
`ts::idOf<model, "adc/Config">()`. Альтернатива —
`model.fieldIndex()/commandIndex()/serviceIndex()` та їхні
`readEncoded/writeEncoded/executeEncoded/callEncoded`. Перевіряйте dispatch,
endpoint status і committed byte count. Transport спочатку виділяє повний
packet із UART/TCP chunks. [Encoded.cpp](../../examples/user_guide/Encoded.cpp)
показує готовий adapter без файлів.

## Який обхід

| Потреба | API | Що отримуєте |
| --- | --- | --- |
| Runtime metadata усіх endpoints | range-for | `FieldEntry` / `CommandEntry` / `ServiceEntry` |
| Concrete definitions усіх endpoints | `forEach` | Типізовану definition |
| Одну concrete definition за runtime ID | `visit` | Typed callback, bool знайдено/не знайдено |
| Одну concrete definition compile-time | `get` | Const reference на definition |
| Файли | range-for | Lazy borrowed `FileView` |

## Результати й пам'ять

| Getter / callback / call form | Native результат |
| --- | --- |
| Field `T` | `optional<T>` |
| Field `const T&` | `BorrowedValue<T>` |
| Command | `CommandResult` |
| Service `Response` / `ServiceResult<Response>` | `ServiceResult<Response>` |
| Service `const Response&` / `BorrowedServiceResult<Response>` | `BorrowedServiceResult<Response>` |
| Service void | `ServiceResult<void>` |
| Command runtime `call` | `CommandCallStatus` |
| Owning Service runtime `callAs<Response>` | `ServiceCallResult<Response>` |
| Borrowed Service runtime `callBorrowed<Response>` | `BorrowedServiceCallResult<Response>` |
| Low-level Command runtime `callAs` | `NativeCallResult<CommandResult>` |
| Low-level Service runtime `callAs<Result>` | `NativeCallResult<Result>` |

`readAs<T>` завжди owning copy. Borrowed result позичає existing const object;
він має лишатися живим і стабільним. Callback/static owning `ServiceResult`
перевіряйте через `hasValue()/status()`; цей wrapper не має optional-style
`bool/*/->`. Flat `ServiceCallResult` підтримує explicit `bool` та один `value()`.

У low-level API `NativeCallResult::hasValue()` означає доставку exact endpoint result.
Для Service це не означає inner `hasValue()` чи application success:
перевірте `result.value().status()` та `result.value().hasValue()` перед payload.
`valueOrNull()` safe також на selection failure. Final-storage
`successFrom(factory)` не додає move/copy wrapper/response; owning result
містить payload bytes, storage обирає caller. Borrowed lifetime rules ті самі.

Encoded default local budget — 32 B; larger objects використовують caller-owned
`Workspace`. Достатній bound однієї операції — `model.maxScratch()`. Синхронізація
і LIFO lifetime leases належать caller. Library core не виділяє heap.

## Standalone codec

Fragments нижче використовують вже declared supported `Config`, `bytes` та
caller-owned `workspace`; повна програма — [Codec.cpp](../../examples/user_guide/Codec.cpp).

```cpp
Config existing{};
const auto status = ts::decode(bytes, existing); // No Workspace.
auto decoded = ts::decode<Config>(bytes, workspace); // Scoped Decoded<Config>.
if (decoded)
    use(decoded.value()); // Reference valid only while decoded lives.
```

Третя, low-level форма `decode(bytes, lease, pointer)` лишається без змін.
Existing object змінюється тільки після всіх length/overlap/representation
checks. `Decoded<T>` immobile, accessor на rvalue видалено; Workspace та bytes
живуть довше за result. Усі results/leases на одному Workspace release LIFO,
а failed result може утримувати reservation до destruction.
[Усі signatures і statuses](CodecAndWorkspace.md).

## Files

```cpp
constinit const auto fs = resource::filesystem(
    resource::file("/settings.bin", settingsProvider));
for (auto file : fs) {
    const auto path = file.path(); // Немає provider call.
    const bool readable = file.readable(); // Також без provider call.
    const auto info = file.stat(); // Explicit size() call.
}
auto result = fs[0].read(cursor, output);
auto reply = resource::protocol::process(fs.view(), packet, response);
```

Provider: exact `size() const noexcept` та `read(cursor, Output) const noexcept`
і/або `write(cursor, Input, final) noexcept`. Paths і providers позичені.
`BytesFile` позичає stable byte array/span. Index — позиція в array; cursor —
opaque u64 provider state. Filesystem не створює directories/storage автоматично.

**Views позичають:** `FileSystemView` — descriptor array, `FileView` — один
descriptor, `ModelView` — runtime catalogs/types, `BorrowedValue` — const T.
`FileSystem` володіє тільки своїм descriptor array; `Model` позичає named
catalogs і формує structural registry, а не зберігає всі live values.

Повний cookbook: [Native API](NativeApi.md) ·
[Транспорт і ресурси](TransportAndResources.md) · [Всі приклади](../../examples/user_guide/README.md).

<a id="runtime-id"></a>

## Runtime ID: model.fields(), commands(), services()

ID відомого endpoint дає `handle.id` або `ts::idOf<model, "path">()`; ID із
пакета приходить числом. Runtime-ID доступ і traversal ідуть через catalogs
моделі; compile-time доступ — шлях і handle вище:

```cpp
const auto& fields = model.fields();
const auto& commands = model.commands();
const auto& services = model.services();

localFields.read<Position::Voltage>();   // local table, без Model
localFields.write<Position::Voltage>(240.0f);
```

Local Position — індекс у local table; global ID — packed u32 group/entry
(16/16). Публічного `makeId` і typed доступу за compile-time ID
(`fields.read<id>()`) немає ([прибрано](APIOverview.md#прибрано)).

<a id="b-typed-traversal--усі-definitions"></a>

### B. Typed traversal — усі definitions

```cpp
localFields.forEach([]<std::size_t I>(const auto& field) {
    using T = typename std::remove_cvref_t<decltype(field)>::Value;
    // I і T відомі компілятору; field.read() викликайте явно.
});
fields.forEach([]<std::size_t G, std::size_t I>(
                   std::string_view group, const auto& field) {
    // Конкретна definition і group name; G і I — позиції групи та entry
    // (PackedId = G << 16 | I).
});
```

**`forEach` сам не викликає getter, Command або Service.**

<a id="c-runtime-id-typed-definition"></a>

### C. Runtime ID, typed definition

Рекомендовані runtime calls із одним outcome:

```cpp
const auto configured = commands.call(ts::idOf<model, "device/Configure">(), Configure{device.settings});
const auto reset = localCommands.call(0); // CommandCallStatus; no request.
const auto snapshot = services.callAs<Snapshot>(ts::idOf<model, "device/Snapshot">(), Query{0});
if (snapshot) {
    const Snapshot& response = snapshot.value();
    (void)response;
}
const auto settings = services.callBorrowed<Settings>(ts::idOf<model, "device/Settings">(), Query{0});
const auto ping = services.callAs<void>(ts::idOf<model, "device/Ping">());
```

`CommandCallStatus` охоплює application outcomes та runtime `NotFound` /
`SignatureMismatch`; `Accepted` не означає завершення. Service result має
один `hasValue()` / explicit `bool`, true лише при routing та application
success, і один `value()` для payload. `status()` — `ServiceCallStatus`;
nonvoid `valueOrNull()` повертає null при будь-якій відмові. Borrowed response
доступний лише як `const T&`; для `void` є success-only `value()` без payload,
але немає `valueOrNull()`. [Повні mapping та lifetime rules](FlatNative.md).

Owning pointer отримуйте через іменований result: `snapshot.valueOrNull()`.
Для `ServiceCallResult<T>` accessor дозволений лише на `&` / `const&`, а
`&&` / `const&&` видалені, включно з immediate-use temporary. Pointer живе
не довше result; `std::move(snapshot).value()` лишається доступним для move-out.
Borrowed temporary pointer допустимий, якщо backing object ще живий.
Захист не додає runtime checks і не змінює old low-level wrappers.

Typed visitors, Field access та наступні low-level calls збережено без змін:

```cpp
fields.visit(id, [](const auto& field) { /* concrete definition */ });
commands.visit(id, [](const auto& command) { /* typed branch */ });
services.visit(id, [](const auto& service) { /* typed branch */ });

// Field: відомий To або exact structural type, без visitor.
fields.readAs<double>(id);
fields.writeAs(id, 240);
localFields.readAs<double, Position::Voltage>();

// Low-level Command: routing та CommandResult окремо.
const auto configured = commands.callAs(ts::idOf<model, "device/Configure">(), Configure{device.settings});
const auto reset = localCommands.callAs(0); // No request.

// Low-level Service: exact result wrapper та exact request type.
const auto snapshot = services.callAs<ts::ServiceResult<Snapshot>>(ts::idOf<model, "device/Snapshot">(), Query{0});
const auto settings = services.callAs<ts::BorrowedServiceResult<Settings>>(ts::idOf<model, "device/Settings">(), Query{0});
const auto ping = services.callAs<ts::ServiceResult<void>>(ts::idOf<model, "device/Ping">()); // No request/payload.
```

Numeric `As` conversions checked; struct/array потребують exact C++ type.
Low-level Command `callAs` повертає `NativeCallResult<CommandResult>`; Service
`callAs<Result>` — `NativeCallResult<Result>`. Request і Service result wrapper
мають точно збігатися з declaration; conversions не виконуються. Input width
перевіряється до narrowing, table/catalog має бути lvalue.
`NativeCallStatus` — `Ok`, `NotFound`, `SignatureMismatch`. Останні дві відмови
не викликають endpoint. `Ok` доставляє application result, включно з
`Busy`/`Unavailable`. Typed `visit` із `if constexpr` лишається альтернативою
для різних shapes; byte payload обробляє encoded API. Повне runnable coverage:
[NativeCalls.cpp](../../tests/ergonomics/NativeCalls.cpp).
