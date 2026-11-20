# Endpoint handles by path: generated and Dynamic clients

The generated (Static) device of a v3.1 descriptor and the Dynamic clients'
`endpoint(path)` give the same handles for the same paths
([reference](../../doc/GeneratedNamesAndHandles.md)). One native Model serves every
client: the group-tree fixture of [`../structured/group_tree`](../structured/group_tree),
whose descriptor is `tests/structured/descriptor/tree.hex` (the runner checks it).

- [`Parity.cpp`](Parity.cpp): in one process, the generated `tree_api::client::Device`
  over the generated `DeviceClient` and `DynamicClient::endpoint(path)` over
  RuntimeSchema, each behind its own Encoded backend dispatching into the Model.
  Every Field reads the same bytes; member paths read their part; a write through one
  client reads back through the other; Commands and Services answer the same; both
  refuse a read-only write with `ReadOnly`; the nested form, `device.endpoint<"...">()`
  and `tree_api::client::endpoint<"...">(device)` are one handle with the device's
  PackedId; a kept Dynamic handle refuses after its admission ends. It prints the
  Dynamic refusals in `describePath()` wording.
- [`Bridge.cpp`](Bridge.cpp) and [`Parity.mjs`](Parity.mjs): the same in JS, the generated
  `bindDevice(client)` against `DynamicClient.endpoint(path)`, run once with the published
  JS and once with the compiled TS handles. The refusals both JS clients throw are
  compared with each other and, by the runner, with the C++ lines.
- [`Usage.ts`](Usage.ts): strict TypeScript over the path forms; every
  `@ts-expect-error` (unknown path, member path, wrong kind, read-only write, wrong
  value type, missing request) must be an error.
- The runner's C++ probes must fail for their reason: an unknown path (the diagnostic
  shows `Path<18>{"adc/channel9/Gain"}`), `read<P>()` of a Command, `call<P>()` of a
  Field, and a wrong value type (the note shows the path). A program using only the
  path forms must keep no path text in its image.

```sh
python3 -B tests/client_handles/run.py --build-dir /external/handles \
  --cxx g++ --node node --tsc /path/to/typescript/bin/tsc
python3 -B tests/client_handles/run.py --build-dir /external/handles-asan \
  --cxx clang++-18 --sanitize --node node --tsc /path/to/typescript/bin/tsc
```

The build directory must be outside the checkout or under its ignored `build/`.
No hardware is used.
