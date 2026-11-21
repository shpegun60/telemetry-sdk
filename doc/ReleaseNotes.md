# Telemetry release notes

Автори: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

[Документація](README.md) · [Migration guide](StructuredTelemetryV3MigrationGuide.md) ·
[API Map](user/APIOverview.md)

## 2026-11-20: descriptor v3.1 - імена типів, дерево груп, шляхи й handles

[PR #65](https://github.com/shpegun60/telemetry-sdk/pull/65) додає descriptor
v3.1 як адитивний контракт поверх замороженого v3.0.

- **Імена типів, увімкнені за замовчуванням.** Кожен enum і struct у
  descriptor отримує ім'я з коду: найближчий scope і ім'я типу (`adc::Config`).
  `TypeReflection<T>` з `typeName("...")` замінює ім'я, з `noTypeName()`
  прибирає його. Перемикач `TELEMETRY_TYPE_NAMES` (типово 1) живе в
  [Config.hpp](../lib/telemetry/Config.hpp) разом з іншими перемикачами SDK;
  проєкт задає його у власному `telemetry_config.hpp` або через `-D`.
  [Fields: імена типів](user/Fields.md#8-імена-типів-у-descriptor).
- **Дерево груп.** `ts::group(name, items...)` змішує subgroups, Fields,
  Commands і Services; `Model{adc::tlm::branch, metrology::tlm::branch}`
  монтує файли гілок. Dispatch лишається O(1), structured ABI revision 6.
- **Шляхи й named handles.** Один запис шляху `group/.../endpoint.member[i]`
  для пристрою, hosts і генератора. На пристрої
  `inline constexpr auto gain1 = telemetry::endpoint<model, "adc/channel1/Gain">;`
  розв'язує шлях під час компіляції; далі `gain1.read()`, `gain1.write(2.5f)`,
  `reset.call()`. Handle компілюється в той самий код, що `makeId`; вільні
  `telemetry::read/write/call/idOf<model, "path">()` лишаються.
  [Model: group tree](user/Model.md#group-tree-модель-із-файлів-гілок),
  [приклад](../examples/group_tree/README.md).
- **Hosts і генератор.** `RuntimeSchema`, `web/telemetry.js` і Python parser
  читають імена, attribute area і дерево; генератор дає іменовані типи,
  namespaces груп і handles у generated та Dynamic clients
  ([GeneratedNamesAndHandles](GeneratedNamesAndHandles.md)).

### Сумісність: старі hosts і новий пристрій

Пристрій, перезібраний з цим SDK, надсилає descriptor v3.1, щойно в ньому є
enum або struct (дерево груп - завжди). **Hosts, старші за цей SDK, відмовляють такому пристрою**
(descriptor minor 1). Виберіть одне:

1. перезберіть host із цим SDK;
2. перегенеруйте static SDK з нового descriptor: код користувача
   компілюється без змін, бо кожен наявний generated symbol зберігає ім'я та
   signature;
3. поставте `#define TELEMETRY_TYPE_NAMES 0` у `telemetry_config.hpp`
   пристрою: descriptor плоскої моделі тоді побайтово дорівнює v3.0,
   fingerprint не змінюється.

Без змін лишаються `Model{fields, commands, services}`, tables,
`ts::group(name, table)`, `makeId`, typed `fields.read<id>()` і generated
symbols (`detail::Type_N` включно). Плоска модель без enums і structs і далі
надсилає v3.0. Values лишається 3.0. Fingerprint пристрою з enum чи struct
змінюється один раз, тому записані сесії, прив'язані до fingerprint,
починаються заново. Значення `TELEMETRY_TYPE_NAMES` входить у structured ABI
tag: перезберіть усі object files SDK з однаковими перемикачами.
[Migration guide](StructuredTelemetryV3MigrationGuide.md#descriptor-v31-імена-типів-і-дерево-груп)
описує кроки.

## 2026-10-11: owning result pointer lifetime

[`5d2bb93`](https://github.com/shpegun60/telemetry/commit/5d2bb937880cb5c47da5399420469803d5ecf8ac)
посилює compile-time контракт трьох owning wrappers:
`ServiceResult<T>`, `NativeCallResult<T>` і `FieldReadResult<T>`.
`valueOrNull()` тепер доступний лише для `&` і `const&`; `&&` та `const&&`
видалено. Те саме стосується `FieldReadResult::operator->()`.
Збережіть result у змінну перед отриманням pointer. Навіть короткий
`makeFieldResult()->member` тепер відхиляється: він використовує той самий
accessor, що й небезпечне збереження pointer з temporary.

`value() &&` залишається для move-out. Посилання, отримане з temporary
через `value()` чи `operator*()`, не подовжує lifetime wrapper; переносіть
саме значення або тримайте owning result живим. Borrowed wrappers посилаються
на зовнішній об'єкт і в цьому коміті не змінюються.
Повний [контракт результатів](user/NativeApi.md#16-borrowed-lifetime-і-узгоджені-дані)
та [тести](../tests/ergonomics/README.md#owning-result-pointer-lifetime)
описують ці відмінності.

### Повна чиста перебудова consumer

Після оновлення **повністю перезберіть усі translation units та static/shared
libraries consumer з поточними headers і однаковими configuration macros**.
Включіть dependent projects, precompiled headers і C++ plugins, якщо вони
використовують ці templates. Старі object files не слід змішувати з новими.
Стандартна clean/rebuild дія вашої build system має прибрати попередні objects;
ручний incremental link не є перевіркою сумісності.

Ref-qualifier входить у тип pointer-to-member-function. Для коду, який явно
задає цей тип, додайте `&` або `const&`, наприклад:

```cpp
using Result = telemetry::FieldReadResult<Config>;
using MutableAccessor = Config* (Result::*)() & noexcept;
using ConstAccessor = const Config* (Result::*)() const& noexcept;

const auto mutableAccessor = static_cast<MutableAccessor>(&Result::valueOrNull);
const auto constAccessor = static_cast<ConstAccessor>(&Result::valueOrNull);
```

Ref-qualifier також змінює C++ mangled symbol, коли compiler породжує
out-of-line method. Зміна не дає універсальної binary ABI compatibility зі
старими precompiled C++ objects, навіть якщо layout wrapper не змінився.
Незмінність перевірених ARM instruction/stack probes і дванадцяти firmware
`.bin` означає незмінність саме тих [перевірених образів](../tests/resources/evidence/README.md),
а не всіх можливих consumer builds.

Accessor bodies, member layout, canonical payload bytes і wire v3.0 не
змінено цим комітом; нових runtime перевірок не додано. Clean rebuild потрібний
через C++ source/type/symbol контракт. Після нього виконайте свої application
тести; qualification бібліотеки не замінює перевірку вашої прошивки.

## 2026-10-11: навігація публічним API

[`535e550`](https://github.com/shpegun60/telemetry/commit/535e550a5675b5dd4a995b2a933075dae056f653)
додає [API Map](user/APIOverview.md) і
[NativeOnly](../examples/user_guide/NativeOnly.cpp), а довідники розділяють
Native / Encoded / Codec / Resource сценарії без нового runtime API.
Documentation-only частина цього зрізу не змінює library headers.

## 2026-10-11: audit follow-up

Уточнено disjoint-output contract DescriptorFile та числовий/raw-bit контракт
JS для NaN. Перевірка Markdown file/heading links тепер запускається в CI;
додано bounded input corpora для resource, encoded payload і JS boundaries.
Production dispatch/codec не змінено. Повні
[результати та нові H7S receipts](evidence/audit-2026-10-11/README.md)
розділяють host checks, offline compiler gates і шість реально виконаних образів.
