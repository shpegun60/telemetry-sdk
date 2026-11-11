# Optional Qt transports

The SDK owns the reusable transport boundary, Resource discovery and UART
adapter under `lib/integration/qt`. They are independent of QML, GUI models and
Workbench policy. The Inspector and the serial console use these same classes.

Two optional CMake components keep their dependencies explicit:

| Component | Public dependencies | Responsibility |
| --- | --- | --- |
| `qt_transport` | Qt Core, Client and Resource/Encoded wire contracts | `DeviceTransport` signals and sequential `DescriptorBootstrap` |
| `qt_serial_transport` | `qt_transport`, Qt SerialPort | COBS framing, owned staged TX, serial discovery and absolute exchange deadlines |

Both are excluded from the default SDK build. The portable modules never search
for Qt, SerialPort or COBS. Source builds of the serial component require the
existing external COBS checkout, qualified at revision
`2e0abf260848fcb74e4a57b37532188047759643`:

```sh
cmake -S /path/to/telemetry-sdk -B /tmp/sdk-qt -G Ninja \
  -DTELEMETRY_COMPONENTS=qt_serial_transport \
  -DCOBS_ROOT=/path/to/cobs -DCMAKE_INSTALL_PREFIX=/tmp/sdk-prefix
cmake --build /tmp/sdk-qt --target install
```

The SDK compiles the existing COBS encoder/decoder into its serial library and
installs their license. Its public headers and exported include paths expose no
COBS types or source paths. No second framing engine or COBS package is added.

An installed consumer needs only the prefix and a matching Qt installation:

```cmake
find_package(Telemetry 0.1 CONFIG REQUIRED COMPONENTS qt_serial_transport)
target_link_libraries(application PRIVATE Telemetry::qt_serial_transport)
```

```cpp
#include <integration/qt/SerialTransport.hpp>

telemetry::integration::qt::SerialTransport transport;
transport.connectTo(portName, 115200);
```

Use explicit `COMPONENTS` selection so the package can discover only the Qt
modules the consumer needs. A client-only consumer of the same installed prefix
does not need Qt. Normal Qt `signals:` and `slots:` keywords remain available;
the libraries do not export `QT_NO_KEYWORDS` or `QT_NO_FOREACH` definitions.

`DeviceTransport::stage()` copies the offered packet into transport-owned storage.
The owner must authorize `prepareHandoff()` before `sendStaged()` and route
accepted/reply signals through its existing client loop. There is one existing
FIFO; this adapter adds no request scheduler, automatic mutation retry or
exactly-once promise. Discovery obtains owned Descriptor/capability bytes. The
caller validates them and admits its session.

`close()` retires local staged TX, driver state and posted callbacks.
`locallyQuiescent()` describes local transport ownership; it does not prove that
a remote RPC mailbox has closed. Application recovery must still meet the
client/backend's remote closure contract.

`SerialTransport::maxBodyBytes()`, `minBaudRate()`, `maxBaudRate()` and
`exchangeTimeoutMs()` expose the existing bounded profile without exposing COBS.
The canonical Qt-free profile is
[`integration/serial/Profile.hpp`](../lib/integration/serial/Profile.hpp), shared
with the firmware and host tests. Its geometry, CRC and timeout formula are
unchanged. The private profile is not installed as a Qt public header.

For source-based qmake applications, select `telemetry_qt_transport` or
`telemetry_qt_serial_transport` in `CONFIG` and include
[`integration.pri`](../lib/integration/integration.pri). The serial selection
requires `COBS_ROOT` and compiles the same source files into the application.

This extraction changes the former example-only include paths and namespace.
There are no compatibility forwarding headers. Historical hardware receipts
retain their original source identities; moving these sources does not qualify
a newly built firmware on a physical board.
