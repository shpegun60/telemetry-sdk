# Перша інтеграція: від свого класу до працюючого API

[User guide](README.md) · [Documentation index](../README.md) · [API cheatsheet](API-CHEATSHEET.md)

[API Map](APIOverview.md) допоможе вибрати Native, Encoded, Codec або Resource
до проходження інтеграції. Якщо потрібний лише DTO ↔ bytes, перейдіть одразу
до [Codec](CodecAndWorkspace.md) та [Codec.cpp](../../examples/user_guide/Codec.cpp).

Цей маршрут розрахований на програміста, який уже має C++ клас пристрою й хоче
підключити telemetry до прошивки або desktop-застосунку. Пройдіть його зверху
донизу один раз; потім використовуйте [шпаргалку](API-CHEATSHEET.md).

Для копіювання є повні програми:

- [NativeOnly.cpp](../../examples/user_guide/NativeOnly.cpp): один local Field,
  Command і Service; без Model/Workspace, transport чи files.
- [QuickStart.cpp](../../examples/user_guide/QuickStart.cpp): перші Field,
  Command і Service в одному файлі.
- [Device integration](../../examples/device_integration/README.md): клас,
  каталоги, простий facade, файли та приймання порцій даних у різних файлах.

## Навігація

- [Крок 1. Оберіть, як застосунок звертатиметься до даних](#крок-1-оберіть-як-застосунок-звертатиметься-до-даних)
- [Крок 2. Підключіть source та include paths](#крок-2-підключіть-source-та-include-paths)
- [Крок 3. Спочатку зберіть готову програму на хості](#крок-3-спочатку-зберіть-готову-програму-на-хості)
- [Крок 4. Напишіть application methods](#крок-4-напишіть-application-methods)
- [Крок 5. Оголосіть local tables](#крок-5-оголосіть-local-tables)
- [Крок 6. Викличте native API й перевірте результат](#крок-6-викличте-native-api-й-перевірте-результат)
- [Крок 7. Додайте каталоги, якщо потрібні global IDs](#крок-7-додайте-каталоги-якщо-потрібні-global-ids)
- [Крок 8. Виберіть доступ залежно від наявної інформації](#крок-8-виберіть-доступ-залежно-від-наявної-інформації)
- [Крок 9. Рознесіть application і metadata по своїх файлах](#крок-9-рознесіть-application-і-metadata-по-своїх-файлах)
- [Крок 10. Підключіть transport або files](#крок-10-підключіть-transport-або-files)
- [Контрольний список першої інтеграції](#контрольний-список-першої-інтеграції)

## Крок 1. Оберіть, як застосунок звертатиметься до даних

| Ваш сценарій | Що підключити | Перший виклик |
| --- | --- | --- |
| Бізнес-логіка у цій самій програмі | Local tables; каталоги, якщо потрібні global IDs | `localFields.read<0>()` |
| Власний UART/TCP протокол передає ID та bytes | Каталоги + `index()` **або** `ModelView` adapter; buffers і Workspace | `fieldCatalogs.index().readEncoded(id, output, workspace)` |
| Треба encode/decode DTO без endpoint-ів | `<telemetry/codec/Codec.hpp>`, object і bytes | `telemetry::decode(bytes, existingObject)` |
| Клієнт читає файли або ви додаєте settings/log/calibration provider | `resource::FileSystem`; за потреби generic file protocol | `fs[0].read(cursor, output)` |
| Клієнт має сам дізнатися структуру telemetry | `Model` + Descriptor provider; live доступ через Encoded/RPC або окремий Values provider | Отримати Descriptor; далі `DynamicClient::readField()` або READ `/telemetry/values.bin` |

Можна використовувати всі ці варіанти в одному застосунку. Поле лишається
прив'язаним до того самого getter/setter; спосіб звернення до нього вибираєте
у місці виклику. Для звичайного C++ виклику файли й transport не потрібні.

`Model` об'єднує каталоги й формує опис їхніх типів. Він не володіє об'єктом
пристрою, не зберігає всі live values і не запускає окрему задачу.

## Крок 2. Підключіть source та include paths

Потрібен C++20. Qt, HAL, RTOS і мережева бібліотека не є залежностями core.
Для CMake та встановленого пакета спочатку оберіть потрібні
[Telemetry components](../Modules.md#cmake): targets додають свої include paths
і залежності самі. Ручні source/include списки нижче призначені для CubeIDE
та інших build-проєктів, де ви не використовуєте ці targets.
Збережіть структуру vendor-папок і їхні ліцензії. Для повного telemetry API
додайте include paths:

```text
<telemetry-root>/lib
<telemetry-root>/lib/boost_pfr/include
<telemetry-root>/lib/magic_enum
```

Тоді public include виглядає так:

```cpp
#include <telemetry/Telemetry.hpp>
```

Стандартний build для compiled ModelView adapter та явного ABI guard включає:

```text
lib/telemetry/abi/StructuredAbi.cpp
lib/telemetry/model/Adapter.cpp
```

`Adapter.cpp` реалізує чотири compiled encoded операції. Native виклики
таблиць залишаються template API. `StructuredAbi.cpp` потрібний для ABI
boundary, коли він використовується. Не додавайте ті самі sources двічі
через власний список і `.pri` одночасно.

Local Native та direct catalog index calls не потребують цих `.cpp` самі
по собі. [NativeOnly.cpp](../../examples/user_guide/NativeOnly.cpp) лінкується
без compiled sources. Explicit `requireStructuredAbi()` — окрема залежність
від `StructuredAbi.cpp`; стандартний `.pri` вже включає його. Header-only calls
все одно мають compile-time type/reflection dependencies; це не Flash-size обіцянка.

Додаткові модулі підключаються за потреби:

| Модуль | Include / compiled source |
| --- | --- |
| Standalone DTO codec | `<telemetry/codec/Codec.hpp>`, header-only |
| Generic filesystem і готові bytes | `<resource/Resource.hpp>`, core header-only |
| LIST/STAT/READ/WRITE/SAVE packets | `<resource/protocol/Protocol.hpp>`, `lib/resource/protocol/Protocol.cpp` |
| Telemetry Descriptor/Values files | Headers у `integration/telemetry/v3`, `lib/integration/telemetry/v3/detail/Values.cpp` |
| Optional RPC mailbox | `<integration/telemetry/v3/RpcMailbox.hpp>`, `lib/integration/telemetry/v3/RpcMailbox.cpp`, разом із telemetry sources |
| Optional Bind/Exchange example | `examples/structured_protocol/Bind.cpp` та `Exchange.cpp`, разом з telemetry sources |

Для qmake достатньо manifests:

```qmake
include(path/to/telemetry/lib/telemetry/telemetry.pri)

# Лише якщо потрібні telemetry Descriptor/Values files.
CONFIG += resource_telemetry
# Окремо, лише для RPC; можна обрати незалежно від Descriptor/Values.
# CONFIG += resource_telemetry_rpc
include(path/to/telemetry/lib/integration/integration.pri)
```

Integration manifest додає залежності вибраних providers. Для generic files без
telemetry providers підключайте лише `lib/resource/resource.pri` без цих прапорців.
Descriptor/Values-only збірка з `resource_telemetry` не компілює RPC source.
Для native telemetry без files достатньо `lib/telemetry/telemetry.pri`.
Є [готовий CMake example](../../examples/device_integration/CMakeLists.txt);
це build прикладу, а не вимога переводити вашу прошивку на CMake.

## Крок 3. Спочатку зберіть готову програму на хості

Виконуйте команди з кореня `telemetry`, де лежить головний `README.md`:

```sh
python tests/docs/run.py --cxx g++ --build-dir /tmp/telemetry-doc-examples
```

На Windows передайте встановлений compiler:

```powershell
python tests/docs/run.py `
  --cxx 'C:/Qt/Tools/mingw1310_64/bin/g++.exe' `
  --build-dir 'C:/Temp/telemetry-doc-examples'
```

Шлях до compiler вище — приклад Qt MinGW kit; замініть його на свій. Runner
збирає та виконує повні програми з asserts і друкує завершення кожної. Output
directory може бути поза checkout. Наявність `.exe` сама не означає успішний
запуск: команда має завершитися без помилки, і перевірки повинні пройти.

Для ручної збірки лише QuickStart на Linux:

```sh
g++ -std=c++20 -O2 -UNDEBUG \
  -Ilib -Ilib/boost_pfr/include -Ilib/magic_enum \
  examples/user_guide/QuickStart.cpp \
  lib/telemetry/abi/StructuredAbi.cpp \
  lib/telemetry/model/Adapter.cpp \
  -o /tmp/telemetry-quickstart
/tmp/telemetry-quickstart
```

Успішний QuickStart нічого не друкує. Він перевіряє write, read, Service і
Command через assertions. Після цього міняйте application class, а build
settings та library paths залиште ті самі.

## Крок 4. Напишіть application methods

У [QuickStart](../../examples/user_guide/QuickStart.cpp) вже є `Device`,
`Config`, `Snapshot` та `device`. Вони є звичайним application кодом:

```cpp
Config read() const noexcept;
telemetry::WriteResult write(const Config& next) noexcept;
telemetry::CommandResult reset() noexcept;
Snapshot sample() const noexcept;
```

Це декларації методів усередині `Device`, а не окремі global functions.
Getter читає state, setter застосовує новий state, Command виконує дію,
Service повертає відповідь. У setter перевіряйте прикладні правила до
зміни state; повертайте `InvalidValue`, якщо нове значення неприйнятне.

| Хочете оголосити | Сигнатура callback | Що повертає native telemetry API |
| --- | --- | --- |
| Read-only Field | `T getter() noexcept` | `std::optional<T>` |
| Read/write Field | Getter + `WriteResult setter(T)` або `setter(const T&)` | Optional value / `WriteResult` |
| Великий Field без result copy | `const T& getter() noexcept` | `BorrowedValue<T>` |
| Command без запиту | `CommandResult action() noexcept` | `CommandResult` |
| Command із запитом | `CommandResult action(const Request&) noexcept` | `CommandResult` |
| Service | `Response query(const Request&) noexcept` | `ServiceResult<Response>` |
| Service, який може відмовити | `ServiceResult<Response> query(...) noexcept` | Той самий owning result |
| Service з існуючим response object | `const Response& query(...) noexcept` | `BorrowedServiceResult<Response>` |

У таблиці скорочені setter signatures теж мають бути `noexcept`. Request у
Command/Service — один aggregate struct або відсутній; Response Service —
aggregate struct або void. Field допускає bool, числа, enum, `std::array` та
підтримуваний aggregate. Якщо потрібно передати кілька аргументів команді,
складіть їх у `Request`, а не створюйте variadic callback.

Типи задає сигнатура функції. Назви додає declaration. Unit, limits, defaults,
step, persistence і member metadata не є параметрами цього API.
Докладні [типи й форми binding](NativeApi.md) можна прочитати після першого
працюючого прикладу.

## Крок 5. Оголосіть local tables

Цей фрагмент використовує `device` і methods із QuickStart:

```cpp
namespace ts = telemetry;

inline constexpr ts::FieldTable localFields{
    ts::field<&Device::read, &Device::write>("Config", device)};
inline constexpr ts::CommandTable localCommands{
    ts::command<&Device::reset>("Reset", device)};
inline constexpr ts::ServiceTable localServices{
    ts::service<&Device::sample>("Sample", device)};
```

Не пишіть цей fragment вдруге поверх тих самих declarations у QuickStart.
Він показує частину повної програми, яку треба замінити своїми methods.

Кожний елемент table отримує **позицію за порядком оголошення**. У цій table
Config має позицію 0. У більших таблицях дайте позиціям імена:

```cpp
enum class LocalField : unsigned { Config = 0 };
auto value = localFields.read<LocalField::Config>();
```

Об'єкт `device` має існувати довше за таблиці та виклики. Самі tables мають
стабільні адреси: не переносіть їх у container, який переміщує elements.
`inline constexpr` підходить для таблиць, оголошених у header; один named
`constexpr` у `.cpp` підходить для прихованої реалізації.

## Крок 6. Викличте native API й перевірте результат

```cpp
const auto changed = localFields.write<0>(Config{240.0f, true});
if (changed == ts::WriteResult::Applied) {
    const auto value = localFields.read<0>();
    if (value) {
        const float voltage = value->voltage;
        // Використати voltage у application logic.
        (void)voltage;
    }
}

const auto reply = localServices.call<0>();
if (reply.hasValue()) {
    const Snapshot& snapshot = reply.value();
    (void)snapshot;
}

const auto resetStatus = localCommands.call<0>();
```

`read()` не повертає fake zero при unavailable getter. Перевіряйте optional
або borrowed view. `ServiceResult` перевіряйте через `hasValue()` і `status()`;
він має інший інтерфейс, ніж `std::optional`. `Executed`, `Applied` та
`ServiceStatus::Ok` — різні success statuses своїх сімейств.

Якщо ці операції виконуються у вашій application task, на цьому можна
зупинитися. Немає потреби кодувати native виклик у wire bytes; синхронізація
доступу між tasks залишається відповідальністю застосунку.

## Крок 7. Додайте каталоги, якщо потрібні global IDs

```cpp
inline constexpr ts::FieldCatalogTable fields{
    ts::group("device", localFields)};
inline constexpr ts::CommandCatalogTable commands{
    ts::group("device", localCommands)};
inline constexpr ts::ServiceCatalogTable services{
    ts::group("device", localServices)};
inline constexpr ts::Model model{fields, commands, services};

inline constexpr auto ConfigId = ts::makeId<0, LocalField::Config>();
```

`0` у `makeId` — позиція групи в каталозі Fields. `LocalField::Config` —
позиція в local table. Command і Service мають окремі ID spaces: однаковий
packed integer може позначати Field 0, Command 0 і Service 0.

Перестановка груп або елементів змінює IDs. Якщо PC client ще використовує
стару схему, застосунок має узгодити новий descriptor перед операціями.
Файлові індекси — ще один незалежний простір, не telemetry IDs.

## Крок 8. Виберіть доступ залежно від наявної інформації

| У цьому місці коду відомо | Приклад | Для чого |
| --- | --- | --- |
| Local position compile-time | `localFields.read<LocalField::Config>()` | Internal firmware logic поруч із table |
| Global ID compile-time | `fields.read<ConfigId>()` | Internal logic, яка працює з усіма групами |
| Runtime ID та exact C++ type | `fields.readAs<Config>(runtimeId)` | Runtime selection без wire encoding |
| Runtime ID та numeric source value | `fields.writeAs(runtimeId, number)` | Checked numeric conversion до типу Field |
| Runtime ID та конкретна definition | `fields.visit(runtimeId, visitor)` | Умовний typed branch з `if constexpr` |
| Runtime ID та wire payload | `writeFieldEncoded(model.view(), runtimeId, bytes, workspace)` | Transport callback після складання frame |

Для Config `writeAs` приймає exact `Config`, а не окремий number. Numeric row
у таблиці стосується numeric Field. `readAs<T>` повертає owning copy навіть
для borrowed getter. Для великого payload використайте його borrowed native
read або encoded path, якщо копія не потрібна.

Коли runtime ID та exact native Request/Response відомі, Command викликайте
через `commands.call(id, request)`, а Service — через
`services.callAs<Reply>(id, request)`. Для операцій без Request аргумент
`request` не потрібний. [Flat runtime API](FlatNative.md) описує статуси,
перевірку сигнатур та borrowed result. `visit` лишається для generic обходу,
а encoded API — для wire payload. Compile-time Command/Service викликаються
через `call<Position>()` / `call<PackedId>(request)`.

## Крок 9. Рознесіть application і metadata по своїх файлах

Практичний розподіл для невеликого пристрою:

```text
Device.hpp / Device.cpp   — state та methods, прикладні перевірки
Api.hpp / Api.cpp         — stable tables/catalogs/Model і public facade
main.cpp                 — host demonstration; у прошивці це ваші tasks
```

У header facade можна віддати `ModelView`, `FileSystemView` або прості
application functions. Таблиці та довгі template types лишаються у `.cpp`.
Якщо інший модуль має викликати compile-time `fields.read<Id>()`, йому
потрібна concrete table declaration; приховування її за `ModelView` залишає
runtime API. Це вибір межі модуля, не втрата доступу до тих самих endpoints.

Повний [multi-file example](../../examples/device_integration/README.md)
показує цей розподіл і перевіряє виклики з `main.cpp`.

## Крок 10. Підключіть transport або files

Для UART/TCP та optional files далі читайте
[transport walkthrough](TransportWalkthrough.md). Він проходить шлях
receive chunk → complete frame → processor → reply і пояснює, як клієнт
знаходить index файла та читає його порціями.

Для кількох tasks і реального device state читайте
[application integration](ApplicationIntegration.md): там описані locks,
snapshots, slots і application validation. Бібліотека не створює task або
mutex за вас і не робить атомарним читання багатьох різних getters.

## Контрольний список першої інтеграції

1. Готовий QuickStart збирається вашим compiler і виконується без помилки.
2. Клас пристрою й усі потрібні DTO звичайні C++ типи; callbacks `noexcept`.
3. Getter і setter Field працюють з одним canonical `T`.
4. Objects, names, tables, catalogs і Workspace backing storage мають
   достатній lifetime; реальний caller перевіряє результати.
5. Native читання, запис і потрібні Commands/Services працюють до додавання
   transport layer.
6. Transport передає повний packet, має bounded buffers і не вважає один
   TCP/UART chunk цілим packet.
7. Після запуску на вашому MCU перевірені application callbacks, tasks,
   stack і transport саме вашої прошивки. Host examples не виконують HAL.

Розв'язання типових помилок: [питання та діагностика](Troubleshooting.md).
