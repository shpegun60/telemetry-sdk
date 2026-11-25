# Services: typed request, response і application status

[API Overview](APIOverview.md) · [Native reference](NativeApi.md) ·
[Native-only приклад](../../examples/user_guide/NativeOnly.cpp)

> **Одна форма — дерево груп, шляхи й handles.** У Model із груп той самий
> endpoint оголошують усередині `ts::group(...)` і викликають за шляхом:
> `ts::call<model, "adc/Calibrate">(adc::Reference{1.25f})` або handle `calibrate.call(request)`. Шлях однаковий на пристрої, у Static C++, TS/JS і Dynamic
> клієнті: [одна операція в чотирьох місцях](APIOverview.md#одна-операція-в-чотирьох-місцях).
> Плоску форму з catalog tables, публічний `makeId` і typed доступ за ID
> [прибрано](APIOverview.md#прибрано): runtime ID обробляють `model.fields()`,
> `model.commands()` і `model.services()`, а catalogs позначають саме їх. Signatures,
> results і lifetime rules самих Field, Command і Service від форми Model не залежать.

`Service` відкриває прикладну операцію з відповіддю: одержати snapshot,
запитати дані каналу, прочитати large cached block, перевірити стан або
обчислити результат за одним request. Callback має no request або одну
aggregate struct; response також є aggregate struct або Void.

Definition та static `call<Id>()` повертають owning `ServiceResult<Response>`
або borrowed `BorrowedServiceResult<Response>`. Для runtime ID рекомендовано
`callAs<Response>(id[, request])` чи `callBorrowed<Response>(id[, request])`:
один result із спільним native status; [Flat runtime API](FlatNative.md).
У callbacks та encoded API application status відділений від
encoded routing/codec status. Один і той самий callback працює для direct
native call і transport adapter; connection/session/retry state Service
не зберігає.

Це довідник поточного C++20 API. `ts` у прикладах означає
`namespace ts = telemetry;`. Загальний include —
[`<telemetry/Telemetry.hpp>`](../../lib/telemetry/Telemetry.hpp).
Для current state без request дивіться [Fields](Fields.md), для action
тільки зі status — [Commands](Commands.md).

Native definition/local table не потребують Model або Workspace; native
global IDs додають catalogs. Публічні family headers —
[`<telemetry/service/ServiceTable.hpp>`](../../lib/telemetry/service/ServiceTable.hpp) і
[`<telemetry/service/ServiceCatalogs.hpp>`](../../lib/telemetry/service/ServiceCatalogs.hpp).
Supported DTO та reflection requirements залишаються чинними;
[Native reference](NativeApi.md) пояснює транзитивні залежності.

## 1. Повний приклад owning та borrowed responses

Приклад використовує три response форми й exact encoded query. Build
requirements ті самі, що в
[runnable guide examples](../../examples/user_guide/README.md).

```cpp
#include <telemetry/Telemetry.hpp>
#include <array>
#include <cassert>
#include <cstdint>

namespace ts = telemetry;

struct Query { std::uint16_t channel; };
struct Reply {
    std::uint32_t sequence;
    std::uint16_t gain;
};

struct Device {
    Reply cached{7, 100};
    bool busy = false;
    unsigned pings = 0;

    Reply snapshot() const noexcept { return cached; }
    const Reply& latest() const noexcept { return cached; }
    void ping() noexcept { ++pings; }

    ts::ServiceResult<Reply> query(const Query& request) const noexcept {
        if (request.channel >= 3)
            return ts::ServiceResult<Reply>::failure(ts::ServiceStatus::InvalidArgument);
        if (busy)
            return ts::ServiceResult<Reply>::failure(ts::ServiceStatus::Busy);
        return ts::ServiceResult<Reply>::success({cached.sequence, cached.gain});
    }
};

inline Device device;
inline constexpr ts::ServiceTable localServices{
    ts::service<&Device::snapshot>("Snapshot", device),
    ts::service<&Device::query>("Query", device),
    ts::service<&Device::latest>("Latest", device),
    ts::service<&Device::ping>("Ping", device),
};
// The same Services in a group of the device's Model: paths, runtime IDs, encoded calls.
inline constexpr ts::Model model{ts::group("device",
    ts::service<&Device::snapshot>("Snapshot", device),
    ts::service<&Device::query>("Query", device),
    ts::service<&Device::latest>("Latest", device),
    ts::service<&Device::ping>("Ping", device))};
inline constexpr auto& services = model.services();

int main() {
    const auto owned = localServices.call<0>();
    assert(owned.hasValue() && owned.value().sequence == 7);
    const auto borrowed = localServices.call<2>();
    assert(borrowed && borrowed.valueOrNull() == &device.cached);
    assert(localServices.call<3>().status() == ts::ServiceStatus::Ok);
    assert(device.pings == 1);

    constexpr auto queryId = ts::idOf<model, "device/Query">();
    const auto rejected = ts::call<model, "device/Query">(Query{9});
    assert(rejected.status() == ts::ServiceStatus::InvalidArgument);
    assert(!rejected.hasValue());

    std::array<std::byte, ts::wireSize<Query>> input{};
    std::array<std::byte, ts::wireSize<Reply>> output{};
    std::array<std::byte, model.maxServiceScratch()> storage{};
    ts::Workspace workspace{storage};
    assert(ts::encode(Query{1}, input) == ts::CodecStatus::Ok);
    const auto result = services.index().callEncoded(queryId, input, output, workspace);
    assert(result.dispatch == ts::DispatchStatus::Ok);
    assert(result.endpointStatus == ts::ServiceStatus::Ok);
    assert(result.written == output.size());
    assert(workspace.used() == 0);
}
```

Plain `Reply` return автоматично стає successful owning result. Plain
`const Reply&` стає successful borrowed result. Plain `void` стає
`ServiceResult<void>::success()`. Explicit wrapper callback зберігає
application refusal: `query` може повернути `Busy` чи `InvalidArgument`
без жодного response object.

Runnable examples із broader coverage:
[Native.cpp](../../examples/user_guide/Native.cpp),
[Encoded.cpp](../../examples/user_guide/Encoded.cpp),
[browser payload tools](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/examples/structured_client/README.md).

## 2. Request та response shapes

| Частина | Підтримувана форма | Meaning |
| --- | --- | --- |
| No request | `Response operation() noexcept` | `Request = void`, native `call()` |
| Request by value | `Response operation(Request value) noexcept` | Exact struct request |
| Request by const reference | `Response operation(const Request& value) noexcept` | Exact struct request без writable reference |
| Owning plain response | `Reply operation() noexcept` | Нормалізується до `ServiceResult<Reply>` із Ok |
| Borrowed plain response | `const Reply& operation() noexcept` | Нормалізується до `BorrowedServiceResult<Reply>` із Ok |
| Owning status wrapper | `ServiceResult<Reply> operation(const Query&) noexcept` | Callback визначає status та owning payload |
| Borrowed status wrapper | `BorrowedServiceResult<Reply> operation(const Query&) noexcept` | Callback визначає status та borrowed view |
| No response | `void operation() noexcept` | `ServiceResult<void>` із Ok |
| Status-only response | `ServiceResult<void> operation(const Query&) noexcept` | Success/refusal без payload |

`Request` та `Reply` — ordinary supported aggregate structs із fixed
scalars, supported scoped enums, `std::array` або nested aggregates.
Scalar/array як весь request чи response не дозволяється: обгорніть його
в struct, наприклад `struct Measurement { float voltage; };` або
`struct Samples { std::array<float, 16> values; };`.
No-request/no-response `telemetry::Void` є alias `void`. Empty struct
залишається struct, хоч її wire payload може мати zero bytes.

Не підтримуються pointers, mutable/rvalue reference request, volatile
payload, два чи більше arguments, variadic/throwing callback, volatile
owner method, rvalue-qualified owner method, mutable-reference/rvalue-reference
response або reference на result wrapper. Result wrapper має бути повернутий
exactly by value: `ServiceResult<Reply>` чи `BorrowedServiceResult<Reply>`.
Plain response має бути unqualified `Reply`, exact `const Reply&` або void.

`std::optional<Reply>` не є service outcome protocol. Перетворіть прикладний
optional у `ServiceResult<Reply>::success` / `failure` всередині callback.
`CommandResult` не є заміною service status; використовуйте
`ServiceResult<void>` для operation з application refusal без response.

Request exact type перевіряється на native call: implicit conversion до
іншої aggregate не виконується. У Service немає `readAs`, `writeAs` чи
generic numeric request conversion. Виконайте бажаний conversion/validation
у application adapter перед формуванням Request.

## 3. Усі declaration та binding forms

Фабрика `telemetry::service` створює `ServiceDefinition<Binding>` із inferred
Request/Response. Name plus binding — повний endpoint declaration.

| Форма | Приклад | Target/lifetime |
| --- | --- | --- |
| Known free function | `service<&queryFree>("Query")` | Exact function у binding type |
| Known owner method | `service<&Device::query>("Query", device)` | Borrowed actual owner |
| Const owner | `service<&Device::query>("Query", constDevice)` | Compatible const method |
| Owner wrapper | `service<&Device::query>("Query", std::cref(device))` | Borrowed underlying object |
| Rebindable owner | `service<&Device::query>("Query", ownerSlot)` | Borrowed `OwnerSlot<Device>` |
| Runtime function pointer | `service("Query", &queryFree)` | Exact pointer copied into binding |
| Capture-free lambda | `service("Ping", []() noexcept {})` | Copied converted function pointer |
| Named functor | `service("Query", functor)` | Borrowed callable lvalue |
| Named capturing lambda | `service("Query", closure)` | Borrowed closure lvalue |
| Callable wrapper | `service("Query", std::ref(closure))` | Borrowed underlying callable |
| Callable slot | `service("Query", querySlot)` | Borrowed stable slot address |

Method owner може бути exact object або compatible derived object;
cv/base adjustment не створює temporary. Pointer/smart-pointer variables
не приймаються як owner: передайте перевірений live `*pointer` або `OwnerSlot`.
Temporary owner, proxy conversion до owner та temporary capturing callable
не може стати borrowed endpoint. Service factory має окремі guards також
для explicit owner type та braced arguments; prefer звичайну deduction.

Const owner/callable зберігає constness: binding має бути noexcept-invocable
на фактичному об'єкті. Noexcept lvalue-qualified methods підтримуються.
Overloaded method потребує вибору exact member pointer. Ordinary class
callable має мати однозначну signature; generic/overloaded callable можна
resolve exact slot signature або обгорнути typed lambda adapter.
Runtime pointer копіюється: зміна source pointer variable не rebind definition.

Name storage borrowed і immutable. Non-null, nonempty valid UTF-8 name має
fit string ceiling. Invalid factory name — constexpr construction diagnostic
або runtime `abort`, а не ServiceStatus. Descriptor перевіряє duplicate names
у відповідних scopes; native/encoded endpoint lookup використовує positions.
Units, limits, defaults та application metadata factory не додає.

### Slot форми для різних response contracts

Signature може бути `Reply(const Query&) noexcept`,
`ServiceResult<Reply>(const Query&) noexcept`,
`const Reply&() noexcept`, `BorrowedServiceResult<Reply>() noexcept`,
`void() noexcept` або `ServiceResult<void>(const Query&) noexcept`.
Slot не змінює declared return contract.

| Slot | Bind API | Ownership |
| --- | --- | --- |
| `OwnerSlot<Device>` | `bind(device)`; `service<&Device::query>("Query", slot)` | Borrowed owner |
| `FunctionSlot<Signature>` | `bind(&queryFree)` | Exact native function pointer |
| `ContextFunctionSlot<Signature>` | `bind(callback, context)` | Extra first `void*` callback argument; borrowed context |
| `DelegateRefSlot<Signature>` | `bind<&freeFunction>()`, `bind<&Device::query>(device)`, `bind(namedCallable)` | Borrowed owner/callable або free target |
| `DelegateSlot<Signature, Bytes, Align>` | `bind([&device](const Query& q) noexcept { return device.query(q); })` | Owned bounded inline closure |

Default owned capacity — 32 bytes; default alignment —
`alignof(std::max_align_t)`. Owning bind copies lvalue closure або moves
rvalue closure, включаючи compatible move-only closures. Construction,
movement, destruction та invocation nothrow; target має fit size/alignment
і не переходить на heap. Referenced captures залишаються borrowed.

Усі slots noncopyable/nonmovable та мають `reset`, `available`, `get` і
explicit bool. Callable slots мають `invoke` з engaged-target precondition;
checked Service call спершу перевіряє selected target. Function/context
snapshots — values, delegate snapshots — borrowed views. Slot bind/reset,
owner destruction і invocation serialized зовні; owned callback не може
знищити/rebind власний executing closure. Exact signature resolution та
всі bind overloads описано в
[runtime slot guide](../../lib/telemetry/slot/README.md).

## 4. ServiceDefinition: API та normalizing rules

| Member | Тип | Meaning |
| --- | --- | --- |
| `Request` | Struct або void | Native input shape |
| `Response` | Struct або void | Payload type після result unwrapping |
| `borrowsResponse` | `static constexpr bool` | Plain const-reference або borrowed wrapper |
| `Result` | `ServiceResult<Response>` / `BorrowedServiceResult<Response>` | Native normalized result |
| `name()` | `const char*` | Borrowed endpoint name |
| `call()` | `Result` | No-request call |
| `call(request)` | `Result` | Exact request call |

| Callback return | `call` result | Поведінка |
| --- | --- | --- |
| `Response` | `ServiceResult<Response>` | Construct successful owned payload |
| `const Response&` | `BorrowedServiceResult<Response>` | Successful view, без response copy |
| `ServiceResult<Response>` | Same owning wrapper | Preserve application status/payload |
| `BorrowedServiceResult<Response>` | Same borrowed wrapper | Preserve application status/view |
| `void` | `ServiceResult<void>` | Callback completes, then return Ok |
| `ServiceResult<void>` | Same status-only wrapper | Preserve application status |

Unavailable binding повертає native failure із `ServiceStatus::Unavailable`
до callback. Target snapshot робиться один раз і застосовується як до
availability check, так і до invocation. Valid direct owner не потребує
runtime null-owner check, але його lifetime обов'язковий.

## 5. Owning ServiceResult

`ServiceResult<T>` тримає status та optional active object у union. Object
`T` існує рівно для status Ok. Failure не default-constructs response і не
змушує створювати dummy data.

| API | Результат/контракт | Meaning |
| --- | --- | --- |
| `ServiceResult<T>::success(value)` | Owning successful result | Convenient by-value payload, `sizeof(T) <= 256` |
| `ServiceResult<T>::successFrom(factory)` | Owning successful result | Factory повертає exact `T`, construct у final result storage |
| `ServiceResult<T>::failure(status)` | Failure без `T` | Status має бути defined non-Ok code |
| `status()` | `ServiceStatus` | Application outcome |
| `hasValue()` | `bool` | `status() == Ok` |
| `value() &` | `T&` | Mutable owned payload, precondition engagement |
| `value() const&` | `const T&` | Const owned payload, precondition engagement |
| `value() &&` | `T&&` | Move доступ до owned payload, precondition engagement |
| `value()` на const rvalue | `const T&` через `const&` overload | Наявна return category; lifetime result не подовжується |
| `valueOrNull() &`, `valueOrNull() const&` | `T*` або `const T*` | Owned payload pointer на success, null на failure; result має лишатися живим |
| `valueOrNull() &&`, `valueOrNull() const&&` | Видалені | Pointer із owning temporary не отримується |
| Copy/move constructors і assignment | Owning state/payload operations | Copy/move payload коли він engaged; source move не автоматично failure |
| Destructor | Ends owned `T` lifetime | Failure не destroys неіснуючий payload |

Owning `ServiceResult<T>` не має `operator bool`, `operator*` або
`operator->`: використовуйте `hasValue`, `value` та `valueOrNull`.
Payload destructor має бути noexcept. Wrapper copy/move properties залежать
від payload operations; вони не є універсальною гарантією nothrow.
Endpoint callback має завершуватися noexcept згідно із declaration contract.

Reference/pointer на owned payload не подовжує lifetime result. Для pointer
зберігайте named owner:

```cpp
auto response = localServices.call<0>();
if (const auto* value = response.valueOrNull())
    consume(*value); // response живий до завершення consume.
```

`ServiceResult<T>::valueOrNull()` має той самий lvalue-only contract, що й
owning flat `ServiceCallResult<T>`: `&&` / `const&&` overloads явно видалені.
Це навмисно відхиляє також immediate-use pointer access на temporary, який
раніше міг бути коректним у межах full expression. Перевірка compile-time,
без runtime checks чи зміни storage/layout. `value()` зберігає попередні
return categories; для owning move-out спочатку перемістіть payload у власний
object через `auto payload = std::move(response).value();`, а не зберігайте
reference на temporary wrapper. Для const rvalue `ServiceResult<T>` наявний
`value() const&` повертає `const T&`, а не додає новий move overload.

Для великої відповіді створюйте payload прямо через `successFrom`:

```cpp
struct Block { std::array<std::uint32_t, 128> words; };

ts::ServiceResult<Block> makeBlock() noexcept {
    return ts::ServiceResult<Block>::successFrom([]() noexcept -> Block {
        return Block{};
    });
}
```

`success(Block{})` для такого типу відхиляється compile-time convenient
factory bound. Ця межа не є максимальною Service response size і не
дорівнює encoded local-object budget. `successFrom` уникає implicit large
by-value factory temporary; загальний callback stack залежить і від вашої
реалізації. Plain `Block` callback return також normalizes через
`successFrom` у Service adapter.

`ServiceResult<void>` має тільки `success()`, `failure(status)`, `status()`
та `hasValue()`. Тут `hasValue()` означає success, response object відсутній,
wire response size zero. `failure(Ok)` та unknown failure status є contract
violation й завершуються через `abort`.

## 6. BorrowedServiceResult і lifetime відповіді

`BorrowedServiceResult<T>` тримає status та `BorrowedValue<T>` const-view.
Він не копіює, не переміщує й не destroys response object.

| API | Meaning |
| --- | --- |
| `BorrowedServiceResult<T>::success(value)` | Borrow exact non-volatile `T` lvalue; temporary/proxy rejected |
| `failure(status)` | Defined non-Ok status, empty view |
| `status()` | Application outcome |
| `hasValue()`, `explicit operator bool` | Response view engaged |
| `valueOrNull()` | `const T*`; safe на failure |
| `value()`, `operator*()` | `const T&`; requires engaged view |
| `operator->()` | Borrowed const pointer; dereferencing requires engagement |
| Copy/move wrapper | Копія view/status без lifetime extension |

Borrowed payload має бути non-void aggregate для Service. Borrowed `void`
не існує; status-only outcome використовує `ServiceResult<void>`.
`failure(Ok)` або unknown status — contract violation.

Для native call referenced response має жити після return до останнього
використання caller view. Це не snapshot: owner може змінити той самий
object через інший шлях. Serializer потребує alive/stable response протягом
усього encoding. Generic const-reference return не може вказувати на local
callback variable, знищену на return.

Borrowed response може посилатися на native caller request, якщо його
lifetime достатній. Якщо callback повертає `const Request&`, виклик із
temporary request створює view, що втратить object після завершення full
expression. Для native persisted view використовуйте named request.
Encoded callback може повернути reference на decoded request: request
залишається живим до завершення response encoding, після чого цей reference
не зберігається library і не може бути retained застосунком.

## 7. Table, catalog та positional IDs

`ServiceTable` володіє declarations і erased rows; local position —
declaration order. `model.services()` — catalogs моделі (internal engine під
деревом); pre-order позиція групи визначає high part global ID.

| API | Тип/результат | Meaning |
| --- | --- | --- |
| `local.get<Position>()` | `const ServiceDefinition&` | Exact declaration |
| `local.call<Position>()`, `call<Position>(request)` | Definition `Result` | Static local typed call |
| `local.callAs<Response>(position[, request])` | `ServiceCallResult<Response>` | Рекомендований owning runtime виклик; `Response` може бути `void` |
| `local.callBorrowed<Response>(position[, request])` | `BorrowedServiceCallResult<Response>` | Рекомендований borrowed runtime виклик |
| `local.callAs<Result>(position[, request])` | `NativeCallResult<Result>` | Low-level exact result wrapper та routing окремо |
| `model.services().callAs<Response>(id[, request])` | `ServiceCallResult<Response>` | Рекомендований owning runtime packed-ID виклик |
| `model.services().callBorrowed<Response>(id[, request])` | `BorrowedServiceCallResult<Response>` | Рекомендований borrowed runtime packed-ID виклик |
| `model.services().callAs<Result>(id[, request])` | `NativeCallResult<Result>` | Low-level exact result wrapper та routing окремо |
| `size()`, `empty()` | `size_t`, `bool` | Local entry count / catalog group count |
| `data()`, `begin()`, `end()` | Borrowed row pointers | Erased rows/catalog iteration |
| `operator[](i)` | `const ServiceEntry&` / `const ServiceCatalog&` | Unchecked, `i < size()` required |
| `forEach(visitor)` | `void` | Typed traversal, без implicit execution |
| `forEachWhile(visitor)` | `bool` | Typed traversal до exact-`bool` false |
| `model.services().forEachEntry(visitor)` | `void` | Packed ID, group name та erased ServiceEntry |
| `model.services().forEachEntryWhile(visitor)` | `bool` | Erased обхід до exact-`bool` false |
| `visit(position_or_id, visitor)` | `bool` | Checked typed selection; visitor return ignored |
| `model.serviceIndex()` | `ServiceIndex` | Copyable borrowed runtime view |

Local table не має `index()`. `local[position].callEncoded` працює з
erased row після вашого local bounds check; global external ID перевіряється
через `model.serviceIndex().find/callEncoded`. Typed доступу за compile-time
packed ID (`call<Id>()`) немає: його дає шлях або handle.

`PackedId` — integer u32 з 16-bit group і 16-bit entry. Наприклад,
`ts::idOf<model, "device/Query">()` — другий Service групи. Field/Command/Service
ID spaces окремі; transport operation обирає family. Reordering declarations
змінює IDs, а зміна slot target — ні. Names не шукаються при dispatch.

Local position може бути integer/scoped enum. Global packed ID має бути
integral, не local enum. Static positions перевіряються compile-time;
runtime original width перевіряється до narrowing, тому negative/wide
input не wraps до валідного endpoint. Публічного `makeId` немає.
`tryGroupOf`/`tryIndexOf` fallible; `groupOf`/`indexOf` вимагають valid u32.
Range validity ще не означає, що catalog/row існує у вашій моделі.

Local `forEach` callback — ordinary `visitor(definition)` або optional
`operator()<Position>(definition)`. Global `forEach` —
`visitor(catalogName, definition)` або
`operator()<Group, Entry>(catalogName, definition)`. Local/global `visit`
обидва передають тільки `definition`. `visit` result означає selection,
а не successful Service response. Visitors повинні компілюватися для всіх
heterogeneous definitions; exact request/response branch обирайте через
`if constexpr`.
`forEachWhile` має ті самі typed callback forms, але вимагає exact `bool`;
true означає повний обхід, false — requested stop. `forEachEntry` показує
metadata без response invocation. Приклади й усі traversal contracts —
[Tables and catalogs](TablesAndCatalogs.md#обійти-definitions-або-metadata).

### Runtime Service з одним result

Для Query із першого повного прикладу:

```cpp
const Query request{1};
const auto result = services.callAs<Reply>(ts::idOf<model, "device/Query">(), request);
assert(result && result.status() == ts::ServiceCallStatus::Ok);
assert(result.value().sequence == 7);

const auto borrowed = localServices.callBorrowed<Reply>(2);
assert(borrowed && borrowed.valueOrNull() == &device.cached);
const auto ping = localServices.callAs<void>(3);
assert(ping);
ping.value(); // Successful void operation; response object відсутній.
```

`ServiceCallResult<T>` та `BorrowedServiceCallResult<T>` мають `hasValue()`,
explicit `bool`, `status()` і `value()`. `bool` true лише при successful
selection **та** application success. Спільний `ServiceCallStatus` має
`Ok`, `NotFound`, `SignatureMismatch`, `Unavailable`, `Busy`, `InvalidArgument`,
`Failed`. Nonvoid `valueOrNull()` safe на кожній відмові та повертає null.
`value()` потребує success; новий фасад перевіряє цю precondition через `abort`.

Owning `ServiceCallResult<T>::valueOrNull()` доступний лише для `&` / `const&`;
`&&` / `const&&` overloads видалені. Спочатку збережіть result у змінній:
`const auto result = services.callAs<Reply>(id, request);`, а потім отримайте
pointer через `result.valueOrNull()`. Pointer не продовжує lifetime result.
Навіть immediate-use виклик accessor на owning temporary не компілюється.
Це source contract без runtime checks чи зміни layout; owning `value() &&`
для move-out лишається доступним. У borrowed фасаду accessor на temporary
дозволений, але backing response повинен жити до останнього pointer use.

Response template argument — exact unqualified aggregate або `void` для
owning form. Borrowed form потребує nonvoid borrowed endpoint і повертає
лише `const T&`; зміна owner видима через view. Request/Response/ownership
mismatch дає `SignatureMismatch` до callback, також для empty slot.
Binding unavailable чи application refusal дає false result без response.
Фасад конструює існуючий nested result у final member storage, без додаткового
response copy/move або heap. Existing borrowed lifetime/synchronization rules
збережено. Повні signatures: [Flat runtime API](FlatNative.md).

### Low-level exact runtime `callAs`

Наступний API збережено без змін для caller, якому потрібні окремі routing
і application statuses. Static `call<Position/Id>()` та encoded API теж незмінні.

`callAs<Result>(position_or_id[, request])` повертає `NativeCallResult<Result>`.
`Result` має бути exact `ServiceResult<Response>`,
`BorrowedServiceResult<Response>` або `ServiceResult<void>`, який повертає
definition `call()`. Request теж exact після зняття cv/ref. Plain `Response`
callback уже normalized до `ServiceResult<Response>`; caller запитує wrapper,
а не сам `Response`. Owning та borrowed wrappers не взаємозамінні.
Conversion чи універсального owning response container тут немає.

Для Query із першого повного прикладу typed visitor раніше міг явно зберігати
response і selection окремо:

```cpp
using Result = ts::ServiceResult<Reply>;
Result outcome = Result::failure(ts::ServiceStatus::Unavailable);
const Query request{1};
const ts::PackedId id = ts::idOf<model, "device/Query">();
const bool selected = services.visit(id, [&](const auto& definition) {
    using Definition = std::remove_cvref_t<decltype(definition)>;
    if constexpr (std::is_same_v<typename Definition::Request, Query> &&
                  std::is_same_v<typename Definition::Result, Result>)
        outcome = definition.call(request);
});
```

Для одного known Request/Result у low-level формі достатньо `callAs`:

```cpp
const Query request{1};
const auto result = services.callAs<ts::ServiceResult<Reply>>(
    ts::idOf<model, "device/Query">(), request);
assert(result.status() == ts::NativeCallStatus::Ok && result.hasValue());
assert(result.value().status() == ts::ServiceStatus::Ok);
assert(result.value().hasValue() && result.value().value().sequence == 7);

const auto borrowed = localServices.callAs<ts::BorrowedServiceResult<Reply>>(2);
assert(borrowed.hasValue() && borrowed.value().valueOrNull() == &device.cached);
const auto ping = localServices.callAs<ts::ServiceResult<void>>(3);
assert(ping.hasValue() && ping.value().hasValue());
```

| `NativeCallStatus` | Meaning |
| --- | --- |
| `Ok` | Exact endpoint result доставлено, включно з application failure |
| `NotFound` | Position/ID invalid на original width або group/row відсутня |
| `SignatureMismatch` | Exact Request, no-request форма або Result wrapper не збігається |

Shape checks передують callback та binding availability. `NotFound` і
`SignatureMismatch` не викликають Service і не конструюють endpoint Result.
Empty compatible slot дає selection `Ok`, всередині —
`ServiceStatus::Unavailable` без response. `result.hasValue()` означає лише,
що inner wrapper існує: це **не** `result.value().hasValue()` і не application
success. Наприклад `Ok + ServiceStatus::Busy` має outer `hasValue() == true`,
inner `hasValue() == false`. `value()` вимагає outer engagement;
`valueOrNull()` повертає null для selection failure. Native selection statuses не є
encoded dispatch або wire codes.

`NativeCallResult<Result>` **володіє Result wrapper**, навіть коли Result є
`BorrowedServiceResult<T>`. Outer `valueOrNull()` вказує саме на цей wrapper;
named outer result має жити до останнього використання pointer. Outer accessor
доступний лише для `&` / `const&`, а `&&` / `const&&` явно видалені для обох
видів inner wrapper. Це не змінює `NativeCallResult::value()`: mutable rvalue
дає `Result&&`, const rvalue — наявний `const Result&&`.
`result.value().valueOrNull()` натомість дістає pointer із inner wrapper:
для owning `ServiceResult<T>` він позичає owned payload, а для
`BorrowedServiceResult<T>` — зовнішній application T. У другому випадку
lifetime T визначає owner, а не outer result. Не плутайте ці два pointer levels.
Accessor самого `BorrowedServiceResult<T>` на temporary лишається допустимим,
бо знищення borrowed wrapper не знищує зовнішній T. Це не подовжує lifetime
response або Request, на який response може посилатися.

`NativeCallResult<Result>::successFrom(factory)` вимагає exact `Result` і
будує його прямо в final storage. Runtime dispatch повертає prvalue endpoint
wrapper у це сховище, без additional move, optional accumulator чи dummy
response. Owning result займає bytes власного payload; caller обирає, де
тримати wrapper, і budgets callback stack. Borrowed wrapper позичає response
за тим самим lifetime contract; callAs не продовжує lifetime request/response.
Encoded Workspace та local-object policy лишаються своїм окремим contract.

Runtime calls вимагають lvalue table/catalog і перевіряють input width до
narrowing. Typed `visit` лишається варіантом для різних Request/Result або
custom handling. Static `call<Position/Id>` зберігає свій exact API;
wire bytes обробляються через `callEncoded`. Runnable coverage:
[NativeCalls.cpp](../../tests/ergonomics/NativeCalls.cpp).

`RootTypes` містить positional пари `Request0, Response0, Request1, Response1, ...`:
два roots на кожен Service, включно з `Void`, зі збереженням повторів.
`RegistryRootTypes` містить унікальні типи в порядку першого входження й
використовується лише для побудови Registry. Каталог має обидва aliases й
зберігає порядок груп та пар. `staticSize` рахує Services у локальній таблиці
або групи каталогу, а `TypeStorage<Registry>` має pair для кожного Service.
`ServiceTypePair` має `requestTypeId`
та `responseTypeId`; `ServiceTypeCatalog` містить matching positional array.
`model.view().serviceTypeIds(id)` повертає optional pair. `Model::typeId<T>()`
та `model.types()` працюють із complete registry; TypeId не є endpoint ID.

## 8. Erased encoded API та storage

| API | Результат | Meaning |
| --- | --- | --- |
| `ServiceEntry::callEncoded(input, output, workspace)` | `EncodedCallResult` | Checked payload operation однієї row |
| `ServiceIndex::find(id)` | `const ServiceEntry*` | Checked lookup, nullptr для invalid/absent ID |
| `ServiceIndex::callEncoded(id, input, output, workspace)` | `EncodedCallResult` | Lookup та bounded call |
| `ServiceIndex::catalogs()`, `count()` | Pointer, `uint32_t` | Borrowed catalog array та group count |
| `callServiceEncoded(modelView, id, input, output, workspace)` | `EncodedCallResult` | Compiled ModelView boundary із exact ABI tag |

`ServiceEntry` містить `context`, raw `invoke`, `name`, `requestWireBytes`,
`responseWireBytes` та `scratchBytes`. Викликайте checked `callEncoded`,
не raw pointer без preflight. Index з user-supplied catalog pointers/count
вимагає coherent live backing rows; `find` перевіряє IDs, а не forged pointers.

Input має мати exact request wire length. Output має вмістити всю successful
response **до callback**, навіть якщо application callback потім відмовить.
No-request input — empty span, Void response потребує zero payload capacity.
Codec перевіряє native representation, включаючи canonical bool 0/1;
application channel/range/mode checks залишаються callback policy.

Порядок boundary: lookup → input/output length checks → scratch overlap та
capacity → decode complete request → select target → invoke → encode response.
No application callback при length/representation/capacity preflight failure.
Borrowed-response encoding може відхилити payload/native-object alias вже
після callback; encoded failure не відкочує side effects, виконані callback.

Input/output spans можуть overlap, включаючи partial overlap: decoded native
request сформовано до першого output byte. Але якщо Service потребує
Workspace, обидва supplied spans мають бути disjoint від Workspace storage.
Borrowed response payload output має бути disjoint від referenced native
object; тримайте transport buffers окремо від live owner objects.

Encoded storage policy використовує default local-object budget 32 bytes
(`TELEMETRY_STRUCTURED_LOCAL_BYTES`), configurable узгоджено в усіх TUs.
Request отримує local budget першим; actual owning result wrapper використовує
залишок. Решта native objects створюється в aligned Workspace leases.
Budget zero примушує owning/request object storage через Workspace.
Void та borrowed response не потребують owning response object storage;
borrowed response без large request може працювати з zero scratch.

`scratchBytes` — сума required request/result reservations із worst-case
alignment. `model.maxServiceScratch()` достатній для однієї fresh Workspace
operation; `model.maxScratch()` включає всі endpoint families. Native object
size, wire size та result-wrapper size відрізняються. Local budget не є
bound повного call stack. Реальні callback frames та nested calls потребують
окремого вимірювання.

Request lease живе до response encoding; result lease звільняється перед
request lease. Workspace і byte storage external, lease release — LIFO.
Callback не може retain decoded request reference чи leased result після
call return. Nested/concurrent operations потребують бюджету всіх live
leases або separate Workspaces. Дивіться
[Workspace](../../lib/telemetry/codec/Workspace.hpp) та
[StoragePolicy](../../lib/telemetry/codec/StoragePolicy.hpp).

## 9. Application status та dispatch status

| `ServiceStatus` | Code | Meaning |
| --- | --- | --- |
| `Ok` | 0 | Successful response або successful Void operation |
| `InvalidArgument` | 1 | Callback відхилив business request |
| `Unavailable` | 2 | Native binding unavailable або application data/operation відсутня |
| `Busy` | 3 | Callback зараз не може виконати request |
| `Failed` | 4 | Application operation завершилася невдачею |

| `DispatchStatus` | Code | Encoded meaning |
| --- | --- | --- |
| `Ok` | 0 | Application status доставлено; refusal має zero payload |
| `NotFound` | 4 | Invalid/absent endpoint ID |
| `InvalidPayload` | 5 | Length, representation, scratch overlap або borrowed-response alias refusal |
| `BufferTooSmall` | 6 | Output недостатній для declared successful response |
| `WorkspaceTooSmall` | 7 | Немає достатнього aligned request/result storage |
| `InternalError` | 8 | Internal result-construction invariant не виконано |
| `Unavailable` | 9 | Binding target absent до callback |

`EncodedCallResult` має `dispatch`, `endpointStatus` та `written`.
Спершу перевірте dispatch. При `Ok` endpointStatus meaningful:

- `Ok + ServiceStatus::Ok` — response payload у `output[0..written)`;
  written дорівнює declared response wire size, включаючи zero для Void
  або empty response struct.
- `Ok + InvalidArgument/Unavailable/Busy/Failed` — application refusal,
  written zero, response payload відсутній.
- `dispatch != Ok` — application result не доставлено; endpointStatus
  default не є підтвердженням success, written zero.

Default constructors полів encoded result не створюють готової successful
відповіді. Відправляйте payload лише відповідно до returned dispatch/status/
written. In-process result layout не є packet envelope; transport має
encode власні status fields і correlation. Frozen canonical payload та
descriptor визначає [WireV3](../WireV3.md), transport flow —
[TransportWalkthrough](TransportWalkthrough.md).

## 10. Lifetime, state та повторні запити

Owners, named callables, name memory, slots, local tables та catalogs
borrowed і stable до останньої invocation/view use. Tables noncopyable/
nonmovable: runtime contexts можуть borrow table-owned bindings.
Borrowing `get`, row pointers, traversal та catalog `index` APIs require
lvalue tables. ModelView/ServiceIndex копіює views, не ownership.

Externally serialize calls, owner mutation, slot bind/reset/destruction та
Workspace access. Const method не робить underlying mutable state atomic.
Plain owning response дає власний native object, але snapshot coherence
його членів має забезпечити callback; borrowed response додатково вимагає
стабільного об'єкта під час caller use/encoding. Жодного cross-Service
transaction, lock або deferred target deletion library не додає.

Повторний call повторно викликає callback. Request ID, timeout, caching,
repeat suppression чи idempotency визначає transport/application. Після
timeout operation outcome може бути невідомим; library не виконує retry.
Queueing operation повинна копіювати request до власної memory; Service
не тримає його після return. Status Busy/Failed не rollback вже виконаних
application changes.

## 11. Джерела та runnable приклади

- [ServiceDefinition і factory overloads](../../lib/telemetry/service/Service.hpp).
- [ServiceTable/ServiceEntry та encoding](../../lib/telemetry/service/ServiceTable.hpp).
- [ServiceCatalogTable/ServiceIndex](../../lib/telemetry/service/ServiceCatalogs.hpp).
- [ServiceResult](../../lib/telemetry/result/ServiceResult.hpp),
  [BorrowedServiceResult](../../lib/telemetry/result/BorrowedServiceResult.hpp),
  [BorrowedValue](../../lib/telemetry/result/BorrowedValue.hpp).
- [Native.cpp](../../examples/user_guide/Native.cpp): Query/Snapshot,
  borrowed Settings, plain void, explicit status-only result та borrowed
  response, що посилається на request.
- [Encoded.cpp](../../examples/user_guide/Encoded.cpp): owning/borrowed
  large Report, insufficient scratch без callback та application refusals.
- [Browser payload tools](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/examples/structured_client/README.md): exact
  integer64, unknown enum codes, request editor і payload decoding.
- [Device integration](../../examples/device_integration/README.md):
  ordinary business types за runtime facade та serialized framed dispatch.
- [Fields](Fields.md), [Commands](Commands.md),
  [TransportWalkthrough](TransportWalkthrough.md).

Authors: Ruslan Kovtun (shpegun60), codexAi. [MIT](../../LICENSE).
