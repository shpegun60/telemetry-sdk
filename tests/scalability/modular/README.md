# Modular composition and consumer dependency fixture

This fixture demonstrates the supported split between implementation storage,
compile-time composition information and a public runtime boundary. It compares
three independent consumer translation units by default. More consumers may be
generated for an explicitly requested timing run; the three endpoint modules
remain fixed, so this is a consumer-fanout experiment, not an endpoint-count limit.

Every module defines one Field, one Command and one Service. The driver reaches
every family through all three modules, reads a separate resource file, checks
the global type identities and uses two fixed transport peer contexts. The source
has no hardware access and no application heap storage.

| Layer | Files | What stays visible |
| --- | --- | --- |
| Module implementation | [ModuleA.cpp](ModuleA.cpp), [ModuleB.cpp](ModuleB.cpp), [ModuleC.cpp](ModuleC.cpp) | Business state, callback definitions and constant-initialized tables stay in individual implementation files. |
| Private composition schema | [Common.hpp](Common.hpp), [ModuleA.hpp](ModuleA.hpp), [ModuleB.hpp](ModuleB.hpp), [ModuleC.hpp](ModuleC.hpp) | Exact DTO definitions, callback signatures and concrete table types are available to composition. |
| One aggregation point | [Composition.hpp](Composition.hpp), [Composition.cpp](Composition.cpp) | Family catalogs and Model derive one structural TypeRegistry across all module roots. |
| Public runtime boundary | [Runtime.hpp](Runtime.hpp) | Borrowed ModelView and FileSystemView access, fingerprint and type count; no module DTO or table aliases. |
| Independent clients | [ClientBody.hpp](ClientBody.hpp), generated ClientN.cpp files | Typed mode includes Composition.hpp and additionally instantiates native calls. Runtime mode calls compiled encoded operations through ModelView. Both modes execute the same encoded checks. |

The current Model API derives Registry from the family catalog RootTypes lists.
Consequently the aggregation translation unit must see every root DTO definition
and table type. Erased catalog views exported from completely private schemas do
not provide those compile-time facts; this fixture does not invent a merge of
registries or a model factory with such behavior. See the real implementation in
[Model.hpp](../../../lib/telemetry/model/Model.hpp) and
[Registry.hpp](../../../lib/telemetry/type/Registry.hpp).

In runtime mode only Composition.cpp includes the private composition header.
The compiled adapter's public header still includes the library's common ABI and
view definitions. An erased public boundary therefore removes the application's
private schema dependencies and their template instantiations, while retaining
the common library headers needed for dispatch. The runner records actual
compiler dependencies and preprocessed consumer bytes rather than promising
zero header parsing.

The three Reading structs have the same shape but retain distinct C++ identities.
Their global TypeIds differ. Delta and Query are shared exact request types and
deduplicate across modules. The single resulting registry has 17 types: the 12
builtins, three Reading structs and two shared request structs. The linked image
must contain exactly one retained registry descriptor definition in either mode.

Tables, catalogs, the Model, its view and the resource table are constant-initialized
and remain at stable process-lifetime addresses. The module tables are declared
extern in private headers and defined once in their own implementation files.
The inline composition declarations in typed mode are ordinary C++ inline
variables: consumers refer to one linked definition. The runtime boundary returns
views borrowing this named storage; it does not return views of local temporaries.

Descriptor construction needs values of the extern module rows. The first
fingerprint() query automatically constructs and validates the immutable
descriptor in function-local static storage; subsequent queries reuse it.
The driver needs that fingerprint to form its Bind request. There is no required
init/setup lifecycle call and no explicit initialized state. The fixture does
not claim a constexpr descriptor from unavailable .cpp row values. The runner
rejects eager global startup constructors in inspected objects. No concurrent
business-state access is modeled; the application must serialize mutable module
state and peer ownership.

Two fixed Peer objects own separate Binding, scratch and Workspace storage.
Admission counts both unbound and bound peers, rejects a third peer, binds the two
accepted peers, processes an actual Command packet, resets one Binding and shows
that the other peer stays ready. The reused peer starts unbound. Scratch leases
must return to zero after calls. The allocation detector covers automatic caching
and all checked operations, and object inspection rejects allocation references
outside that detector. This establishes these fixture paths; it does not measure
an operating system transport, C runtime startup or MCU execution.

Run from the repository root with a fresh external output directory:

    python tests/scalability/modular.py --cxx g++ --build-dir /external/validation/modular-smoke

The normal smoke run uses three clients. To generate sources without compiling:

    python tests/scalability/modular.py --prepare-only --build-dir /external/validation/modular-prepared

After reserving an isolated CPU slot, a larger timing run can use:

    python tests/scalability/modular.py --measure --clients 24 --cxx g++ --build-dir /external/validation/modular-measured

The runner retains summary.json, compiler commands, dependency files, generated
sources, objects, preprocessed consumer output, symbol listings and functional
logs. Source paths and hashes, checkout HEAD and compiler hash identify the
inputs. Generated files never enter the checkout, and a nonempty output directory
is refused so previous evidence remains intact.

For each mode it performs a full sequential build, then appends a comment to the
external copy of ModuleA.cpp and rebuilds only its affected translation unit.
It next changes the external ModuleA.hpp copy and rebuilds every translation
unit whose actual compiler dependency list includes that header. The expected
consumer rebuild count is all clients in typed mode and zero in runtime mode.
The module implementation and composition still rebuild in runtime mode. These
comment edits isolate dependency fanout without altering runtime behavior or
checkout timestamps. Every rebuilt image repeats the complete functional check.
This build isolation does not promise unchanged wire schema: real DTO changes
can alter payload requirements and the descriptor fingerprint, so consumers must
still honor the normal schema agreement and encoded-operation status checks.

Timing values are reported only with --measure and describe the recorded host,
compiler, options and consumer count. Timing logs from smoke runs are diagnostic
durations. The two modes share encoded checks; typed mode also compiles native
calls, so its compile cost includes useful typed access instantiation. There is
no runtime latency comparison or endpoint-count recommendation in this fixture.

For the surrounding ownership and transport contracts see
[Architecture](../../../doc/Architecture.md),
[Transport walkthrough](../../../doc/user/TransportWalkthrough.md) and
[Wire v3](../../../doc/WireV3.md).
