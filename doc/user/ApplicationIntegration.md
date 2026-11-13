# Підключити telemetry до наявного застосунку

[User guide](README.md) · [Documentation index](../README.md) · [API cheatsheet](API-CHEATSHEET.md)

Автори: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

Цей маршрут починається там, де у прошивці вже є об'єкти з даними та методами.
Він пояснює, де оголосити таблиці, хто володіє даними, як обрати binding і як
обробити результат. Для першої збірки прочитайте
[Початок роботи](GettingStarted.md); для повного переліку сигнатур —
[Native API](NativeApi.md).

Фрагменти нижче не є окремими програмами. `Device`, `Settings`, `Snapshot`,
`Query`, `Position`, `device`, `localFields`, `localServices` та каталоги
`fields`, `commands`, `services` взяті з повного
[Native.cpp](../../examples/user_guide/Native.cpp). Скорочення
`namespace ts = telemetry;` і стандартні заголовки слід додати у свій файл.
Порівнюйте фрагмент із відповідною частиною прикладу; альтернативні declarations
не потрібно вставляти одночасно.

## Навігація

- [1. Визначте межу між модулем і доступом до нього](#1-визначте-межу-між-модулем-і-доступом-до-нього)
- [2. Побудуйте об'єкти у порядку їхнього життя](#2-побудуйте-обєкти-у-порядку-їхнього-життя)
- [3. Оберіть binding для вже наявного callback](#3-оберіть-binding-для-вже-наявного-callback)
- [4. Якщо object або callback з'являється після таблиці, використайте slot](#4-якщо-object-або-callback-зявляється-після-таблиці-використайте-slot)
- [5. Вирішіть, чи caller отримує копію, чи позичає дані](#5-вирішіть-чи-caller-отримує-копію-чи-позичає-дані)
- [6. Перевіряйте прикладні значення всередині callback](#6-перевіряйте-прикладні-значення-всередині-callback)
- [7. Виберіть спосіб синхронізації з задачами прошивки](#7-виберіть-спосіб-синхронізації-з-задачами-прошивки)
- [8. Обробіть результат перед використанням даних](#8-обробіть-результат-перед-використанням-даних)
- [9. Для runtime ID використовуйте явний тип або typed visitor](#9-для-runtime-id-використовуйте-явний-тип-або-typed-visitor)
- [10. Для обходу розрізняйте metadata й операції](#10-для-обходу-розрізняйте-metadata-й-операції)
- [11. Для enum із пропусками задайте словник до оголошення endpoint](#11-для-enum-із-пропусками-задайте-словник-до-оголошення-endpoint)
- [12. Закрийте declarations фасадом свого модуля](#12-закрийте-declarations-фасадом-свого-модуля)

## 1. Визначте межу між модулем і доступом до нього

Залиште вимірювання, конфігурацію та виконання дій у тому модулі, який уже ними
керує. Telemetry викликає його звичайні C++ методи синхронно, у контексті caller.
Таблиця сама не переходить у задачу вимірювання і не чекає завершення відкладеної
дії.

| Що треба відкрити | Що має надати ваш модуль | Оголошення |
| --- | --- | --- |
| Значення для читання | Getter `T()` або `const T&()` | Field |
| Значення для читання і запису | Getter та setter `WriteResult(T)` або `WriteResult(const T&)` | Field із setter |
| Дію без відповіді з даними | `CommandResult()` або `CommandResult(Request)` / `CommandResult(const Request&)` | Command |
| Запит із відповіддю | Метод без аргументів або з одним Request, що повертає Response чи result wrapper | Service |

Усі endpoint callbacks мають бути `noexcept`. У Command і Service Request,
якщо він є, має бути підтримуваною aggregate-структурою. Service Response із
даними також є aggregate; для дії без payload дозволений `void`.
Число, bool, enum, `std::array` і підтримувана aggregate-структура можуть бути
значенням Field. Параметри, яких кілька, об'єднайте в один Request:

```cpp
struct Query { std::uint16_t channel; };
// Метод Device у Native.cpp:
// ts::ServiceResult<Snapshot> query(const Query& request) const noexcept;
```

Інтерфейс модуля лишається придатним для прямого C++ використання. Перевірку
стану приладу та допустимості зміни робіть у цьому інтерфейсі, щоб локальний
виклик і звернення через транспорт отримували однакове рішення.

## 2. Побудуйте об'єкти у порядку їхнього життя

Практичний порядок: application object → named callbacks/slots → local tables
→ catalogs → Model → transport adapter. Знищення відбувається у зворотному
порядку, після завершення всіх звернень.

| Об'єкт | Що він зберігає | Що має залишатися доступним |
| --- | --- | --- |
| `Device` та його дані | Стан застосунку | На весь час викликів і використання borrowed outputs |
| Definition | Binding; адреса імені | Owner, callable або slot відповідного binding |
| Local table | Definitions і записи для runtime доступу | Позичені ними owners/callables/slots та імена |
| Catalog table | Групи, що посилаються на local tables | Самі named local tables |
| `Model` | Посилання на каталоги; спільний опис типів | Named catalog tables |
| `ModelView`, Index, metadata view | Позичені адреси | Таблиці та інші об'єкти, з яких створено view |

Для прошивки зі сталим набором модулів зручно розмістити ці objects у одному
integration `.cpp`, у порядку оголошення. Рядкові літерали дають іменам достатній
час життя. Якщо object створюється пізніше, оголосіть slot до таблиці та
прив'яжіть object під час запуску модуля — це показано у кроці 4.

```cpp
// Такий порядок уже використаний у Native.cpp.
inline Device device;
inline constexpr ts::FieldTable localFields{
    ts::field<&Device::readVoltage, &Device::writeVoltage>("Voltage", device)};
inline constexpr ts::FieldCatalogTable fields{
    ts::group("meter", localFields)};
// commands і services також мають бути named catalog objects.
inline constexpr ts::Model model{fields, commands, services};
```

Це скорочена альтернативна таблиця з одним Field; наступні фрагменти
використовують повні таблиці з `Native.cpp`.
Таблиці й slots не можна копіювати або переміщувати. Не збирайте view із
локальної таблиці функції та не повертайте його після виходу з цієї функції.
`Model` приймає named lvalue catalogs. Він не зберігає копію всіх поточних
значень: getter виконається тоді, коли caller попросить значення.

## 3. Оберіть binding для вже наявного callback

Якщо функція або метод відомі при компіляції, передайте їх template-аргументом.
Якщо function pointer обирається під час запуску, використайте parameter form.
Named callable з власним станом передається як lvalue та позичається.

```cpp
auto freeField = ts::field<&readFree, &writeFree>("Free");
auto memberField = ts::field<&Device::readVoltage, &Device::writeVoltage>(
    "Voltage", device);
auto functionField = ts::field("RuntimeFunction", &readFree);
auto constantField = ts::field("Constant", []() noexcept -> std::uint32_t {
    return 42;
});

auto getter = [owner = &device]() noexcept -> float { return owner->readVoltage(); };
auto closureField = ts::field("Closure", getter);
```

У цьому локальному фрагменті `getter` має жити довше за `closureField` і всі
таблиці, до яких Field потрапить. Не передавайте тимчасову lambda із capture
у binding, який її позичає. Captureless lambda може безпечно задавати function
pointer; lambda зі станом потребує named lvalue або owning slot.

Метод отримує реальний object, наприклад `device`, а не pointer на нього.
`std::ref(device)` / `std::cref(device)` також підтримуються там, де метод
допускає відповідну const-форму; wrapper не подовжує час життя object.
Для overloaded або generic callable зробіть невеликий adapter із явними
параметрами та return type, щоб сигнатура endpoint була однозначна.

Для Command та Service обираються ті самі форми binding:

```cpp
auto reset = ts::command<&Device::reset>("Reset", device);
auto query = ts::service<&Device::query>("Query", device);
```

## 4. Якщо object або callback з'являється після таблиці, використайте slot

Slot дозволяє оголосити стабільну таблицю до запуску драйвера або модуля.
Сигнатура endpoint уже відома; `bind` змінює тільки target.

| Slot | Коли обрати | Хто володіє target |
| --- | --- | --- |
| `OwnerSlot<Device>` | Метод відомий, Device буде готовий пізніше | Застосунок; slot позичає Device |
| `FunctionSlot<float() noexcept>` | Потрібно обрати free function | Slot зберігає function pointer |
| `ContextFunctionSlot<float() noexcept>` | Function pointer працює з окремим context | Context позичений |
| `DelegateRefSlot<float() noexcept>` | Потрібно обрати метод або named callable без копії | Owner/callable позичений |
| `DelegateSlot<float() noexcept, 32>` | Slot має зберігати власну closure | Slot володіє closure у своєму storage |

Оголосіть slots один раз, до таблиці, наприклад у integration `.cpp`:

```cpp
ts::OwnerSlot<Device> owner;
ts::FunctionSlot<float() noexcept> function;
ts::ContextFunctionSlot<float() noexcept> context;
ts::DelegateRefSlot<float() noexcept> reference;
ts::DelegateSlot<float() noexcept, 32> owned;

const ts::FieldTable lateFields{
    ts::field<&Device::readVoltage>("Owner", owner),
    ts::field("Function", function),
    ts::field("Context", context),
    ts::field("Reference", reference),
    ts::field("Owned", owned),
};
```

Після того як `device` готовий, виконайте прив'язки:

```cpp
owner.bind(device);
function.bind(+[]() noexcept -> float { return 12.0f; });
context.bind(+[](void* pointer) noexcept -> float {
    return pointer ? static_cast<Device*>(pointer)->readVoltage() : 0.0f;
}, &device);
reference.bind<&Device::readVoltage>(device);
owned.bind([bias = 0.5f, owner = &device]() noexcept -> float {
    return owner->readVoltage() + bias;
});

const auto value = lateFields.read<0>();
owner.reset(); // Наступне lateFields.read<0>() поверне empty optional.
```

`ContextFunctionSlot` має сигнатуру без context; bound function додатково
отримує перший аргумент `void*`. `reference.bind(getter)` може позичити named
closure із кроку 3. `owned.bind(...)` копіює lvalue або переміщує rvalue
closure; її розмір має вміститися у вибрані `Bytes` та `Align`. Heap fallback
для завеликої closure тут немає.

Owning slot володіє closure, а захоплений `&device` усе ще є позиченим object.
Усі п'ять slots мають `available()`, `bind()` і `reset()`. Порожній getter дає
empty optional/view, setter і Command — `Unavailable`, Service —
`ServiceStatus::Unavailable`. Зміна target не змінює тип endpoint або ID.

Перед reset/rebind зупиніть нові звернення і дочекайтеся завершення поточного.
Callback owning slot не повинен скидати або замінювати власний slot під час
виконання. Докладні варіанти binding наведені у
[slot README](../../lib/telemetry/slot/README.md).

## 5. Вирішіть, чи caller отримує копію, чи позичає дані

Return type callback визначає політику результату. Для невеликого значення
зручно повернути `T`. Для великого, вже готового і стабільного object можна
повернути `const T&`.

| Callback output | Результат Field | Результат Service |
| --- | --- | --- |
| `T` | `std::optional<T>` | `ServiceResult<T>` для aggregate Response |
| `const T&` | `BorrowedValue<T>` | `BorrowedServiceResult<T>` для aggregate Response |
| `ServiceResult<T>` | — | Owning response зі статусом |
| `BorrowedServiceResult<T>` | — | Borrowed response зі статусом |
| `void` / `ServiceResult<void>` | — | `ServiceResult<void>`, тільки статус |

У повному прикладі два getters читають ті самі settings різними способами:

```cpp
const auto copy = localFields.read<Position::SettingsCopy>();
const auto view = localFields.read<Position::SettingsView>();
if (copy) {
    const Settings saved = *copy; // Окрема копія.
    (void)saved;
}
if (view) {
    const Settings& current = view.value(); // Позичені device.settings.
    (void)current;
}

const auto owning = localFields.readAs<Settings, Position::SettingsView>();
```

`readAs<T>` завжди повертає owning `optional<T>`, навіть для borrowed getter.
Копія borrowed result копіює лише посилання; вона не знімає snapshot і не
подовжує час життя даних. Не повертайте reference на локальну змінну callback.
Raw pointer та mutable `T&` outputs не підтримуються як endpoint payload.
Для доступу до готового object використовуйте саме `const T&`.

Враховуйте пам'ять caller: велика owning структура займає місце у native
result і може впливати на stack. Для owning Service Response використовуйте
`ServiceResult<T>::successFrom(factory)`, коли хочете сформувати результат
без окремої попередньої копії. Для borrowed response caller має гарантувати
стабільність даних до останнього читання, а transport — до кінця encoding.

## 6. Перевіряйте прикладні значення всередині callback

Підтримуваний C++ тип ще не означає, що прилад приймає будь-яке його значення.
У `Native.cpp` setter перевіряє finite value, допустимий діапазон та стан
owner **до** зміни даних:

```cpp
ts::WriteResult Device::writeVoltage(float value) noexcept {
    if (!std::isfinite(value) || value < 0.0f || value > 300.0f)
        return ts::WriteResult::InvalidValue;
    if (busy) return ts::WriteResult::Busy;
    voltage = value;
    return ts::WriteResult::Applied;
}
```

Це альтернативна форма визначення методу, який уже оголошений у `Device`,
а не додаткове визначення поверх inline-методу прикладу.
Для структури конфігурації спочатку перевірте всі потрібні поля й їхні
поєднання; застосуйте зміну лише після успіху. Setter отримує той самий `T`
або `const T&`, який визначив getter. Валідація, синхронізація з драйвером і
правила зміни стану належать вашому модулю.

Command має розрізняти `Executed` і `Accepted`: друге означає, що owner
прийняв дію, наприклад у свою чергу, але її завершення ще не встановлено.
Service може відмовити без створення Response:

```cpp
if (request.channel >= 3)
    return ts::ServiceResult<Snapshot>::failure(ts::ServiceStatus::InvalidArgument);
```

Цей фрагмент є частиною `Device::query`. Передавати `Ok` у `failure(...)`
не можна. Окремих declaration-параметрів для units, limits, defaults або
persistence у поточному API немає.

## 7. Виберіть спосіб синхронізації з задачами прошивки

Для кожного endpoint визначте, яка задача може його викликати і яка змінює
дані. Синхронізуйте одночасні callbacks, запис даних, а також bind/reset і
знищення target. `noexcept` та `const` метод не дають такого захисту.

| Ситуація у застосунку | Практична інтеграція |
| --- | --- |
| Усі звернення й оновлення виконує одна задача | Викликайте таблицю у цій задачі; передайте туди отриманий запит |
| Getter має повернути узгоджену малу структуру іншій задачі | Owner копіює її під власним lock і повертає owning aggregate |
| Треба відкрити великий готовий блок | Публікуйте стабільний snapshot; утримуйте його на весь час borrowed читання/encoding |
| Зміна виконується тільки у задачі драйвера | Callback передає власну копію запиту через чергу застосунку і повертає належний статус |
| Модуль може зупинитися або бути замінений | Припиніть нові виклики, дочекайтеся активних, виконайте reset, потім знищуйте object |

Owning getter теж має прочитати вихідну структуру узгоджено: копіювання під
час незахищеного запису іншою задачею не є коректним snapshot. Читання кількох
Fields поспіль може бачити різні моменти вимірювання. Якщо Voltage, Current
та стан мають належати одному кадру, сформуйте їх у один `Snapshot` і відкрийте
його як Field або Service Response.

Для borrowed getter lock, який звільняється перед `return`, не захищає
подальше читання caller. Потрібен стабільний published object або зовнішня
охорона на всю операцію. `volatile` не замінює цього правила. Дані регістрів
і atomic storage переносіть у звичайні підтримувані значення у своєму модулі.

Callback із `const Request&` не повинен зберігати його адресу для відкладеної
дії. Скопіюйте потрібні дані у власний storage/чергу. Якщо native borrowed
Response посилається на Request, сам Request має жити до кінця використання
Response. Тимчасовий аргумент у `call(Query{0})` такого часу життя не надає.

## 8. Обробіть результат перед використанням даних

```cpp
const auto measured = localFields.read<Position::Voltage>();
if (measured) {
    const float voltage = *measured;
    (void)voltage;
}

const auto written = localFields.write<Position::Voltage>(240.0f);
if (written == ts::WriteResult::Applied) {
    // Модуль застосував запис.
}

const auto reply = localServices.call<1>(Query{0});
if (reply.hasValue()) {
    const Snapshot& snapshot = reply.value();
    (void)snapshot;
} else {
    const ts::ServiceStatus reason = reply.status();
    (void)reason;
}
```

Для owning `ServiceResult<T>` використовуйте `hasValue()`/`status()`;
optional-style `if (reply)`, `*reply` та `reply->member` у нього немає.
Borrowed wrappers мають `bool`, `*` та `->`. `value()` потребує успішного
результату; `valueOrNull()` дає nullable pointer без читання відсутнього payload.

| Операція | Що перевірити |
| --- | --- |
| Field `read()` / `readAs()` | Чи є значення; empty не означає нуль |
| Запис Field | `Applied`; окремо покажіть `ReadOnly`, `InvalidValue`, `Busy`, `Unavailable`, `NotFound` |
| Command | `Executed` чи `Accepted`; відмову обробіть за `CommandResult` |
| Service | `hasValue()` та `ServiceStatus`; для void response — `status()` |
| Encoded операція | Спочатку `dispatch`, потім endpoint status і `written`, якщо вони є |

`visit(...) == true` означає вибір definition, а не успіх її операції.
Так само `DispatchStatus::Ok` із endpoint `Busy` означає, що запит дійшов
до owner, який штатно відмовив. Розділення цих рівнів показане у
[Транспорті та ресурсах](TransportAndResources.md).

## 9. Для runtime ID використовуйте явний тип або typed visitor

Коли endpoint відомий у цьому місці коду, використовуйте `read<Position>()`,
`write<Position>(value)` або `call<Id>(request)`. Runtime доступ потрібен для
загального редактора, диспетчера чи обраного користувачем ID.

```cpp
const auto id = ts::makeId<0, Position::Voltage>();
const auto voltage = fields.readAs<double>(id);
const auto status = fields.writeAs(id, 240);
```

Numeric `readAs`/`writeAs` виконують перевірене перетворення, а struct/array
потребують точного C++ типу. Невдале перетворення запису не викликає setter;
setter усе одно перевіряє прикладні правила. Runtime `readAs` повертає empty
при відсутньому ID, недоступному getter або несумісному типі/перетворенні.
`writeAs` повертає `WriteResult`.

Для runtime ID і відомого exact Request використайте `commands.call(id, request)`;
форма без Request — `commands.call(id)`. Результат — один `CommandCallStatus`.
Owning Service викликається через `services.callAs<Response>(id[, request])`,
borrowed — через `services.callBorrowed<Response>(id[, request])`; один `bool`
або `hasValue()` перевіряє routing та application success. `Response = void`
підтримується в owning формі. Request і Response не перетворюються з іншої
структури: потрібен exact native тип та відповідна owning/borrowed policy.

Для окремих routing/application результатів залишаються Command `callAs(...)`
та Service `callAs<ServiceResult<Response>>(...)` / borrowed result wrapper.
Local tables мають ті самі форми з position замість packed ID. Повний contract —
[Flat runtime API](FlatNative.md).

`visit` зручний для custom handling кількох heterogeneous shapes. Ось
альтернативний native виклик лише Command з Request `Query`:

```cpp
const auto runtimeId = ts::makeId<0, 2>(); // Select у Native.cpp.
ts::CommandResult result = ts::CommandResult::NotFound;
bool invoked = false;
const bool found = commands.visit(runtimeId, [&](const auto& definition) {
    using Definition = std::remove_cvref_t<decltype(definition)>;
    if constexpr (std::is_same_v<typename Definition::Request, Query>) {
        invoked = true;
        result = definition.call(Query{0});
    }
});
// found: ID існує. invoked: цей adapter підтримує його Request.
// result: відповідь callback, якщо invoked == true.
```

Visitor компілюється для кожної definition каталогу; `if constexpr` прибирає
несумісні виклики. Для Service аналогічно перевірте `Definition::Request`
і `Definition::Response`, а потім прочитайте result wrapper вибраної гілки.
Коли транспорт уже має bytes Request/Response, використовуйте encoded API
з [Encoded.cpp](../../examples/user_guide/Encoded.cpp).

Глобальний ID містить позицію групи та позицію entry. Field, Command і Service
мають окремі простори ID. Перестановка груп або definitions змінює ці позиції;
зберігайте порядок як частину інтерфейсу, якщо зовнішній consumer зберігає IDs.

## 10. Для обходу розрізняйте metadata й операції

Range-for потрібен, щоб показати назви та можливості. `forEach` дає concrete
definition із її C++ типом, коли ви хочете зробити однакову дію з усіма Fields:

```cpp
std::size_t readable = 0;
localFields.forEach([&](const auto& definition) {
    const auto value = definition.read(); // Getter викликаємо явно.
    if (value) ++readable;
});

for (const auto& catalog : fields) {
    for (std::uint32_t i = 0; i < catalog.count; ++i) {
        const char* name = catalog.entries[i].name;
        (void)name; // Наприклад, додати до списку endpoint у застосунку.
    }
}
```

`forEach` сам не читає даних і не виконує Command/Service. Range-for також
не викликає callbacks. Range-for каталогу обходить групи; вкладений цикл —
їхні entries. Повернення `false` із visitor не зупиняє `forEach`.
Не використовуйте обхід Command як автоматичне виконання всіх дій.

## 11. Для enum із пропусками задайте словник до оголошення endpoint

У `Native.cpp` режим `Standby = 3000` лежить поза стандартним automatic scan
`-128..127`. Явна specialization додає всі потрібні коди:

```cpp
namespace telemetry::reflection {
template <> struct EnumReflection<guide::Mode> {
    inline static constexpr auto entries = enumCodes<guide::Mode::Off,
        guide::Mode::Running, guide::Mode::Standby>();
};
}
```

Це specialization повного прикладу; у своєму проєкті оголосіть її один раз
після enum, до першого endpoint/model, який використовує enum. Для власних
назв є `enumEntries(enumEntry(code, u8"Назва"), ...)`; цю форму показує
`guide::Label` у тому самому прикладі.

Словник описує коди для consumer. Він не відхиляє автоматично інші числові
значення underlying type. Якщо дозволені лише певні режими, setter або
Command/Service має перевірити їх явно. Для вкладеної структури й enum Model
сам збере доступні типи; окремий ручний список member-типів не потрібен.

## 12. Закрийте declarations фасадом свого модуля

Зовнішнім модулям прошивки часто потрібні дві-три звичайні функції, а не тип
великих heterogeneous таблиць. Тримайте object, bindings, local tables,
catalogs і Model у integration `.cpp`. У public header оголосіть потрібні
application types та функції; `.cpp` викликає точний `read/write/call` і
повертає їхній результат.

Фасад також є зручним місцем для правила «всі звернення тільки з цієї задачі»
або зовнішньої охорони на всю borrowed/encoded операцію. Якщо він віддає
`ModelView` транспорту, backing tables мають жити до завершення роботи цього
транспорту. Повернення view не переносить володіння.

У повному [device integration](../../examples/device_integration/README.md)
[Api.hpp](../../examples/device_integration/Api.hpp) оголошує звичайні функції
`app::api::readPeriod`, `writePeriod`, `readDisplay` і `writeDisplay`.
[Api.cpp](../../examples/device_integration/Api.cpp) тримає declarations та
викликає `readAs`/`writeAs` для власних відомих IDs. Consumer включає `Api.hpp`:

```cpp
std::uint32_t period = 0;
if (app::api::readPeriod(period)) {
    const auto result = app::api::writePeriod(period);
    // Обробити WriteResult у коді застосунку.
    (void)result;
}
```

`onCompletePacket` у тому самому фасаді обслуговує runtime packets, а
`StreamReceiver::feed` відділяє framing від declarations.
Для роботи з усіма binding forms і результатами вже є
[Native.cpp](../../examples/user_guide/Native.cpp); для підключення packet
adapter — [Encoded.cpp](../../examples/user_guide/Encoded.cpp).
Команди збирання та запуску готових програм наведені у
[README прикладів](../../examples/user_guide/README.md).
