# Питання та діагностика під час інтеграції

[User guide](README.md) · [Documentation index](../README.md) · [API cheatsheet](API-CHEATSHEET.md)

Це доповнення до [першої інтеграції](GettingStarted.md). Починайте з того,
що бачите у compiler, status або відповіді transport, і перевіряйте конкретний
контракт нижче.

## Навігація

- [Що готове використовувати, а що треба додати у своєму застосунку?](#що-готове-використовувати-а-що-треба-додати-у-своєму-застосунку)
- [Не знаходиться telemetry/Telemetry.hpp або boost/pfr.hpp](#не-знаходиться-telemetrytelemetryhpp-або-boostpfrhpp)
- [Compiler каже, що немає span / concepts / consteval](#compiler-каже-що-немає-span--concepts--consteval)
- [Linker не знаходить readFieldEncoded / callServiceEncoded](#linker-не-знаходить-readfieldencoded--callserviceencoded)
- [field() або command() видає великий template diagnostic](#field-або-command-видає-великий-template-diagnostic)
- [Великий Model упирається в compiler depth або memory limit](#великий-model-упирається-в-compiler-depth-або-memory-limit)
- [Чому не можна передати Command два аргументи?](#чому-не-можна-передати-command-два-аргументи)
- [У UI приходить double, а setter приймає float](#у-ui-приходить-double-а-setter-приймає-float)
- [readAs<T>(id) повертає порожній результат](#readastid-повертає-порожній-результат)
- [Чому однакові ID працюють у різних сімействах?](#чому-однакові-id-працюють-у-різних-сімействах)
- [WorkspaceTooSmall або BufferTooSmall](#workspacetoosmall-або-buffertoosmall)
- [Чому не серіалізується packed struct або memory dump?](#чому-не-серіалізується-packed-struct-або-memory-dump)
- [Можна додати string, vector, pointer, довільний клас?](#можна-додати-string-vector-pointer-довільний-клас)
- [field() має unit/default/limits аргументи?](#field-має-unitdefaultlimits-аргументи)
- [Callback повернув Busy / InvalidValue, а dispatch — Ok](#callback-повернув-busy--invalidvalue-а-dispatch--ok)
- [Я додав /settings.bin. Де він створився на Flash?](#я-додав-settingsbin-де-він-створився-на-flash)
- [BytesFile є writable або telemetry ValuesFile приймає WRITE?](#bytesfile-є-writable-або-telemetry-valuesfile-приймає-write)
- [Навіщо cursor u64, якщо ID u32?](#навіщо-cursor-u64-якщо-id-u32)
- [TCP/UART надіслав половину packet або кілька packets одразу](#tcpuart-надіслав-половину-packet-або-кілька-packets-одразу)
- [Чи буде Command виконана двічі після retry?](#чи-буде-command-виконана-двічі-після-retry)
- [Дані змінюються іншою task під час encode](#дані-змінюються-іншою-task-під-час-encode)
- [Що читати далі?](#що-читати-далі)

## Що готове використовувати, а що треба додати у своєму застосунку?

Готові native Field/Command/Service declarations, таблиці, каталоги, Model,
type registry, codec, bounded encoded operations, slots, generic resource
filesystem/protocol та Descriptor/Values providers. Повні host приклади
і ARM compile/link/codegen перевіряються у CI.

Ваш застосунок надає device methods, прикладні правила, storage provider для
своїх файлів, transport framing, receive/send, connection state й синхронізацію.
Прив'язка getter до telemetry не запускає ADC, task або TCP server.

Підтвердження готовності конкретної ревізії дивіться у
[CI](https://github.com/shpegun60/telemetry-sdk/actions/workflows/ci.yml),
[qualification](../StructuredTelemetryV3FinalQualification.md) та
[image evidence](../../tests/resources/evidence/README.md). Апаратні звіти
мають власні source identities. Підключений прилад із вашими callbacks треба
перевірити у вашій інтеграції.

## Не знаходиться telemetry/Telemetry.hpp або boost/pfr.hpp

| Missing include | Що додати в include paths |
| --- | --- |
| `telemetry/Telemetry.hpp` | `lib`, не `lib/telemetry` |
| `boost/pfr.hpp` | `lib/boost_pfr/include` |
| `magic_enum.hpp` | `lib/magic_enum` |
| `resource/Resource.hpp` | `lib` |

Шляхи задаються відносно вашого build project, а не документа. Для qmake
використайте відповідний `.pri`. Якщо компілюєте вручну, виконайте повну
команду з [GettingStarted](GettingStarted.md), щоб перевірити paths окремо
від інтеграції з IDE.

## Великий Model упирається в compiler depth або memory limit

Спочатку розрізніть число endpoints, root occurrences і unique exact types.
Поточні tables і catalogs скорочують повторені roots через `RegistryRootTypes`
перед побудовою Model registry; positional `RootTypes` зберігає всі endpoints.
Одна велика local table усе одно створює великий `std::tuple`. Поділіть таблиці за
модулями, повторно використовуйте exact DTO для спільного контракту і не включайте
повний application Model у кожний runtime consumer.

Порівняння і відтворювані команди є в [Scalability](../Scalability.md).
Explicit compiler budget override — окреме виміряне рішення, яке не усуває
потреби в compiler memory/time та Flash/RAM. Не збільшуйте числа без визначення
точного diagnostic. Для pinned C++20 PFR aggregate201 вже перевищує backend
capacity200, хоча normalized `maxStructMembers` дорівнює 256; тут compiler-depth
override не є виправленням. Розділіть DTO на звичайні вкладені aggregates.

## Compiler каже, що немає span / concepts / consteval

Перевірте language mode **C++20 у кожній translation unit**, включно з library
`.cpp`. Сучасний compiler із C++17 mode теж відхилить цей API. У qmake
manifests C++20 задається автоматично; власний build має задати його сам.

## Linker не знаходить readFieldEncoded / callServiceEncoded

Додайте `lib/telemetry/model/Adapter.cpp` і
`lib/telemetry/abi/StructuredAbi.cpp`. Вони мають збиратися тим самим compiler,
ABI options та `TELEMETRY_STRUCTURED_LOCAL_BYTES`, що й application modules.
ABI symbol із довгим template tag не треба замінювати власною порожньою
функцією: він може вказувати на різні settings між modules.

Для missing `resource::protocol::process` додайте
`lib/resource/protocol/Protocol.cpp`. Для live Values provider додайте
`lib/integration/telemetry/v3/detail/Values.cpp`.

## field() або command() видає великий template diagnostic

Прочитайте перший `static_assert` або constraint message. Потім звірте
signature callback:

| Перевірка | Правильна форма |
| --- | --- |
| Getter | `T()` або `const T&()`, `noexcept` |
| Setter | `WriteResult(T)` / `WriteResult(const T&)`, `noexcept` |
| Command | `CommandResult()` / `CommandResult(Request)` / `CommandResult(const Request&)`, `noexcept` |
| Service | 0/1 aggregate Request; aggregate/void Response або supported Service result wrapper, `noexcept` |
| Owner | Живий object lvalue; `OwnerSlot<T>` для явного rebind |
| Request | Один supported aggregate, а не список чисел і не raw pointer |

Setter не приймає інший canonical type лише тому, що два числа можуть
перетворюватися. Наприклад, `float getter()` має setter із `float` або
`const float&`. Перетворення значення користувача додається на рівні
`writeAs`, а не через несумісну signature binding.

Не передавайте тимчасовий owner/capturing callable у borrowed binding.
Створіть named object із достатнім lifetime або owning `DelegateSlot`.
Форми, які library відхиляє, наведені у [Native API](NativeApi.md).

## Чому не можна передати Command два аргументи?

У поточному API Command має нуль або один aggregate Request. Напишіть:

```cpp
struct ConfigureRequest {
    float voltage;
    bool enabled;
};

telemetry::CommandResult configure(const ConfigureRequest& request) noexcept;
```

Виклик отримує один `ConfigureRequest`. Function declaration вище не містить
implementation; її потрібно реалізувати у своєму застосунку. Telemetry
reflection розкладає members request для codec/descriptor автоматично.

## У UI приходить double, а setter приймає float

Для numeric Field використайте runtime `fields.writeAs(id, valueFromUi)` або
compile-time `fields.writeAs<Id>(valueFromUi)`. Значення буде перевірено перед
conversion; відмова не викликає setter.

Command/Service потребує exact Request. Складіть його у своєму UI/application
adapter і перевірте прикладні правила. Runtime Command `call(id, request)` та
Service `callAs<Response>(id, request)` / `callBorrowed<Response>(id, request)`
підтримуються, але не виконують numeric чи structural Request conversions.
Їхній contract наведено у [Flat runtime API](FlatNative.md). Якщо дані вже
прийшли у canonical wire form, використайте encoded operations.

## `readAs<T>(id)` повертає порожній результат

Перевірте окремо:

1. ID належить **Fields**, а не Commands/Services або файловому table.
2. Group/entry існують; `fields.index().find(id)` повертає endpoint.
3. `T` числовий і дозволяє checked conversion або exact structural type.
4. Getter/slot доступний, а referenced owner живий.

Порожнє `optional` саме не розрізняє ці причини. Для transport diagnostics
перевіряйте encoded `dispatch`; для application diagnostics можна спершу
перевірити index і availability slot. Не замінюйте empty read значенням 0,
якщо нуль має зміст реального вимірювання.

## Чому однакові ID працюють у різних сімействах?

Field, Command і Service мають незалежні packed ID spaces. Операція визначає
сімейство; ID визначає group/entry всередині нього. `0` може бути valid Field,
Command і Service одночасно. FileIndex також окремий — це позиція provider
у filesystem. Запис за file index не стає автоматичним записом Field.

## WorkspaceTooSmall або BufferTooSmall

| Status | Що збільшити / перевірити |
| --- | --- |
| `DispatchStatus::WorkspaceTooSmall` | Caller scratch; для однієї Model operation bound — `model.maxScratch()` |
| `DispatchStatus::BufferTooSmall` | Output span для response/value; достатньо canonical `wireSize<T>` для exact T |
| `resource::Status::BufferTooSmall` | Read/LIST buffer з урахуванням цілого token/path і protocol header |

32 B local budget — storage policy encoded endpoint, не розмір packet.
Малий local endpoint може працювати з empty Workspace; великий Request або
owning result потребує його. `model.maxScratch()` не є receive buffer size
і не розмір Response на wire.

Один Workspace використовуйте для серіалізованих synchronous operations.
Для parallel calls потрібні окремі buffers/Workspace або зовнішній lock.
Backing bytes мусять жити довше за Workspace та всі активні leases.

## Чому не серіалізується packed struct або memory dump?

Codec працює зі звичайними C++ members і передає canonical little-endian
values без native padding. У packed member адреса може не підходити для
звичайного reference; такий тип не є supported reflected DTO.

Перетворіть зовнішній packed packet у звичайний aligned aggregate, а далі
передайте його telemetry. Для готового raw binary файла, який вам не потрібно
розкладати на telemetry types, можна використати `resource::BytesFile`.

## Можна додати string, vector, pointer, довільний клас?

Wire type model обмежений bool, підтримуваними числами, enum, fixed arrays,
ordinary aggregates та void у відповідних місцях. `std::string`, `std::vector`,
raw pointer і довільний private class не стають wire payload автоматично.
Application class може бути owner methods; його response/request має бути
supported DTO. Для власного file format реалізуйте provider, який працює з
bytes. Повний список обмежень є у [Native API](NativeApi.md).

## field() має unit/default/limits аргументи?

Ні. Поточна declaration містить name + binding. Прикладну межу перевіряє
setter/Command/Service, display unit та UI defaults зберігає application/UI,
persistence реалізує application storage. Технічні `telemetry::Limits`
обмежують складність/розмір type model, а не допустиму напругу чи струм.

## Callback повернув Busy / InvalidValue, а dispatch — Ok

Це два різні рівні. `dispatch == Ok` означає, що library operation дійшла до
endpoint result. `endpointStatus` повідомляє, чи application застосувала
значення / виконала дію. Перевіряйте обидва; не відображайте «успіх», якщо
setter повернув `Busy` або `InvalidValue`.

Read-only Field може повернути `WriteResult::ReadOnly` без виклику setter.
Encoded read має dispatch і byte count, без setter status. Service payload
існує тільки при успішному Service result. Надсилайте рівно committed
`written` bytes, а не весь response array.

## Я додав /settings.bin. Де він створився на Flash?

`file("/settings.bin", provider)` реєструє path і callbacks. Воно не створює
storage або файл у FatFs. State належить provider: RAM array, Flash backend,
logger або serializer. Filesystem зберігає descriptors і маршрутизує за
index; path залишається плоскою назвою.

Для сталих read-only bytes використайте `BytesFile`. Для записуваного файла
реалізуйте `size()` і `write()`; для читання — `read()`. Обидві операції
повертають progress і cursor; partial transfer продовжується з повернутого
`next`, не обов'язково з `cursor + count`.

## BytesFile є writable або telemetry ValuesFile приймає WRITE?

Ні. `BytesFile`, DescriptorFile і ValuesFile — read-only providers.
`FileView::writable()` показує declared capability provider; `stat()` також
повертає flags. Для зміни Field використайте `writeFieldEncoded` або native
`write/writeAs`, а для власного settings file — writable provider.

## Навіщо cursor u64, якщо ID u32?

Це різні речі. FileIndex/PackedId вибирає entry. Cursor — opaque state
продовження read/write. Byte provider може використати offset; serializer
може закодувати свій стан, logger — sequence. Generic resource core не
перетворює cursor на byte offset і не підміняє повернутий provider cursor.

## TCP/UART надіслав половину packet або кілька packets одразу

Додайте application framing перед library processor. Receiver накопичує
дані у bounded buffer, виділяє complete packets і викликає processor один
раз на кожний із них. На disconnect/втраті framing state його потрібно
очистити за правилами transport. Готовий приклад і формат його framing
наведені у [transport walkthrough](TransportWalkthrough.md).

## Чи буде Command виконана двічі після retry?

Command може виконатися повторно, якщо ви знову передали packet у processor.
Callback може відмовити або сам обробляти повтори; library не створює dedupe
або exactly-once state. Request correlation і retry policy належать transport
та application. Особливо врахуйте це для commands із побічною дією.

## Дані змінюються іншою task під час encode

Серіалізуйте доступ, поверніть coherent snapshot за значенням або надайте
stable borrowed object із lock, який охоплює весь encode. Lock тільки на
час getter не захищає borrowed reference після повернення. Окремі atomic
members не гарантують coherent multi-field snapshot.
Практичні варіанти — у [ApplicationIntegration](ApplicationIntegration.md).

## Що читати далі?

| Потрібно | Документ |
| --- | --- |
| Повторити першу working integration | [GettingStarted](GettingStarted.md) |
| Methods, business logic, slots, lifetime, concurrency | [ApplicationIntegration](ApplicationIntegration.md) |
| Receive chunks → operation → reply, files | [TransportWalkthrough](TransportWalkthrough.md) |
| Точні API signatures та всі supported binding forms | [NativeApi](NativeApi.md) |
| Ресурсний wire contract та provider API | [TransportAndResources](TransportAndResources.md) |
