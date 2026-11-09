# Runnable examples

Author: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

Choose an example here before selecting its build project. The repository's
root `CMakeLists.txt` builds and installs the library. Ready-to-run windows live
in Telemetry UX; the Universal Inspector has its own CMake project in
`telemetry-ux/examples/inspector`. Application-specific Qt windows do not belong
to the SDK.

Run commands in each linked README from the Telemetry repository root unless
that README explicitly changes directory. Put generated SDKs, build directories
and test receipts outside the source checkout. Qt, the STM32 SDK and COBS are
required only by the examples that explicitly select them.

## Start with a complete program

| Task | Example and build entry point |
| --- | --- |
| Connect a browser to the reference WebSocket server with an owned Session | [Connection Session](connection_session/README.md), no GUI package required |
| Local Native Field, Command and Service | [User-guide programs](user_guide/README.md); start with `NativeOnly.cpp`, no Qt or transport |
| Integrate your device class and receive complete requests | [Device integration](device_integration/README.md), `device_integration/CMakeLists.txt` |
| Inspect or transactionally edit unknown DTO bytes offline | [Runtime schema](runtime_schema/README.md); standalone reader/writer commands |
| Call unknown runtime endpoints | [DynamicClient](dynamic_client/README.md), `dynamic_client/CMakeLists.txt` |
| Write, execute Commands/Services and read in one ordered request | [Mixed Batch](batch/README.md), `batch/CMakeLists.txt` |
| Read 1–32 selected scalar Fields in one Encoded operation | [ReadMany](read_many/README.md), `read_many/CMakeLists.txt` |
| Compare individual, aggregate and Resource reads before ReadMany | [Read comparison](read_comparison/README.md), `read_comparison/CMakeLists.txt` |
| Use a portable read-only model without Qt | [InspectorModel](inspector/README.md), `inspector/CMakeLists.txt` |
| Open the Universal Inspector window | [Qt/QML Inspector](https://github.com/shpegun60/telemetry-ux/blob/main/examples/inspector/README.md), `telemetry-ux/examples/inspector/CMakeLists.txt` |
| Edit a Field and read back its applied value | [FieldWrite Workbench](https://github.com/shpegun60/telemetry-ux/blob/main/examples/inspector/README.md#read-edit-write-and-refresh), same Qt/QML project; choose Writable demo |
| Connect that window to NUCLEO-H7S3L8 over UART/DMA | [Board firmware](nucleo_h7s3l8/README.md), `nucleo_h7s3l8/Build.ps1`; enable serial in the Inspector project |

The Inspector starts with an in-process Demo device. TCP can also be exercised
with its loopback demo server. UART requires Qt SerialPort, the documented COBS
revision and separately built device firmware. Importing a Descriptor file alone
provides metadata browsing, not live device values.

## Generated SDKs and clients

| Example | What to build |
| --- | --- |
| [Generated C++](codegen_cpp/README.md) | `codegen_cpp/CMakeLists.txt` generates the SDK and builds the standalone, Encoded and RPC programs |
| [Generated Encoded](codegen_encoded/README.md) | Build the `codegen_encoded` target through the C++ project above |
| [Generated RPC](codegen_rpc/README.md) | Build the `codegen_rpc` target through the C++ project above |
| [Generated Qt/QML](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/examples/codegen_qml/README.md) | Telemetry UX example over an installed SDK; a device-specific generated API, distinct from the Universal Inspector |
| [Generated web](codegen_web/README.md) | Follow its SDK generation and Node/browser commands |
| [Portable async client](async_client/README.md) | Bare-metal owner loop and FreeRTOS port examples; application supplies the backend and transport |
| [JS/TS async client](client_web/README.md) | Shared runtime, typed callbacks and application transport |

The [generator](../tools/README.md) is a Python host tool. Native API and a typed
client with an application Codec do not require a Generated SDK.

## Resource and transport integration

| Example | Purpose |
| --- | --- |
| [Resource providers](resources/README.md) | Generic custom files and providers |
| [COBS](cobs_integration/README.md) | Real COBS framing/pools on a simulated host port |
| [RPC mailbox](rpc_mailbox/README.md) | Server request/result lifecycle through Resource |
| [RPC client](rpc_client/README.md) | Low-level client packet engine and host peer |
| [RPC sessions](rpc_multi_session/README.md) | Multiple independent server mailbox sessions |
| [Structured native fixture](structured_client/README.md) | Native DTOs and a Qt Core console payload smoke |
| [Structured browser payload tools](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/examples/structured_client/README.md) | Telemetry UX over installed C++ and npm SDK packages |
| [Bind/Exchange](structured_protocol/README.md) | Optional application-owned agreement example |
| [Shared serial profile](../lib/integration/serial/README.md) | SDK header used by both Inspector and Nucleo; not a separate executable |

Reusable transport-neutral adapters belong to [Integration](../lib/integration/README.md).
The [experimental ReadMany comparison](read_many_mapping/README.md) measures
compact frames against an ordinary padded Service; it is not a production client API.
Qt sockets, GUI objects and board HAL configuration stay in their application
examples. See [modules](../doc/Modules.md) for CMake/qmake selection and
[maintained tests](../tests/README.md) for qualification commands.
