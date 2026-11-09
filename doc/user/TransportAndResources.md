# Файли ресурсів, пакети та encoded доступ

[Посібник](README.md) · [API Overview](APIOverview.md) · [Resource files](Resources.md) ·
[Descriptor and Values](DescriptorAndValues.md) · [Wire v3.0](../WireV3.md) ·
[Transport walkthrough](TransportWalkthrough.md)

Готовий [COBS integration](COBSIntegration.md) використовує upstream
`cobs::Endpoint` зі sibling repository; повний host project та його перевірки
описані у [README прикладу](../../examples/cobs_integration/README.md).

Окремий [Resource API guide](Resources.md) пояснює provider/FileView
signatures та transfer results; [Descriptor/Values](DescriptorAndValues.md)
пояснює Model providers і whole-token reads. Тут зібрано повний шлях
від files і encoded operations до packet/transport integration.

Автори: Ruslan Kovtun (shpegun60), codexAi. MIT.

Нативні оголошення Field/Command/Service описані в
[NativeApi.md](NativeApi.md). Ця сторінка показує, як передати їхні значення
байтами, додати власні файли та підключити обробку до транспорту.
Усі наведені обробники синхронні. Вони не створюють UART, TCP-з'єднання,
задачу, чергу або сховище.

## Навігація

