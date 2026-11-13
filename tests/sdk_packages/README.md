# Installed SDK packages

End-user installation commands are in [SDK packages](../../doc/SDKPackages.md).

This offline suite builds only from an external prepared source copy. It relocates
the CMake install, checks same-minor patch selection and strict explicit `EXACT`
requests, packs and installs the
private ESM tarball, and builds/installs the Python wheel through its source archive.
Its simulated 32-bit downstream CMake check accepts the header-only prefix and
refuses a mismatched compiled prefix; it does not execute an MCU binary.
The installed core CLI generates C++/JS/TS with canonical optional runtime assets
after the prepared source has been renamed. Selecting QML without the optional
UX extension must fail clearly before output publication. The full compatibility
package and UX extension have a separate three-wheel integration suite.
No registry publication or GUI is involved.

An external CMake-only fixture installs a newer patch (currently `0.1.7`) from
the real package rules without changing the checkout's `VERSION`. Its relocated
prefix must accept an older requested patch in the same minor and its own exact
version, and reject a newer requested patch, other minor/major versions and an
older `EXACT` request. One accepted older-patch consumer is also built and run.
Installed metadata must retain the fixture package version and unchanged wire
versions; the package selection policy does not negotiate Descriptor or schema
compatibility.

```sh
python3 -B tests/sdk_packages/run.py --build-dir /external/sdk-packages \
  --node /path/to/node --tsc /path/to/typescript/bin/tsc --cxx g++
```

Python 3.10+, setuptools, wheel, pip, venv, CMake, Ninja, a C++20 compiler, Node
with its bundled npm, and TypeScript are supplied by the caller. No tool download
is performed. All source/output hashes and actual commands are retained externally.
