# Runtime schema qualification

Run from the repository root with an external output directory:

```sh
python3 -B tests/runtime_schema/run.py --cxx g++ --build-dir /external/runtime-gcc
python3 -B tests/runtime_schema/run.py --cxx clang++-18 --sanitize --build-dir /external/runtime-sanitized
python3 -B tests/runtime_schema/run.py --big-endian --build-dir /external/runtime-endian
```

The Native producer exports a real model descriptor and compares core
`dispatchEncoded(FieldRead)` bytes with the static Codec. A consumer including
only runtime headers checks scalar bit patterns, nested arrays/structs, open
enums, empty values, prepared paths, workspace identity/invalidation, exact
capacities, overlap and malformed values. A generated standalone SDK decodes
and re-encodes both native payloads byte for byte. Compiler dependency output
rejects reflection, Resource, Client and Integration in the runtime-only probe.

A second real Native descriptor checks category-aware endpoint lookup with
separate names and literal `group.endpoint` paths. It covers Fields, Commands
and Services, UTF-8 group/endpoint names, paths ambiguous because names contain
dots, missing names, invalid categories and exact prefix/suffix/case matching.
The allocation-trap reader runs those same lookups. Lifetime tests repeat them
after invalidation and actual release of descriptor and metadata storage.

The move-only Writer stages edits without a generated DTO. Its independent
checks exercise exact scalar types, nested arrays, open enum codes, unaligned
buffers, all integer boundaries and bit-exact signed zeros, subnormals,
infinities, quiet and signaling NaNs of both signs. They check the first-error
rule, unchanged staging/destinations on failure, source snapshot lifetime,
consumed/moved editors and aliases against every published schema index.
Intentional staging corruption checks the final canonical-value validation.
The committed fixture is compared byte for byte with an independently encoded
Native DTO, passed through real FieldWrite/FieldRead dispatch, and decoded and
re-encoded with the generated standalone SDK.

The same runtime checks execute with all replaceable C++ allocation functions
trapped. A separate control must exit 86 through the allocation trap. This is a
C++ allocation proof for the tested paths; the file-loading harness uses C stdio.
The Writer executable also runs through the same allocation trap.

The lifetime checks exercise temporary parse results, copy/move operations,
foreign workspaces and failed reparsing. They actually free descriptor, payload
and index storage after invalidation while keeping the workspace object alive;
the sanitized suite checks that stale access never touches those buffers.
Writer lifetime checks also release the original immediately after staging,
destroy move sources, and free scratch plus schema backing storage after reset
or a failed reparse. Only the stable workspace control object is retained.

On Linux, a separate plain executable protects a 64 KiB Boolean payload with
`mprotect`. Navigation succeeds with all payload pages inaccessible; prepared
scalar reads succeed with only the selected page accessible. Two child-process
controls must receive `SIGSEGV` from a direct protected read and whole-value
validation. This proves the pages are protected and detects accidental subtree
rescans without timing thresholds. The controls run without sanitizer signal
interception, including in the sanitized suite. Other hosts report this probe
as skipped; the runtime reader itself has no operating-system dependency.

The Writer has a separate plain protected-page check: after `begin`, all
original pages and unrelated staging pages are inaccessible. Prepared setters
still work on the selected page. Three child-process controls require SIGSEGV
for a protected read, whole-value validation and commit. A shared destination
must retain its sentinel bytes when commit cannot finish validation. This
establishes that sets do not rescan the DTO and commit validates before copying.
The Writer itself has no operating-system dependency.

The parser corpus includes rehashed semantic mutations, section truncations,
depth boundaries, UTF-8 names, and seeded random inputs. Every acceptance result
is compared with the existing independent Python descriptor parser. The runner
retains command logs, binary/generated/fixture hashes, corpus classifications and
before/end source hashes in `summary.json`. These are host checks, with no board
execution or transport behavior claimed.

`--big-endian` requires `s390x-linux-gnu-g++`, `qemu-s390x` and the s390x sysroot
at `/usr/s390x-linux-gnu`. It executes the reader, public Writer setters and
Writer allocation-trap executable on that actual big-endian ABI. The endian CI
job requires these tools; it never substitutes an endian preprocessor macro.