- [Який інтерфейс потрібен](#який-інтерфейс-потрібен)
- [Додати файли та встановити шляхи](#додати-файли-та-встановити-шляхи)
- [Перебрати файли та отримати FileView](#перебрати-файли-та-отримати-fileview)
- [Готові байти: BytesFile та JSON](#готові-байти-bytesfile-та-json)
- [Власний provider із читанням та записом](#власний-provider-із-читанням-та-записом)
- [LIST, STAT, READ та WRITE через generic protocol](#list-stat-read-та-write-через-generic-protocol)
- [UART/TCP: спочатку повний packet](#uarttcp-спочатку-повний-packet)
- [Прямі encoded операції Model без файлового шару](#прямі-encoded-операції-model-без-файлового-шару)
- [Encoded без Model: catalog indexes](#encoded-без-model-catalog-indexes)
- [Wire значення та перевірка результатів](#wire-значення-та-перевірка-результатів)
- [Workspace, локальний бюджет та borrowed response](#workspace-локальний-бюджет-та-borrowed-response)
- [DescriptorFile та ValuesFile у тій самій таблиці](#descriptorfile-та-valuesfile-у-тій-самій-таблиці)
- [RpcMailbox через Resource WRITE/READ](#rpcmailbox-через-resource-writeread)
- [Коли потрібен приклад Bind/Exchange](#коли-потрібен-приклад-bindexchange)
- [Збірка та що перевіряють приклади](#збірка-та-що-перевіряють-приклади)

## Який інтерфейс потрібен

| Потреба | Інтерфейс | Що вибирає адресат |
| --- | --- | --- |
| Прочитати/змінити C++ значення всередині програми | Нативні таблиці з [Native API](NativeApi.md) | Позиція або packed ID та точний тип |
| Передати одне значення/виклик як байти без Model | Family catalog `index().readEncoded/writeEncoded/executeEncoded/callEncoded` | Операція, packed ID, payload |
| Передати те саме через compiled boundary | `readFieldEncoded`, `writeFieldEncoded`, `executeCommandEncoded`, `callServiceEncoded` з `ModelView` | Операція, u32 packed ID, payload |
| Надати набір файлів із власними шляхами | `resource::filesystem(file(path, provider), ...)` | u32 індекс файла |
| Передати файлові операції як пакети | `resource::protocol::process(view, request, response)` | LIST/STAT/READ/WRITE |
| Прочитати опис Model та всі його Fields | `resource::telemetry::v3::DescriptorFile` і `ValuesFile` | Файли, додані в звичайну таблицю ресурсів |
| Зібрати RPC request, виконати один encoded dispatch і прочитати result | Optional `resource::telemetry::v3::RpcMailbox` | Resource file index, transaction ID, operation та endpoint ID |
| Узгодити Model перед керівними викликами | Окремий приклад Bind/Exchange | Контекст підключення, який належить транспорту |

Індекс файла і telemetry packed ID — різні адресні простори. Файловий
`WRITE` викликає `provider.write()`. `ValuesFile` і `DescriptorFile` мають
тільки читання; зміну Field, Command та Service маршрутизують до encoded
endpoint indexes, compiled ModelView adapter або optional RpcMailbox.

## Додати файли та встановити шляхи

Повний приклад із локальним доступом, JSON-текстом, частковим записом і
пакетами — [Resources.cpp](../../examples/user_guide/Resources.cpp).
Він використовує тільки загальну resource бібліотеку:

```cpp
#include <resource/Resource.hpp>

inline constexpr std::array versionBytes{
    std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
inline constexpr resource::BytesFile version{versionBytes};

inline constinit const auto files = resource::filesystem(
    resource::file("/device/version.bin", version));
```

`file("/your/path", provider)` одночасно визначає шлях і прив'язує живий
об'єкт. Наступний `file(...)` додає наступний індекс. Окремої реєстрації
шляхів, `init()` для таблиці чи створення каталогів немає. Ім'я файла не
вибирає накопичувач: RAM, Flash, SD-картку чи генерацію байтів реалізує
provider.

Шлях є плоскою міткою. `/logs/state.json` можна показати клієнту як папки,
але ядро не обходить дерево. Шлях починається з `/`, не закінчується `/`,
не має порожніх компонентів, `.`/`..`, зворотного слеша або ASCII control
байтів. У таблиці не може бути двох однакових шляхів. Невалідна constexpr
або constinit декларація не компілюється; невірна runtime декларація
порушує контракт і завершується `abort()`.

Стабільний рядковий літерал — простий спосіб задати шлях. `std::string_view`
і lvalue `std::string` також позичені: їхній текст повинен залишитися за
тією самою адресою. Зберігайте текст шляхів незмінним: construction
перевіряє його один раз. `file()` відхиляє тимчасові provider та owning рядки.
Створення `string_view` вручну не подовжує життя його джерела.

`FileIndex` — u32 позиція, починаючи з нуля. Зміна порядку декларацій
змінює індекси. Клієнт може отримати поточний порядок через LIST; core не
робить lookup за шляхом. `path(index)` повертає порожній view для невірного
індексу; `stat/read/write` повертають `InvalidFile`. Статус `FileStat`
відрізняє порожній файл від відсутнього.

`FileSystem<N>` володіє масивом дескрипторів. Provider, його дані і тексти
шляхів залишаються у застосунку. `file()` повертає opaque immutable
`FileEntry`: його path, provider context та operation table приватні.
Публічний доступ до metadata й операцій дають FileSystem та FileView.
`files.view()` дає `FileSystemView`, який
позичає саме цю таблицю. Не знищуйте та не переміщуйте таблицю під час
використання view. Одержання view з тимчасової таблиці відхилене. Копія view
не копіює provider і не подовжує його життя.

## Перебрати файли та отримати FileView

```cpp
for (const auto file : files) {
    const auto index = file.index();
    const auto path = file.path();
    const bool canRead = file.readable();
    const bool canWrite = file.writable();
    // Показати index/path. Provider ще не викликався.
    (void)index;
    (void)path;
    (void)canRead;
    (void)canWrite;
}

const auto view = files.view();
for (const auto file : view) {
    const auto stat = file.stat(); // Тепер явно питаємо поточний size/flags.
    if (stat.status == resource::Status::Ok) {
        // Використати stat.size та stat.flags.
    }
}

const auto selected = view[1];
if (selected) {
    // selected.read(cursor, output);
    // selected.write(cursor, input, final);
}
```

`size()` таблиці/view — кількість файлів, `empty()` — чи немає записів.
Це не сумарний обсяг даних; байтовий розмір окремого файла повертає
`file.stat().size`. `FileView` має `index()`, `path()`, `valid()`, explicit
bool, constexpr `readable()`/`writable()` та `stat/read/write`.
Capability methods перевіряють тільки наявність callback, без виклику
provider. Вони не гарантують готовність storage чи успіх операції.
Отримання facade за `view[index]` перевіряє
bounds: відсутній запис дає `valid()==false`, порожній path та
false в обох capabilities; операції дають `InvalidFile`, а `index()`
зберігає запитаний індекс.

`begin/end` дають forward iterators, які повертають невеликий FileView
за значенням. Перебір, `index()`, `path()` і `valid()` читають тільки
дескриптор; те саме стосується `readable()`/`writable()`.
Provider `size/read/write` викликаються лише через явні
операції. Тому можна будувати список імен без читання SD-картки чи
getter telemetry. [Resources.cpp](../../examples/user_guide/Resources.cpp)
перевіряє це лічильником звернень до `size()`.

FileView/iterator позичають descriptor table напряму, а не маленький
об'єкт FileSystemView. `for (auto file : files.view())` допустимий:
тимчасовий view не володіє таблицею. FileSystemView також є
`std::ranges::borrowed_range`. Це дозволяє зберігати iterator, одержаний
з тимчасового view, тільки поки **underlying FileSystem, paths та providers**
живі. Тимчасовий owning FileSystem не перетворюється на довгоживучу
таблицю; його `view/begin/end/operator[]` для rvalue відхилені.
Не розіменовуйте end і не збільшуйте його далі.

У тестовому прикладі runtime facade
[DeviceResources.hpp](../../tests/resources/fixtures/resources/DeviceResources.hpp) надає
`device::resources::files()` із типом `resource::FileSystemView`.
Transport/UI може перебрати цей view без telemetry templates:

```cpp
for (const auto file : device::resources::files()) {
    const auto stat = file.stat();
    // file.index(), file.path(), stat.status, stat.size, stat.flags
}
```

Додайте include цього application header для такого коду. Це функція demo,
а не глобальна registry бібліотеки. Вона позичає статичну таблицю
Descriptor/Values providers із DeviceResources.cpp.
Імена індексів demo містяться в `device::resources::fileIds::Descriptor`
та `device::resources::fileIds::Values`.

## Готові байти: BytesFile та JSON

`BytesFile` — read-only provider над byte span або живим масивом
`std::byte`. Він не копіює байти і не виділяє пам'ять. Конструктор приймає
стабільні C-масиви/std::array та явні byte spans; owning тимчасові масиви,
conversion wrappers і приховані braced джерела відхиляються. Явний span
має власний контракт життя backing storage.

```cpp
inline constexpr char text[] = "{\"ready\":true}";
inline const resource::BytesFile json{
    std::as_bytes(std::span{text}).first(sizeof(text) - 1)};
inline constinit const auto textFiles = resource::filesystem(
    resource::file("/status.json", json));
```

Тут саме застосунок підготував JSON і виключив кінцевий NUL. Розширення
`.json` не запускає серіалізацію telemetry і не встановлює HTTP Content-Type.
За потреби це робить HTTP адаптер застосунку. Для змінного JSON provider
генерує або бере стабільний buffer і визначає його актуальний `size()`.
Під час зміни buffer застосунок забезпечує узгодженість читання.

У `BytesFile` курсор — байтовий offset. На EOF читання дає
`Ok`, `written=0`, `eof=true`; за EOF — `InvalidCursor`; порожній output
до EOF — `BufferTooSmall`. Великий u64 offset перевіряється перед
звуженням. Розмір source повинен поміститися в u32. Копія provider позичає
ті самі байти; вона не створює snapshot. Читання допускає overlap з
mutable source через `memmove`; такий output змінить backing bytes, тому
для збереження source використовуйте окремий buffer.

## Власний provider із читанням та записом

Потрібен звичайний клас із точними `noexcept` сигнатурами. Базового класу
або macro реєстрації немає:

```cpp
resource::FileSize size() const noexcept;
resource::ReadResult read(resource::Cursor cursor,
                          resource::Output output) const noexcept;
resource::WriteResult write(resource::Cursor cursor,
                            resource::Input input, bool final) noexcept;
```

`size()` потрібен завжди; принаймні один із `read`/`write` повинен існувати.
Пропущений `write` робить файл read-only, пропущений `read` — write-only.
Capabilities обчислюються з наявних callbacks, а `stat()` питає актуальний
розмір. Provider може мати власний startup; готовність SD-драйвера чи
application state не створюється таблицею ресурсів.

`Cursor` — u64 непрозоре значення provider. Core не збільшує його і не
порівнює з `size()`. Для byte provider це offset; для іншого формату це
може бути sequence/state token. Нуль починає передачу. Далі клієнт
передає `result.next`, а не самостійно обчислену позицію.

| Результат | Що перевіряє клієнт |
| --- | --- |
| `ReadResult{status, next, written, eof}` | Статус; використовувати лише перші `written` байтів; продовжити з `next` до `eof` |
| `WriteResult{status, next, consumed, complete}` | Статус; відкинути лише `consumed` байтів input; продовжити з `next`; `complete` є підтвердженням provider |
| `FileStat{status, size, flags}` | Статус перед читанням розміру/capabilities |

`FileSize`, `written` і `consumed` — u32; `next` — u64 Cursor. Якщо provider
обчислює count у `std::size_t`, перевірте span/storage bounds і можливість
представити count у u32 перед перетворенням. Простий cast не перевіряє
переповнення; для малого фіксованого buffer його місткість уже задає межу.

На `Ok` count не перевищує наданий span. Незавершена операція повинна
передати байти або змінити курсор. `Ok/0` з тим самим курсором і
`eof=false`/`complete=false` не означає «повторіть пізніше»; це порушення
контракту. Поверніть відповідний статус, наприклад `BufferTooSmall`.
Завершена операція з нульовим count є нормальною.

На помилці provider повертає вхідний курсор, нульовий count та false
завершення. Клієнт відкидає output цієї операції. Уже зроблені побічні
ефекти запису core не відкочує: provider повинен визначити це сам.
Spans із аргументів `read()`/`write()` діють протягом одного синхронного
виклику; callback не зберігає їх для подальшої роботи.

`final=true` позначає останню частину submitted input, але не гарантує,
що provider прийняв її всю. Якщо `consumed < input.size()`, повторно
подайте suffix із тим самим `final=true` та поверненим курсором.
Порожній final input може завершити передачу. `complete` — окреме
підтвердження; семантику запису на Flash чи застосування settings задає
provider. У [Resources.cpp](../../examples/user_guide/Resources.cpp)
RAM provider бере максимум два байти: трьохбайтовий final input потребує
двох викликів. Клієнт перевіряє progress на кожному кроці.

## LIST, STAT, READ та WRITE через generic protocol

```cpp
#include <resource/protocol/Protocol.hpp>
auto reply = resource::protocol::process(files.view(), completeRequest,
                                         responseBuffer);
// У транспорт відправляється тільки responseBuffer[0 .. reply.written).
```

`Reply{status, written}` — локальний C++ результат, не packet struct.
Функція сама пише packet байтами в `responseBuffer`. Не передавайте весь
buffer із невикористаним хвостом або memory representation `Reply`.

Усі числа в packet little-endian. u8 bool — тільки 0 або 1. Після
path/data немає NUL. Пакет має точну довжину; зайві байти також є помилкою.

| Op | Request | Response |
| --- | --- | --- |
| LIST=1 | `u8 op, u64 cursor` | `u8 status, u64 next, u8 eof, u16 dataSize, data[]` |
| STAT=2 | `u8 op, u32 index` | `u8 status, u32 size, u8 flags` |
| READ=3 | `u8 op, u32 index, u64 cursor` | `u8 status, u64 next, u8 eof, u16 dataSize, data[]` |
| WRITE=4 | `u8 op, u32 index, u64 cursor, u8 final, u16 dataSize, data[]` | `u8 status, u64 next, u32 consumed, u8 complete` |

LIST повертає `[u16 pathLength][path bytes]` для кожного цілого запису.
Його cursor — порядковий індекс наступного файла, а не cursor provider.
LIST не викликає `size()` або читання provider. STAT питає поточний size.
READ/WRITE використовують provider cursor і його правила progress.

Розміри request: LIST 9, STAT 5, READ 13, WRITE `16 + dataSize` байтів.
Response header LIST/READ має 12 байтів, STAT — 6, WRITE — 14.
READ payload не перевищує 65535 байтів. Цілий LIST path разом із u16
length повинен поміститися в 65535, тому максимум path — 65533 байти.
Core дозволяє довші локальні шляхи; packet LIST поверне `InvalidData`
при старті на такому записі. Додаткові правила paging є в
[protocol README](../../lib/resource/protocol/README.md).

Неправильний packet, невірний final byte чи невідома операція можуть
отримати однобайтову відповідь `InvalidData`. Якщо нормальний response
header не поміщається, `Reply{BufferTooSmall, 0}` не викликає provider.
Клієнт перевіряє `written` перед розбором відповіді та не припускає, що
будь-яка помилка має звичайний envelope. Result provider із неможливим
count/status/progress перетворюється на `InternalError`.

Resource статуси: `Ok`, `InvalidFile`, `NotReadable`, `NotWritable`,
`InvalidCursor`, `CursorExpired`, `BufferTooSmall`, `InvalidData`,
`InternalError`. Їхні wire коди 0..8 за цим порядком фіксовані.
`Readable=1`, `Writable=2`; read/write файл має flags=3.

### Клієнт: зібрати request і перевірити response

`<resource/protocol/Packets.hpp>` надає header-only API у
`resource::protocol::client`. Усі builders приймають caller-owned `Output`
першим аргументом:

```cpp
namespace client = resource::protocol::client;
std::array<std::byte, resource::protocol::wire::readRequestSize> request{};
auto built = client::makeRead(request, fileIndex, cursor);
if (built.status == client::BuildStatus::Ok) {
    // Передайте тільки request[0 .. built.written).
    // Транспорт повертає один повністю зібраний receivedPacket.
    auto parsed = client::parseRead(receivedPacket);
    if (parsed && !parsed.shortError && parsed.response.status == resource::Status::Ok) {
        consume(parsed.response.data); // До повторного використання receive buffer.
        cursor = parsed.response.next;
    }
}
```

| Builder | Решта аргументів |
| --- | --- |
| `makeList(output, cursor)` | LIST cursor |
| `makeStat(output, index)` | індекс файла |
| `makeRead(output, index, cursor)` | індекс і provider cursor |
| `makeWrite(output, index, cursor, data, final)` | індекс, cursor, `Input`, final |

Результат усіх builders — `BuildResult{BuildStatus status, size_t written}`.
`Ok` означає готовий prefix для транспорту. `BufferTooSmall` і
`PayloadTooLarge` мають `written=0` та залишають output незмінним.
WRITE приймає не більш як 65535 байтів. Arrays, fixed spans та dynamic spans
працюють через той самий перевірений API. Розміри packet доступні як
constexpr у `resource::protocol::wire` з `Wire.hpp`.

`parseList(reply)`, `parseStat(reply)`, `parseRead(reply)` і
`parseWrite(reply, submittedBytes)` перевіряють точну довжину, status codes,
flags, boolean bytes та declared counts. LIST перевіряє всі length-prefixed
paths, їхній синтаксис і максимальну довжину 65533. Для WRITE передайте
довжину payload саме того request, на який прийшла відповідь: parser
перевіряє `consumed <= submittedBytes <= 65535`.

`ParseResult::parsing` (`Ok`/`Malformed`) відокремлений від remote
`response.status`. `if (parsed)` перевіряє лише правильність розбору;
provider може відповісти помилкою у коректному packet. Один байт
`InvalidData` теж є коректною відповіддю на неправильний request і має
`shortError=true`. У ньому значущий лише `response.status`: cursor, counts
та completion fields не були передані. Однобайтний `Ok` чи інший status
parser відхиляє. Caller перевіряє cursor progress та completion з урахуванням
попереднього request; READ/WRITE cursor може бути opaque.

**Parsed views позичають received packet.** `ReadReply::data`, кожен
`std::string_view` із `ListReply::paths` та його iterators дійсні, поки
receive buffer живий і незмінний. Скористайтеся ними до отримання наступного
packet у цей buffer. Копія parsed result чи path view не копіює bytes.
Клієнт не виділяє пам'ять і не повертає owning packet.

Повний runnable приклад із локальним обміном packet:
[ResourceClient.cpp](../../examples/user_guide/ResourceClient.cpp).

## UART/TCP: спочатку повний packet

`resource::protocol::process` приймає **один уже зібраний packet**.
Він не зберігає недочитані байти між викликами. Один TCP `recv()` може
дати частину packet або кілька packet разом. UART DMA/IRQ callback також
не визначає його межу. Не передавайте такий fragment відразу в process.

Нижній шар застосунку визначає межі, обмежує накопичення, збирає повний
packet і виділяє кожен packet окремо. Він також володіє framing, перевіркою
цілісності, timeout, прийомом/відправленням і disconnect. Бібліотека не
надає прихованого `PacketAssembler` чи готового socket/UART driver.
В [Resources.cpp](../../examples/user_guide/Resources.cpp) тільки
демонструється копіювання двох відомих fragments у buffer; довжина
packet відома harness заздалегідь. Справжній транспорт мусить визначити
цю довжину сам до виклику process.

`written=0` означає відсутність packet для відправлення. Після одержання
відповіді транспорт може потребувати кількох send/DMA операцій, щоб
передати всі `written` байти. Buffer та його contents залишаються
живими до завершення фізичної передачі. Наступний request не використовує
цей самий output, поки передача ще читає його.

Повторний WRITE знову викликає provider. Timeout не доводить, що попередній
запис не відбувся. Політику повторів, повторюваність операції та облік
результату визначає застосунок. RpcMailbox має власні правила збережених
duplicate requests; generic core не переносить їх на інші providers.

## Прямі encoded операції Model без файлового шару

Коли transport уже має операцію, точний u32 ID та payload, файли не потрібні.
Encoded access має два шляхи: direct catalog indexes без Model або compiled
adapter із coherent ModelView. Canonical methods/results/build requirements —
у [Model: Encoded API](Model.md#encoded-api-catalog-index-або-modelview).
Повний [Encoded.cpp](../../examples/user_guide/Encoded.cpp) оголошує
Device, Field/Command/Service tables, catalogs, Model, Workspace та
реальний `onCompletePacket`, який маршрутизує байти до цих API:

```cpp
namespace ts = telemetry;
auto field = ts::readFieldEncoded(model.view(), id, output, workspace);
auto write = ts::writeFieldEncoded(model.view(), id, input, workspace);
auto command = ts::executeCommandEncoded(model.view(), id, input, workspace);
auto service = ts::callServiceEncoded(model.view(), id, input, output, workspace);
```

`id` має тип `telemetry::PackedId` (`uint32_t`), output —
`std::span<std::byte>`, input — `std::span<const std::byte>`, workspace —
`telemetry::Workspace&`. Старші 16 біт ID — catalog position, молодші —
entry position. Кожна родина має свій простір IDs: Field 0, Command 0 та
Service 0 можуть одночасно існувати й означати різні callbacks.
Порядок declarations визначає identity; імена чи TypeId не підмінюють її.
До передачі ширшого зовнішнього числа в u32 adapter перевірте, що воно
поміщається, щоб не обрізати адресу до іншого endpoint.

Той самий checked доступ є в runtime indexes:

```cpp
model.fieldIndex().readEncoded(id, output, workspace);
model.fieldIndex().writeEncoded(id, input, workspace);
model.commandIndex().executeEncoded(id, input, workspace);
model.serviceIndex().callEncoded(id, input, output, workspace);
```

Compiled Model adapter приймає `ModelView` за значенням і позичає його
таблиці. Чотири вільні functions потребують `model/Adapter.cpp`.
`abi/StructuredAbi.cpp` потрібний для explicit `requireStructuredAbi()`;
qmake manifest додає обидва sources. Exact layout tag у adapter symbol та
explicit guard — різні linked boundaries, а не runtime checks кожного packet.
Прямі index/header-defined native операції цих object files самі не потребують.

У прикладі застосунок вибрав свій header: `u8 operation + u32 LE ID`,
потім native payload. Це **тільки формат прикладу**, а не wire protocol
telemetry чи resource LIST/STAT/READ/WRITE. `onCompletePacket` отримує
повний packet; UART/TCP framing він не реалізує. Повернений `guide::Reply`
є локальним результатом. Реальний transport явно кодує свій response
header/status і передає лише успішно записаний payload.

## Encoded без Model: catalog indexes

Transport може отримати тільки потрібні family indexes із application facade:

```cpp
// Fragments: catalogs, input/output spans and Workspace are already alive.
auto read = fields.index().readEncoded(id, output, workspace);
auto write = fields.index().writeEncoded(id, input, workspace);
auto command = commands.index().executeEncoded(id, input, workspace);
auto service = services.index().callEncoded(id, input, output, workspace);
```

`fields`, `commands` і `services` тут — **catalog tables**. Local
`FieldTable`/`CommandTable`/`ServiceTable` не мають `index()`. Index — невеликий
copyable borrowed view; facade може повернути його, якщо arrays/owners мають
стабільний довший lifetime. Model чи registry не створюються заради цього call.
Endpoint selection і canonical bytes ті самі, що у compiled adapter; transport
так само визначає framing та response envelope. Вибір між шляхами — про module
boundary/metadata requirements, а не про інший протокол чи інші callbacks.

Без Model required scratch можна взяти з entry metadata після `index.find(id)`;
перевірте pointer перед dereference. Field має окремі read/write requirements,
Command і Service — `scratchBytes`. Для direct index `Adapter.cpp` не потрібний;
manual explicit ABI guard залишається окремим рішенням module integration.

## Wire значення та перевірка результатів

Canonical payload shape, exact-length codec та standalone decode forms описані у
[Codec and Workspace](CodecAndWorkspace.md#wire-shape). Чотири різні encoded
results, fields/status meaning і refusal ordering мають один reference у
[Model](Model.md#encoded-результати-й-порядок-перевірок).

Transport спочатку перевіряє `dispatch`, і тільки при `Ok` — `endpointStatus`,
якщо відповідний result його має. Field read має лише dispatch і written;
Field write та Command не мають written. Для read/Service передавайте тільки
reported payload prefix; status header належить вашому envelope, а не canonical
T. Не відправляйте memory representation C++ result struct.

`Ok + WriteResult::InvalidValue` означає прикладну відмову після decode, тому
її не слід показувати клієнту як malformed packet. Bad bool чи length mismatch
відхиляються перед callback; borrowed response overlap може бути виявлений
пізніше, після getter/Service invocation. Успішний void Service і Service
refusal обидва можуть мати written=0: count не заміняє status check.
`CommandResult::Accepted` не створює library queue: queue і встановлення
завершення належать callback/owner застосунку.

## Workspace, локальний бюджет та borrowed response

Default local budget, shared Service Request/Result storage та overlap rules
описані у [Codec and Workspace](CodecAndWorkspace.md#local-object-budget).
Для transport scratch не є RX/TX buffer; його lifetime не задає lifetime
відправленого packet. Model дає fresh-operation bound:

```cpp
inline std::array<std::byte, model.maxScratch()> storage;
inline telemetry::Workspace workspace{storage};
```

Це достатній bound з урахуванням alignment для однієї encoded операції.
Nested/concurrent операціям потрібна пам'ять для всіх живих leases або
окремі Workspaces. Лізи звільняються у зворотному порядку. Buffer і
Workspace позичені, доступ до них та owner state серіалізує застосунок.
Wire buffers мають бути disjoint із Workspace, коли operation використовує
scratch. Повністю local endpoint Workspace не використовує. Configuration
macros мають бути однаковими в усіх translation units.

Getter `const T&` і Service `const Response&` чи
`BorrowedServiceResult<Response>` кодують існуючий const об'єкт. Вони
не копіюють цей response в Workspace. Borrowed Service витрачає payload
бюджет на request; статус/view не є response storage. Для fallible borrowed
Service використовуйте `success(existingObject)` або `failure(status)`.
Для великого owning response використовуйте
`ServiceResult<T>::successFrom(factory)`; `success(value)` призначений
для response до 256 native байтів.

У [Encoded.cpp](../../examples/user_guide/Encoded.cpp) `Report` має
64 wire байти. Owning `CopyReport` потребує Workspace; borrowed `ViewReport`
працює з порожнім Workspace при малому Query. Обидва мають однаковий
canonical response layout. Змінна `report` у Device живе весь час, коли
callback/encoder читає її; повернення посилання на локальний temporary
цього контракту не виконає.

Borrowed output мусить бути disjoint із повним native об'єктом, включно
з padding. Адреса стає відомою після getter/callback, тому такий overlap
може бути відхилений уже після його побічних ефектів. Transport envelope
теж не повинен записуватися поверх цього об'єкта. Borrowed view не
захоплює snapshot і не дає міжпольової coherence. Докладніше —
[BorrowedNativeValues.md](../BorrowedNativeValues.md).

Це також precondition для **повного** Values READ або Exchange output,
включно з token/status/packet headers. Перевірка borrowed payload prefix у
getter thunk не захищає caller від запису власного envelope поверх live object.
Exchange header не можна розміщувати поверх borrowed response, навіть коли
сам payload span disjoint. Тримайте окремий TX buffer до завершення send/DMA.

## DescriptorFile та ValuesFile у тій самій таблиці

Додайте providers до звичайного `filesystem(...)` поруч із власними
файлами. Шляхи задає застосунок:

```cpp
namespace rs = resource::telemetry::v3;
inline constexpr ::telemetry::descriptor::Descriptor descriptor{model};
inline constexpr auto descriptorBytes = ::telemetry::descriptor::packDescriptor<descriptor>();
inline constexpr rs::DescriptorFile descriptorFile{descriptorBytes};
inline constexpr rs::ValuesFile valuesFile{descriptor, workspace};
inline constinit const auto telemetryFiles = resource::filesystem(
    resource::file("/telemetry/descriptor.bin", descriptorFile),
    resource::file("/telemetry/values.bin", valuesFile));
```

`v3` називає wire 3.0 поточної Model. Descriptor містить типи, catalogs,
Fields/Commands/Services та cached fingerprint. Metadata і names залишаються
незмінними; owners і slot targets у fingerprint не входять. Для constexpr
Model packed bytes є способом зберегти готовий descriptor у Flash;
streaming Descriptor також підтримується для startup metadata.

Values містить 24-byte header із fingerprint і потім для кожного Field
`u8 status + wireSize<T> bytes`. Token має поміститися **цілим**. Status
Unavailable має zero payload, який не декодується як значення.
Це coherent returned value одного getter, але не спільний snapshot усієї
Model. Provider не викликає getter для token, який не поміститься.
Валідний Values cursor — позиція в header, початок token або EOF;
payload interior дає `InvalidCursor`.

Перевірте `valuesFile.maxTokenSize()` проти output capacity та protocol
65535-byte payload limit. Більший token не фрагментується цим provider;
повторення того самого too-small READ не дасть progress. Для Values
використовується тільки read scratch; `requiredWorkspace()` може бути
нульовим, навіть якщо великий Field setter потребує decoded storage.
Underlying tables/metadata та Workspace мають жити протягом усіх читань.
Формат і lifetime подробиці — у
[v3 provider README](../../lib/integration/telemetry/v3/README.md).

## RpcMailbox через Resource WRITE/READ

Готовий [RpcClient](../RpcClientV1.md) автоматизує request header, exact-byte
retry, status polling і typed decode без TCP/UART залежності. Application
доставляє повні відповідні replies з local generation/request ticket.
Для N серверних каналів [RpcSessionPool](../RpcSessionsV1.md) вибирає окремий
mailbox/filesystem і відкриває callback gate після підтвердження відповідного
final WRITE ACK. На одному multiplexed transport pairing/serialization
залишається application contract; нових полів у Resource/RPC wire немає.

Optional [RpcMailbox](../RpcMailboxV1.md) використовує той самий packet
processor і file array. Його qmake source обирається окремим
`CONFIG += resource_telemetry_rpc`; Descriptor/Values `resource_telemetry`
не компілює mailbox. Після вибору прапорців підключіть
`lib/integration/integration.pri`, який додає Telemetry і Resource dependencies.
Повний [host example](../../examples/rpc_mailbox/README.md)
публікує `/telemetry/rpc.bin`, складає RPC request у caller-owned bytes,
передає їх через `client::makeWrite()`, а потім починає READ із zero та
перевіряє його через `client::parseRead()`. Внутрішні RPC state/statuses —
окремий шар від зовнішнього Resource status. Client зіставляє transaction ID,
operation та endpoint, перевіряє Ready, потім dispatch і endpoint status,
і лише після цього декодує payload canonical codec-ом.

У mailbox немає peer identity або wire session ID. Ingress записує свій
`connectionGeneration` разом із peer у metadata **кожного** queued packet;
owner відсікає невідповідні peer/generation до Resource processor. Нове
з'єднання отримує нове generation, і descriptor fingerprint узгоджується
перед admission. Generation лишається поза wire. Одне очищення черги не
захищає від старого кадру, який ingress поставить із затримкою.

Owner періодично передає `process(nowMs)` application clock. Для останнього
WRITE він спочатку формує та віддає ACK transport-у, а вже наступним кроком
дозволяє `process()` виконати Pending request. ACK не чекає callback або
підтвердження доставки. `complete=true` означає seal, а disconnect/reset
може скасувати Pending без dispatch. Running callback не переривається;
`resetSession()` у Running є no-op, тому owner повторює reset після повернення.

Збережені bytes дозволяють підтвердити identical retry без нового callback
в одному admitted session. Після disconnect/reset deduplication history
втрачається. Якщо результат не отриманий, outcome може бути невідомим;
повтор неідемпотентної операції в новому сеансі може повторити ефект. Цю
невизначеність вирішує application. Mailbox не додає mutex, heap, task чи
паралельну обробку до синхронного Model adapter.

## Коли потрібен приклад Bind/Exchange

[examples/structured_protocol](../../examples/structured_protocol/README.md)
є готовим окремим прикладом control packet handlers. Він включається
явно і не є вимогою native, encoded або local resource операцій.

Transport зберігає `Binding` на admitted connection та окремий Workspace.
Bind порівнює wire version і descriptor fingerprint один раз для
поточної immutable Model. Він позичає **іменований живий `ModelView`**:

```cpp
#include <structured_protocol/Bind.hpp>
#include <structured_protocol/Exchange.hpp>

inline constexpr auto modelView = model.view();
example::structured_protocol::Binding peer;
auto bind = example::structured_protocol::Bind::process(
    peer, modelView, descriptor.fingerprint(), completeBindPacket, bindOutput);
auto call = example::structured_protocol::Exchange::process(
    peer, completeExchangePacket, exchangeOutput, workspace);
```

Bind має `TSBN/TSBA`, Exchange — `TSRQ/TSRP`. Exchange передає correlation
requestId, operation та packed endpoint ID; fingerprint не додається до
кожного виклику. Перевіряйте status і відправляйте тільки `written` байти.
`peer.reset()` при disconnect/model replacement скидає agreement; очищення
старих queued frames, admission, serialization та generation правила
належать транспорту. Не подавайте `model.view()` temporary у Bind.

Для цієї частини додайте `examples` до include path і явно компілюйте
`examples/structured_protocol/Bind.cpp` та `Exchange.cpp`, або включіть
`examples/structured_protocol/protocol.pri`. Звичайні telemetry/resource
manifests ці handlers не підключають.

Exchange підтримує FieldWrite, Command і Service; Field reading можна
робити прямим encoded доступом або Values. Correlation не є deduplication:
повторний packet знову виконує callback. Приклад не створює прихованої
черги повторів, persistent owner або транзакцій.

## Збірка та що перевіряють приклади

Загальний resource core потребує тільки `-std=c++20 -Ilib`.
Для resource packets додайте `lib/resource/protocol/Protocol.cpp`.
Encoded приклад потребує також include directories `lib/boost_pfr/include`,
`lib/magic_enum` та `lib/telemetry/model/Adapter.cpp` для чотирьох ModelView calls.
Explicit ABI guard додає `lib/telemetry/abi/StructuredAbi.cpp`; звичайний qmake
manifest і maintained приклади включають обидва sources. Direct catalog-index
encoded operations не потребують compiled adapter. Values додає свій
`lib/integration/telemetry/v3/detail/Values.cpp`. `.pri` manifests описані у
[resource README](../../lib/resource/README.md) та
[telemetry README](../../lib/telemetry/README.md).

Обидва [user guide приклади](../../examples/user_guide/README.md) —
самостійні host програми. Resources перевіряє table/provider binding,
partial writes, чотири resource operations, malformed input і недостатню
reply capacity. Encoded виконує routing по ID, business refusal окремо від
bad bool, повторний Command, Workspace preflight і owning/borrowed Service.
Це виконання прикладів на host, а не measurement UART/TCP або MCU.
