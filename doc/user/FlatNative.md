# Runtime Command/Service: один результат

[API Overview](APIOverview.md) · [Посібник](README.md) · [Commands](Commands.md) · [Services](Services.md) ·
[Повна runnable програма](../../examples/user_guide/FlatNative.cpp)

> **Одна форма — дерево груп, шляхи й handles.** У Model із груп той самий
> endpoint оголошують усередині `ts::group(...)` і викликають за шляхом:
> `ts::call<model, "adc/Reset">()` і `ts::call<model, "adc/Calibrate">(request)`. Шлях однаковий на пристрої, у Static C++, TS/JS і Dynamic
> клієнті: [одна операція в чотирьох місцях](APIOverview.md#одна-операція-в-чотирьох-місцях).
> Плоску форму з catalog tables, публічний `makeId` і typed доступ за ID
> [прибрано](APIOverview.md#прибрано): runtime ID обробляють `model.fields()`,
> `model.commands()` і `model.services()`, а catalogs позначають саме їх. Signatures,
> results і lifetime rules самих Field, Command і Service від форми Model не залежать.

Коли ID відомий лише під час виконання, а native Request/Response відомі в C++,
використовуйте ці форми. Visitor, подвійні `value()` та окремий routing result
для звичайного виклику не потрібні.

```cpp
auto response = services.callAs<Reply>(id, request);
if (response)
    use(response.value());
else
    log(response.status());

auto config = services.callBorrowed<Config>(configId);
if (config)
    use(config.value());

auto status = commands.call(commandId, SetPeriod{100});
```

Це fragments; declarations, callbacks та assertions зібрані в програмі вище.
Public include: `<telemetry/Telemetry.hpp>`. Endpoint ID має ті самі правила:
local table приймає position, global catalogs — packed `u32` ID.
Model і Workspace objects для цього native API не потрібні. Мінімальні
local declarations наведені в
[NativeOnly.cpp](../../examples/user_guide/NativeOnly.cpp); public headers і
чинні compile-time type/reflection dependencies — у [Native reference](NativeApi.md).

## Форми виклику

| Завдання | Local table | Global catalogs | Результат |
| --- | --- | --- | --- |
| Command з Request | `call(position, request)` | `call(id, request)` | `CommandCallStatus` |
| Command без Request | `call(position)` | `call(id)` | `CommandCallStatus` |
| Owning Service | `callAs<Reply>(position, request)` | `callAs<Reply>(id, request)` | `ServiceCallResult<Reply>` |
| Owning Service без Request | `callAs<Reply>(position)` | `callAs<Reply>(id)` | `ServiceCallResult<Reply>` |
| Borrowed Service | `callBorrowed<Reply>(position, request)` | `callBorrowed<Reply>(id, request)` | `BorrowedServiceCallResult<Reply>` |
| Borrowed без Request | `callBorrowed<Reply>(position)` | `callBorrowed<Reply>(id)` | `BorrowedServiceCallResult<Reply>` |
| Service без response payload | `callAs<void>(position[, request])` | `callAs<void>(id[, request])` | `ServiceCallResult<void>` |

Service Response — exact unqualified aggregate struct або `void`, відповідно
до [правил Service](Services.md). Request також exact: схожі structs не
взаємозамінні, implicit numeric conversion відсутня. Неправильний Request,
Response або ownership повертає `SignatureMismatch` до callback. Borrowed
`void`, raw pointers, cv-qualified та reference template arguments не підтримуються.
Від'ємні та wide IDs перевіряються до звуження; invalid/absent ID дає `NotFound`.
Runtime calls вимагають lvalue table/catalog.

## Service result

| Метод | Owning | Borrowed |
| --- | --- | --- |
| `status()` | `ServiceCallStatus` | `ServiceCallStatus` |
| `hasValue()`, explicit `bool` | Routing **і** application success | Routing success **і** present successful const view |
| `value()` | `T&`, `const T&`, `T&&`, `const T&&` за category result | `const T&` |
| `valueOrNull()` | `T*` для `&`, `const T*` для `const&`; null на failure; `&&` / `const&&` видалені | `const T*`, null на failure; temporary view допустимий за зовнішнім lifetime contract |
| `value_type` | `T` | `T` |

`value()` вимагає успішного result; порушення precondition завершується через
`abort`. Для `void` є `value()`, що перевіряє
success, але не створює об'єкта; `valueOrNull()` відсутній. `hasValue()` для
`void` означає успішне завершення операції, а не наявність штучного payload.

| `ServiceCallStatus` | Причина | Callback |
| --- | --- | --- |
| `Ok` | Endpoint знайдено, exact signature, application success | Виклик або успішне розв'язання binding |
| `NotFound` | Неприпустимий чи відсутній ID | Не викликається |
| `SignatureMismatch` | Request/Response/ownership не збігається | Не викликається |
| `Unavailable` | Empty compatible slot або application відмова | Може бути відсутній |
| `Busy` | Application зараз зайнята | Application outcome |
| `InvalidArgument` | Application відхилила request | Application outcome |
| `Failed` | Application operation не вдалася | Application outcome |

`Busy`, `Unavailable`, `InvalidArgument`, `Failed` завжди мають `bool == false`
та `hasValue() == false`. Вихідні application statuses не переписуються;
фасад лише показує спільний native outcome. Ці enum values не є wire-кодами.

## Command status

`commands.call(id, request)` повертає enum `CommandCallStatus`:

| Status | Meaning |
| --- | --- |
| `Executed` | Application завершила дію |
| `Accepted` | Application прийняла роботу; завершення ще не встановлено |
| `NotFound` | ID не знайдено або callback повернув application NotFound |
| `SignatureMismatch` | Request shape/no-request форма не збігається; callback не викликається |
| `Unavailable` | Binding/slot unavailable або application відмова |
| `ArgumentCountMismatch` | Callback повернув відповідний application outcome |
| `InvalidValue`, `Busy`, `Failed` | Відповідні application outcomes |

Невизначений numeric `CommandResult` у новій flat формі дає `Failed`.
Старий low-level `callAs` зберігає raw application code. `CommandResult`,
його numeric values та encoded statuses залишаються незмінними. Flat enum
не серіалізується автоматично й не встановлює completion для `Accepted`.

## Пам'ять і lifetime

Фасад зберігає наявний nested result у своєму final member storage.
`fromNative(factory)` конструює його без проміжного owning response та без
додаткового move/copy `T`. Сам owning result усе одно містить `sizeof(T)`
даних: caller обирає місце для великого result. Звичайна automatic variable
може зайняти стільки stack; encoded Workspace policy не застосовується до
native result. Callback для великого owning response продовжує використовувати
`ServiceResult<T>::successFrom(factory)`.

Pointer із owning `ServiceCallResult<T>` можна отримати лише з lvalue result.
Збережіть result в іменованій змінній і тримайте його живим до останнього
використання pointer:

```cpp
auto response = services.callAs<Reply>(id, request);
const Reply* reply = response.valueOrNull();
if (reply)
    use(*reply); // response ще живий.
```

`services.callAs<Reply>(id, request).valueOrNull()` не компілюється: `&&` та
`const&&` overloads явно видалені. Це стосується також immediate-use expression,
який раніше міг укластись у lifetime temporary. Заборона є перевіркою source
contract, без runtime checks та без зміни storage/layout. Вона не продовжує
lifetime іменованого result: pointer стає недійсним після його знищення або
втрати payload. Для owning move-out `std::move(response).value()` лишається
доступним; `value() &&` та `value() const&&` не змінені.

Borrowed фасад тримає лише view/status і не копіює, не переміщує та не
знищує `T`. Owner і borrowed response повинні жити до останнього використання
view; synchronization лишається application responsibility. View не є snapshot.
Якщо callback повертає reference на Request, сам Request теж має жити достатньо
довго: використовуйте named request замість temporary для persisted view.
`services.callBorrowed<Config>(configId).valueOrNull()` лишається допустимим:
знищення temporary wrapper не знищує зовнішній `Config`. Pointer можна
використовувати лише доки backing object живий і доступ до нього синхронізовано.

Обидві форми використовують наявний indexed O(1) dispatch. Навіть однакові за
формою DTO не означають однаковий C++ тип. Descriptor, TypeIds, fingerprint,
wire v3 та старі static/encoded paths не змінюються.

## Детальний API лишається

```cpp
auto detailed = services.callAs<telemetry::ServiceResult<Reply>>(id, request);
if (detailed.status() == telemetry::NativeCallStatus::Ok) {
    const auto applicationStatus = detailed.value().status();
    // Routing і application outcomes доступні окремо.
}
```

`callAs<BorrowedServiceResult<T>>()`, Command `callAs(id[, request])`, static
`call<Id>()`, `NativeCallResult`, `ServiceResult` і `BorrowedServiceResult`
зберігають routing, application statuses та ownership. Flat overloads не
конвертують Result у payload: exact wrapper і payload overload вибираються
однозначно за типом.

Owning wrappers `ServiceResult<T>`, `NativeCallResult<T>` і `FieldReadResult<T>`
мають такий самий lvalue-only `valueOrNull()` guard: `&&` / `const&&` видалені.
Owning Field `operator->()` також недоступний на rvalue result. Guard навмисно
відхиляє й immediate-use temporary pointer access, без runtime checks чи
storage/layout change. Для named result lifetime payload усе ще обов'язковий.
`value()` та Field `operator*()` зберігають попередні return categories й
mutable rvalue move-out; reference із них не подовжує lifetime result.
Outer `NativeCallResult<BorrowedServiceResult<T>>::valueOrNull()` вказує на
owned inner wrapper, а не на external T. Два pointer levels та приклади
описані у [Services](Services.md#low-level-exact-runtime-callas).

[Flat runner](../../tests/ergonomics/README.md) перевіряє status mapping,
ownership, lifetime, великі response і old/new API compatibility. ARM
compile/disassembly та individual frames відділені від device cycle measurements.

Authors: Ruslan Kovtun (shpegun60), codexAi. [MIT](../../LICENSE).
