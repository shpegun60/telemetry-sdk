# Structured native fixture and Qt Core console

Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.

This SDK example retains the application DTO/table fixture in [Device.hpp](Device.hpp)
and the headless Qt Core binary-payload smoke in [QtSmoke.cpp](QtSmoke.cpp).
QByteArray owns its wire buffers; Workspace owns decoded object lifetimes. It
checks the native Model, encoded Service, U64/S64 extremes and exact 41-byte
response. Qt JSON is only a desktop summary with integer64 strings, not an MCU
formatter or a new Telemetry format.

The fixture's descriptor is v3.1: its enum and structs carry the type names
taken from their code, `client_example::Mode`, `client_example::Config`,
`client_example::Request` and `client_example::Response`, with nothing written
for them. `#define TELEMETRY_TYPE_NAMES 0` in a project `telemetry_config.hpp`
would leave them out; see
[Fields: type names](../../doc/user/Fields.md#8-імена-типів-у-descriptor).

The HTML editor, localhost adapter and real-browser tests now belong to
[Telemetry UX payload tools](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/examples/structured_client/README.md), with an independent
[installed-package browser suite](https://github.com/shpegun60/telemetry-ux/blob/a336b8c2984d5adf235b0791b545a37d3da3cc8b/tests/structured_client/README.md). That example imports the installed
SDK npm runtime and links installed SDK C++ targets. Starting it does not require
running a test suite or accessing the SDK source tree.

## Qt Core smoke

From this SDK checkout, with a matching Qt 6 C++20 kit:

```sh
mkdir -p build/client-qt
cd build/client-qt
qmake6 ../../examples/structured_client/qt.pro CONFIG+=release QMAKE_CXXFLAGS+=-Werror
make -j2
./structured_client_qt
```

On Windows use the matching kit's qmake/make and `.exe` suffix; put that MinGW
runtime directory first on PATH. This is a console program and creates no GUI.

## Headless payload interoperability

The SDK [structural client suite](../../tests/structured/client/README.md) keeps
the independent native producer, canonical Descriptor/Values and request/response
bytes, exhaustive JS parser/codec checks and application-status controls. Its
current command requires neither Qt nor Playwright:

```sh
python3 tests/structured/client/run.py --cxx g++ --node node --build-dir build/client
```

The old `--browser` switch produces an actionable migration error before creating
an output directory or running tools. The new browser suite has its own explicit
installed C++/npm SDK inputs; it is not silently substituted into this runner.

The local [Device.hpp](Device.hpp) remains the shared SDK test fixture used by
headless generator/codec tests. The UX demo owns its separate application fixture
with recorded provenance; no SDK client, parser, codec or FIFO implementation is copied.
