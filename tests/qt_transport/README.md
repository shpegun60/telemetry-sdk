# Installed optional Qt transport

This software-only gate builds the SDK's explicitly selected Qt Core and optional
SerialPort modules, installs and relocates them, then makes its prepared SDK and
COBS source trees unavailable. An ordinary `QObject` consumer uses `signals:`,
`slots:` and MOC without inheriting private Qt keyword flags. Serial checks only
construct an unopened transport, inspect bounded profile queries and close it;
no port is opened and no board is accessed.

Fresh downstream projects test explicit client and default package discovery
with Qt disabled, unavailable optional SerialPort, base-only installation, and
the serial module. Public headers and exported CMake files must not expose COBS;
consumer compile commands and Ninja dependencies must not reach hidden source.
The actual ReadMany UART example also builds against only the relocated prefix
with a deliberately missing COBS checkout. Its missing-port argument test rejects
the request before creating an exchange. This is not a physical UART test.

```sh
python3 -B tests/qt_transport/run.py --build-dir /external/qt-transport \
  --cobs-root /path/to/COBS --cxx g++
```

Supply CMake, Ninja and a compiler matching the Qt kit. Use `--qt-prefix` for a
non-system kit and pass explicit `--cmake`/`--ninja` paths where needed. Qt 6.4+
Core and SerialPort are required for this full gate, but portable SDK selection
remains independent of Qt. Logs, source hashes and receipts stay in the fresh
external output directory. The runner deletes nothing and publishes no package.

The relocated `qt_connection` consumer additionally verifies the installed owned
Session linkage, invalid-profile/closed admission, monotonic reservation and
portable consumers from a Qt-enabled package with Qt discovery disabled. Real
TCP/QML lifecycle qualification is in UX `tests/qt_connection` plus the maintained
Inspector TCP suite, both using this same SDK implementation.
