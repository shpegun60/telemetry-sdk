# Installed SDK packages

End-user installation commands are in [SDK packages](../../doc/SDKPackages.md).

This offline suite builds only from an external prepared source copy. It relocates
the CMake install, accepts the same minor version and refuses the next minor and an
unrelated version, packs and installs the
private ESM tarball, and builds/installs the Python wheel through its source archive.
Its simulated 32-bit downstream CMake check accepts the header-only prefix and
refuses a mismatched compiled prefix; it does not execute an MCU binary.
The installed core CLI generates C++/JS/TS with canonical optional runtime assets
after the prepared source has been renamed. Selecting QML without the optional
UX extension must fail clearly before output publication. The full compatibility
package and UX extension have a separate three-wheel integration suite.
No registry publication or GUI is involved.

```sh
python3 -B tests/sdk_packages/run.py --build-dir /external/sdk-packages \
  --node /path/to/node --tsc /path/to/typescript/bin/tsc --cxx g++
```

Python 3.10+, setuptools, wheel, pip, venv, CMake, Ninja, a C++20 compiler, Node
with its bundled npm, and TypeScript are supplied by the caller. No tool download
is performed. All source/output hashes and actual commands are retained externally.
