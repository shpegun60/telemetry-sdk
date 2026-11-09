# Документація Telemetry

[Репозиторій](../README.md) · [Посібник](user/README.md) ·
[API шпаргалка](user/API-CHEATSHEET.md) · [Архітектура](Architecture.md) ·
[Runtime native та обхід](user/Ergonomics.md) ·
[Command/Service: один результат](user/FlatNative.md) ·
[Wire v3.0](WireV3.md) · [Тести](../tests/README.md)

[Генератор C++/TS/JS SDK](CodeGenerator.md) ·
[Generated C++](GeneratedCppApi.md) · [Generated JS/TS](GeneratedTsJsApi.md) ·
[Generated Qt/QML](GeneratedQmlApi.md) ·
[Schema compatibility](GeneratedSchemaCompatibility.md) ·
[Asynchronous client](AsyncClient.md) ·
[Runtime schema and values](RuntimeSchema.md) ·
[DynamicClient contract](DynamicClient.md) · [Runtime client usage](DynamicClientUsage.md) ·
[Read-only Inspector Model](InspectorModel.md) ·
[FieldWrite, Command and Service Workbench](Workbench.md) ·
[Read comparison before ReadMany](ReadComparison.md) ·
[ReadMany design](ReadMany.md) · [Production C++ Encoded ReadMany](ReadManyEncoded.md) ·
[Typed C++ ReadMany](StaticReadMany.md) ·
[JS/TS ReadMany](JsReadMany.md) ·
[JS/TS Mixed Batch](JsBatch.md) ·
[Dynamic JS/TS client](DynamicJsClient.md) ·
[JS/TS connection Session](ConnectionSession.md) ·
[Simpler host API: reviewed plan and acceptance](HostApiRoadmap.md) ·
[Optional structural metadata](GeneratedMetadata.md) ·
[Installed SDK packages](SDKPackages.md) ·
[Optional UX generator](UXGeneratorExtension.md) ·
[Optional Qt transport components](QtTransport.md) ·
[Typed C++ Mixed Batch](StaticBatch.md) ·
[ReadMany mapping prototype](ReadManyMapping.md) ·
[Mixed Encoded Batch](MixedBatchEncoded.md) ·
[Mixed Batch host example](../examples/batch/README.md) ·
[Qt/QML scalar Watchlist](ReadManyWatchlist.md) ·
[Qt/QML Inspector example](https://github.com/shpegun60/telemetry-ux/blob/main/examples/inspector/README.md) ·
[NUCLEO-H7S3L8 UART/DMA](../examples/nucleo_h7s3l8/README.md) ·
[SDK and UX roadmap](SdkUxRoadmap.md) ·
[Усі приклади та build entry points](../examples/README.md)

[Optional Integration adapters](../lib/integration/README.md) поєднують незалежні
модулі. Inspector використовує Resource для завантаження Descriptor та Encoded
для live FieldRead; TCP і UART є окремими транспортами застосунку.

**[API Map: Native / Encoded / Codec / Resource](user/APIOverview.md)** —
короткий вибір сценарію з headers, залежностями, довідниками й runnable examples.

Цей індекс описує чинний C++20 `telemetry` і wire v3.0. Для першої
інтеграції почніть із [GettingStarted](user/GettingStarted.md); повні
програми мають окремі runnable source files. Таблиці нижче ведуть до
документа, який володіє конкретною темою.

## Навігація

- [Підключення за кроками](#підключення-за-кроками)
- [Довідник за завданням](#довідник-за-завданням)
- [Повні приклади](#повні-приклади)
- [Модулі та контракти](#модулі-та-контракти)
- [Перевірки та вимірювання](#перевірки-та-вимірювання)
- [Підтримка документації й архів](#підтримка-документації-й-архів)

## Підключення за кроками

| Крок | Документ | Результат |
| --- | --- | --- |
| 1 | [Перша інтеграція](user/GettingStarted.md) | Source/include settings, свій Device, declarations і native виклик |
| 2 | [Application integration](user/ApplicationIntegration.md) | Валідація, facade, lifetimes, slots і узгоджений доступ до state |
| 3 | [Transport walkthrough](user/TransportWalkthrough.md) | Receive chunks → complete packet → operation → reply |
| 4 | [Device integration](../examples/device_integration/README.md) | Multi-file проєкт із CMake та host перевірками |
| 5 | [Діагностика](user/Troubleshooting.md) | Рішення compiler/linker, type, ID, status, Workspace і framing питань |

Для local typed доступу достатньо кроків 1–2. Для віддаленого доступу
додайте transport framing і encoded adapter; file interface — окремий вибір.
[User guide](user/README.md) порівнює ці шляхи на одній схемі.

## Довідник за завданням

| Потрібно | Документ |
| --- | --- |
| Згадати declarations та назви викликів | [API шпаргалка](user/API-CHEATSHEET.md) |
| Getter/setter і конкретні Field operations | [Fields](user/Fields.md) |
| Request та результат application дії | [Commands](user/Commands.md) |
| Request/Response, Service statuses і reply payload | [Services](user/Services.md) |
| Local tables, named groups, packed IDs та runtime indexes | [Tables and catalogs](user/TablesAndCatalogs.md) |
| Structural TypeRegistry, ModelView та всі runtime indexes | [Model](user/Model.md) |
| Великі catalogs, unique DTOs, visitor code size та module dependencies | [Масштабування](Scalability.md), [reproducible checks](../tests/scalability/README.md) |
| Canonical codec, Workspace/Lease, scratch та overlap | [Codec and Workspace](user/CodecAndWorkspace.md) |
| Всі callback/binding forms, exact/As доступ, enums, iteration і slots | [Native API](user/NativeApi.md) |
| Custom files, providers і cursors | [Resource files](user/Resources.md) |
| Model descriptor, Values, fingerprint і whole-token READ | [Descriptor and Values](user/DescriptorAndValues.md) |
| LIST/STAT/READ/WRITE, framing та transport integration | [Transport and resources](user/TransportAndResources.md) |
| Typed remote виклики, retry та caller-owned result decode | [RpcClient v1](RpcClientV1.md) |
| N mailbox, session handles, ACK gate і спільний Workspace | [RpcSessionPool v1](RpcSessionsV1.md) |
| З'єднати telemetry resources і реальний COBS endpoint | [COBS integration](user/COBSIntegration.md) |
| Повернути `const T&` без owning copy і забезпечити lifetime | [Borrowed native values](BorrowedNativeValues.md) |
| Зрозуміти межі Model, codec, providers і transport | [Архітектура](Architecture.md) |
| Реалізувати незалежний descriptor/Values decoder | [Wire v3.0](WireV3.md) |
| Перенести consumer з попереднього API | [Migration guide](StructuredTelemetryV3MigrationGuide.md) |
| Оновити headers, accessor types та повністю перезібрати consumer | [Release notes](ReleaseNotes.md) |
| Зібрати або перевірити конкретний приклад | [Example checks](../tests/docs/README.md), [test index](../tests/README.md) |

## Повні приклади

| Приклад | Що демонструє |
| --- | --- |
| [User-guide programs](../examples/user_guide/README.md) | QuickStart, Native, custom Resources і encoded доступ без файлів |
| [Device integration](../examples/device_integration/README.md) | Business class, public facade, tables у `.cpp` та bounded stream receiver |
| [COBS integration](../examples/cobs_integration/README.md) | Реальний sibling COBS endpoint, client/server resource exchange, TX backpressure і recovery на host |
| [Custom resources](../examples/resources/README.md) | Додати providers із власними paths |
| [RPC mailbox](../examples/rpc_mailbox/README.md) | RPC через file table, зовнішній connectionGeneration, ACK до dispatch та checked reply |
| [Typed RPC client](../examples/rpc_client/README.md) | Begin/decode API, реальні Resource packets і втрачений ACK |
| [Multi-session RPC](../examples/rpc_multi_session/README.md) | Незалежні mailbox-и, generation/release, спільний Model та serialized Workspace |
| [Structured client](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/examples/structured_client/README.md) | Installed SDK payload tools in Telemetry UX |
| [Optional Bind/Exchange](../examples/structured_protocol/README.md) | Descriptor agreement і control envelope, якими володіє application |

Fenced fragments пояснюють окремий крок; повну програму для збирання
беріть із linked translation unit чи example project. Host receiver
і simulated Device не є виконанням HAL або transport на MCU.

## Модулі та контракти

| Модуль/контракт | Технічний довідник |
| --- | --- |
| Native endpoints, TypeRegistry і Model | [Telemetry](../lib/telemetry/README.md) |
| Late binding | [Slots](../lib/telemetry/slot/README.md) |
| Незалежний filesystem/providers | [Resource](../lib/resource/README.md) |
| Packet processor | [Resource protocol](../lib/resource/protocol/README.md) |
| Optional RPC provider: wire v1.0, повтори, стани й сеанс | [RpcMailbox contract](RpcMailboxV1.md), [host example](../examples/rpc_mailbox/README.md) |
| Optional typed caller і server session manager | [RpcClient](RpcClientV1.md), [RpcSessionPool](RpcSessionsV1.md) |
| Descriptor/Values provider lifetime і storage | [V3 providers](../lib/integration/telemetry/v3/README.md) |
| Exact wire constants, default ceilings і goldens | [Wire v3.0](WireV3.md), [freeze manifest](../tests/structured/freeze/contract.json) |
| JS native payload codec та parser | [Client API](../tests/structured/client/README.md) |

## Перевірки та вимірювання

[Аудит бібліотеки 2026-11-02](evidence/library-audit-2026-11-02/README.md):
покриття файлів, відтворені дефекти, виправлення, перевірки та виміряна вартість.

[Повний повторний перегляд рядків бібліотеки](evidence/library-line-review-2026-11-02/README.md):
221 файл `lib`, генератор, Qt/QML, точні діапазони та сім адресних виправлень.

Почніть із [test index](../tests/README.md): host execution, sanitizers,
ARM compile/link/disassembly та H7S execution — окремі види доказів.
Кожний receipt/report має власні captured inputs і source identity.
Успіх старого checkpoint не доводить статус нового commit.

| Доказ | Що в ньому шукати |
| --- | --- |
| [Qualification suite index](../tests/structured/README.md) | Підтримувані suites та команда для потрібного контракту |
| [Borrowed software/ARM checks](../tests/structured/borrowed/README.md) | Result/lifetime refusals, storage budgets, ABI та codegen gates |
| [Borrowed H7S harness](../tests/structured/borrowed/h7s/README.md), [results](../tests/structured/borrowed/h7s/RESULTS.md) | Фактичні owning/borrowed comparison profiles, cycles, observed stack і їхні межі |
| [MCU harness](../tests/structured/mcu/README.md), [H7S contract](../tests/structured/mcu/h7s/README.md), [results](../tests/structured/mcu/h7s/RESULTS.md) | Mixed/Scale scenarios, captured source, image bounds і повне Flash restore/readback |
| [Descriptor](../tests/structured/descriptor/h7s/README.md), [Values](../tests/structured/resources/h7s/README.md), [Exchange](../tests/structured/exchange/h7s/README.md) | Окремі H7S receipts для відповідних consumers |
| [Resource image equivalence](../tests/resources/evidence/README.md) | File-only звірка з retained images; це не новий hardware run |
| [RpcClient / sessions qualification](RpcExtensionsQualification.md) | API, фінальна host/ARM матриця, пам'ять та незмінність попередніх firmware bytes |
| [Stage 15 freeze checkpoint](StructuredTelemetryV3FreezeQualification.md) | Історичний baseline contract і його exact-SHA verification |
| [Stage 20 qualification checkpoint](StructuredTelemetryV3FinalQualification.md) | Закритий migration baseline, тодішні software/H7S inputs і publication gate |
| [Pre-borrowed receipt archive](evidence/pre-borrowed/README.md) | Exact попередні receipts/validators, відокремлені від оновлених input sets |
| [Pre-unification evidence](evidence/pre-unification/README.md) | Збережені попередні measurements із незмінними source identities |

## Підтримка документації й архів

README репозиторію є landing page; цей індекс маршрутизує читача;
user guide пояснює використання; module guides і wire reference володіють
контрактами. При зміні API оновіть relevant guide та runnable example
разом і виконайте відповідний runner із [test index](../tests/README.md).

Скасований scalar-план, завершений implementation plan, старі review,
транскрипти та exploratory probes зберігаються поза активним checkout.
Точний inventory, архів і hash verification описані у
[Repository maintenance](RepositoryMaintenance.md). Збережені receipts
і вимірювання залишаються у своїх evidence directories з початковими
source identities. Вони доступні для перевірки походження результатів;
чинний API визначають довідники вище.

[SDK + UX source audit](evidence/sdk-ux-audit-2026-11-05/README.md) ? source coverage, confirmed repairs and qualification limits.
