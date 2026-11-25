"""Endpoint handles of a bound device: nested and path forms, for every descriptor.

A handle keeps one client and one endpoint: it is cheap to copy and stays
valid as long as the client does. `device.adc.channel1.Gain` (C++ and TS/JS)
and the path forms `device.endpoint<"adc/channel1/Gain">()` (C++) or
`device.endpoint("adc/channel1/Gain")` (TS/JS) give the same handle. Paths
follow the section-15 grammar. A group tree nests its groups; a catalog
descriptor (v3.0) has the same form with one root group per catalog name
(`device.motor.Config`, path `motor/Config`). There two endpoints of
different families sharing a name take a kind suffix (`Reset_Field`,
`Reset_Command`) and have no path form: the path is ambiguous, and
RuntimeSchema::findEndpoint refuses it too.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
from dataclasses import dataclass, field
import json

from ..naming import allocate
from . import cpp

DEVICE_METHODS = ("read", "write", "call", "endpoint")
KINDS = ("", "Field", "Command", "Service")


@dataclass
class Leaf:
    endpoint: object
    path: str
    registered: bool  # has a path form (unambiguous)
    constant: str = ""  # the endpoint constant relative to the namespace: adc::channel1::Gain
    access: str = ""  # member chain from the device: adc.channel1.Gain


@dataclass
class Node:
    name: str  # generated struct name, Group<n>
    members: list = field(default_factory=list)  # (symbol, Node | Leaf)


def handle_tree(schema):
    """Root members of the device (symbol, Node) and every leaf in endpoint order."""
    leaves = []
    paths = [schema.endpoint_path(endpoint) for endpoint in schema.endpoints]
    counts = {}
    for path in paths:
        counts[path] = counts.get(path, 0) + 1
    nodes = [Node(f"Group{group.index}") for group in schema.api_groups]
    for group in schema.api_groups:
        if group.parent is not None:
            nodes[group.parent].members.append((group.symbol, nodes[group.index]))
    for endpoint, group, symbol, path in zip(schema.endpoints, schema.api_group_of, schema.api_symbols, paths):
        leaf = Leaf(endpoint, path, counts[path] == 1, f"{schema.api_groups[group].qualified}::{symbol}")
        nodes[group].members.append((symbol, leaf))
        leaves.append(leaf)
    roots = [(group.symbol, nodes[group.index]) for group in schema.api_groups if group.parent is None]
    # A root may not take a device method's name; such a group keeps a suffix.
    symbols = allocate((symbol for symbol, _ in roots), DEVICE_METHODS)
    roots = [(symbol, node) for symbol, (_, node) in zip(symbols, roots)]

    def walk(prefix, node):
        for symbol, child in node.members:
            if isinstance(child, Leaf):
                child.access = prefix + symbol
            else:
                walk(prefix + symbol + ".", child)
    for symbol, node in roots:
        walk(symbol + ".", node)
    return roots, leaves


def _nodes(roots):
    """Every node, children before parents."""
    result = []

    def walk(node):
        for _, child in node.members:
            if isinstance(child, Node):
                walk(child)
        result.append(node)
    for _, node in roots:
        walk(node)
    return result


def _constant(schema, leaf):
    return f"::{schema.namespace}::{leaf.constant}"


CPP_HANDLES = r"""
// Endpoint handles. A handle keeps its client and endpoint, is
// cheap to copy and stays valid as long as the client does; results arrive through
// the client's callbacks, as for client.read(endpoint, callback).
namespace @NS@::client {
namespace handles {
enum class Kind : std::uint8_t { Field = 1, Command = 2, Service = 3 };
// A completion that ignores its result, for a write or call nobody awaits.
struct Ignore {
    template<class R> void operator()(const R&) const noexcept {}
};
// A path as a template argument: device.endpoint<"adc/channel1/Gain">().
template<std::size_t N> struct Path {
    char text[N]{};
    consteval Path(const char (&value)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) text[i] = value[i];
    }
    [[nodiscard]] constexpr std::string_view view() const noexcept { return {text, N - 1}; }
};
template<class Client, class E> class Handle;
template<class Client, class T> class Handle<Client, endpoint::Field<T>> {
public:
    using Endpoint = endpoint::Field<T>;
    using Value = T;
    static constexpr Kind kind = Kind::Field;
    constexpr Handle(Client& client, Endpoint endpoint) noexcept : client_(&client), endpoint_(endpoint) {}
    [[nodiscard]] constexpr std::uint32_t id() const noexcept { return endpoint_.id; }
    [[nodiscard]] constexpr Endpoint endpoint() const noexcept { return endpoint_; }
    [[nodiscard]] constexpr bool writable() const noexcept { return endpoint_.writable(); }
    template<class F> [[nodiscard]] auto read(F&& callback, std::uint32_t timeoutMs = 30000) const noexcept {
        return client_->read(endpoint_, std::forward<F>(callback), timeoutMs);
    }
    template<class F> [[nodiscard]] auto write(const T& value, F&& callback, std::uint32_t timeoutMs = 30000) const noexcept {
        return client_->write(endpoint_, value, std::forward<F>(callback), timeoutMs);
    }
    [[nodiscard]] auto write(const T& value) const noexcept { return write(value, Ignore{}); }
    friend constexpr bool operator==(const Handle& a, const Handle& b) noexcept {
        return a.client_ == b.client_ && a.endpoint_.id == b.endpoint_.id;
    }
private:
    Client* client_;
    Endpoint endpoint_;
};
template<class Client, class Q> class Handle<Client, endpoint::Command<Q>> {
public:
    using Endpoint = endpoint::Command<Q>;
    using Request = Q;
    static constexpr Kind kind = Kind::Command;
    constexpr Handle(Client& client, Endpoint endpoint) noexcept : client_(&client), endpoint_(endpoint) {}
    [[nodiscard]] constexpr std::uint32_t id() const noexcept { return endpoint_.id; }
    [[nodiscard]] constexpr Endpoint endpoint() const noexcept { return endpoint_; }
    template<class R = Q, class F> requires(!std::is_void_v<R>)
    [[nodiscard]] auto call(const R& request, F&& callback, std::uint32_t timeoutMs = 30000) const noexcept {
        return client_->command(endpoint_, request, std::forward<F>(callback), timeoutMs);
    }
    template<class F> requires(std::is_void_v<Q> && std::is_nothrow_invocable_v<std::decay_t<F>&, const ::telemetry::client::Result<void>&>)
    [[nodiscard]] auto call(F&& callback, std::uint32_t timeoutMs = 30000) const noexcept {
        return client_->command(endpoint_, std::forward<F>(callback), timeoutMs);
    }
    template<class R = Q> requires(std::is_void_v<R>)
    [[nodiscard]] auto call() const noexcept { return call(Ignore{}); }
    friend constexpr bool operator==(const Handle& a, const Handle& b) noexcept {
        return a.client_ == b.client_ && a.endpoint_.id == b.endpoint_.id;
    }
private:
    Client* client_;
    Endpoint endpoint_;
};
template<class Client, class Q, class S> class Handle<Client, endpoint::Service<Q, S>> {
public:
    using Endpoint = endpoint::Service<Q, S>;
    using Request = Q;
    using Response = S;
    static constexpr Kind kind = Kind::Service;
    constexpr Handle(Client& client, Endpoint endpoint) noexcept : client_(&client), endpoint_(endpoint) {}
    [[nodiscard]] constexpr std::uint32_t id() const noexcept { return endpoint_.id; }
    [[nodiscard]] constexpr Endpoint endpoint() const noexcept { return endpoint_; }
    template<class R = Q, class F> requires(!std::is_void_v<R>)
    [[nodiscard]] auto call(const R& request, F&& callback, std::uint32_t timeoutMs = 30000) const noexcept {
        return client_->service(endpoint_, request, std::forward<F>(callback), timeoutMs);
    }
    template<class F> requires(std::is_void_v<Q> && std::is_nothrow_invocable_v<std::decay_t<F>&, const ::telemetry::client::Result<S>&>)
    [[nodiscard]] auto call(F&& callback, std::uint32_t timeoutMs = 30000) const noexcept {
        return client_->service(endpoint_, std::forward<F>(callback), timeoutMs);
    }
    friend constexpr bool operator==(const Handle& a, const Handle& b) noexcept {
        return a.client_ == b.client_ && a.endpoint_.id == b.endpoint_.id;
    }
private:
    Client* client_;
    Endpoint endpoint_;
};
"""

CPP_DEVICE_METHODS = r"""
    // The path forms resolve at compile time against the generated path table;
    // a wrong path or value type is a build error that shows the path.
    template<handles::Path P> [[nodiscard]] constexpr auto endpoint() const noexcept {
        constexpr auto index = handles::indexOf<P>;
        if constexpr (index < handles::paths.size())
            return handles::At<index>::get(*this);
        else
            static_assert(index < handles::paths.size(), "No endpoint at path P in this device (see the template argument P)");
    }
    template<handles::Path P, class... A> [[nodiscard]] auto read(A&&... arguments) const noexcept {
        static_assert(decltype(endpoint<P>())::kind == handles::Kind::Field, "read<P>() takes a Field path (see P)");
        return endpoint<P>().read(std::forward<A>(arguments)...);
    }
    template<handles::Path P, class... A> [[nodiscard]] auto write(A&&... arguments) const noexcept {
        static_assert(decltype(endpoint<P>())::kind == handles::Kind::Field, "write<P>() takes a Field path (see P)");
        return endpoint<P>().write(std::forward<A>(arguments)...);
    }
    template<handles::Path P, class... A> [[nodiscard]] auto call(A&&... arguments) const noexcept {
        static_assert(decltype(endpoint<P>())::kind != handles::Kind::Field, "call<P>() takes a Command or Service path (see P)");
        return endpoint<P>().call(std::forward<A>(arguments)...);
    }
"""


def cpp_handles(schema):
    """The C++ device: nested handle structs, the path table and the path forms."""
    roots, leaves = handle_tree(schema)
    ns = schema.namespace
    rows = ["\n#include <array>\n#include <string_view>\n#include <type_traits>\n#include <utility>\n",
            CPP_HANDLES.replace("@NS@", ns)]
    for node in _nodes(roots):
        rows.append(f"template<class Client> struct {node.name} {{\n")
        initializers = []
        for symbol, child in node.members:
            if isinstance(child, Leaf):
                constant = _constant(schema, child)
                rows.append(f"    Handle<Client, std::remove_cv_t<decltype({constant})>> {symbol};\n")
                initializers.append(f"{symbol}{{client, {constant}}}")
            else:
                rows.append(f"    {child.name}<Client> {symbol};\n")
                initializers.append(f"{symbol}{{client}}")
        rows.append(f"    explicit constexpr {node.name}(Client& client) noexcept" +
                    (" : " + ", ".join(initializers) if initializers else "") +
                    " { (void)client; }\n};\n")
    registered = [leaf for leaf in leaves if leaf.registered]
    rows.append(f"inline constexpr std::array<std::string_view, {len(registered)}> paths{{{{\n" +
                "".join(f"    {cpp._string(leaf.path)},\n" for leaf in registered) + "}};\n")
    rows.append("consteval std::size_t find(std::string_view path) noexcept {\n"
                "    for (std::size_t i = 0; i < paths.size(); ++i)\n"
                "        if (paths[i] == path) return i;\n"
                "    return paths.size();\n}\n"
                "template<Path P> inline constexpr std::size_t indexOf = find(P.view());\n"
                "template<std::size_t I> struct At;\n")
    for index, leaf in enumerate(registered):
        rows.append(f"template<> struct At<{index}> {{ template<class D> static constexpr auto get(const D& d) noexcept "
                    f"{{ return d.{leaf.access}; }} }};\n")
    rows.append("} // namespace handles\n\n")
    rows.append("// The device: device.adc.channel1.Gain, or device.endpoint<\"adc/channel1/Gain\">().\n"
                "template<class Client> class Device {\npublic:\n")
    initializers = [f"{symbol}{{client}}" for symbol, _ in roots]
    rows.append("    explicit constexpr Device(Client& client) noexcept" +
                (" : " + ", ".join(initializers) if initializers else "") + " { (void)client; }\n")
    for symbol, node in roots:
        rows.append(f"    handles::{node.name}<Client> {symbol};\n")
    rows.append(CPP_DEVICE_METHODS)
    rows.append("};\n// api::client::endpoint<\"adc/channel1/Gain\">(device) is device.endpoint<\"adc/channel1/Gain\">().\n"
                "template<handles::Path P, class Client>\n"
                "[[nodiscard]] constexpr auto endpoint(const Device<Client>& device) noexcept {\n"
                "    return device.template endpoint<P>();\n}\n")
    rows.append(f"}} // namespace {ns}::client\n")
    return "".join(rows)


def _q(value):
    return json.dumps(value, ensure_ascii=True)


def _value_type(schema, endpoint):
    return f"api.Type_{endpoint.value_type if endpoint.category == 1 else endpoint.request_type}"


def _handle_type(schema, endpoint):
    if endpoint.category == 1:
        name = "FieldHandle" if endpoint.capabilities == 3 else "ReadOnlyFieldHandle"
        return f"{name}<api.Type_{endpoint.value_type}>"
    if endpoint.category == 2:
        return f"CommandHandle<api.Type_{endpoint.request_type}>"
    return f"ServiceHandle<api.Type_{endpoint.request_type}, api.Type_{endpoint.response_type}>"


WEB_RUNTIME = """// One handle per endpoint: read/write for a Field, call for a Command or Service.
// A wrong kind or a read-only write throws, naming the path, as the Dynamic client does.
const _kinds = ['', 'Field', 'Command', 'Service'];
export function noEndpointError(path@ANY@)@ERROR@ {
  return new Error(`No endpoint at path "${String(path)}"`);
}
class EndpointHandle {
  @FIELDS@constructor(client@CLIENT@, path@STRING@, endpoint@ENDPOINT@) {
    this.#client = client;
    this.id = endpoint.id;
    this.path = path;
    this.category = _kinds[endpoint.category];
    this.endpoint = endpoint;
    Object.freeze(this);
  }
  read(timeoutMs@NUMBER@) {
    if (this.endpoint.category !== 1)
      throw new Error(`"${this.path}" is a ${this.category}: read() takes a Field`);
    return this.#client.read(this.endpoint, timeoutMs);
  }
  write(value@UNKNOWN@, timeoutMs@NUMBER@) {
    if (this.endpoint.category !== 1)
      throw new Error(`"${this.path}" is a ${this.category}: write() takes a Field`);
    if (this.endpoint.capabilities !== 3)
      throw new Error(`"${this.path}" is a read-only Field`);
    return this.#client.write(this.endpoint, value, timeoutMs);
  }
  call(...args@ARGS@) {
    if (this.endpoint.category === 1)
      throw new Error(`"${this.path}" is a Field: call() takes a Command or Service`);
    const [value, timeoutMs] = this.endpoint.requestTypeId === 0 ? [undefined, args[0]] : args;
    return this.endpoint.category === 2 ? this.#client.command(this.endpoint, value, timeoutMs)
                                        : this.#client.service(this.endpoint, value, timeoutMs);
  }
}
"""

WEB_TYPES = """export type CallArguments<Q> = [Q] extends [undefined] ? [timeoutMs?: number] : [request: Q, timeoutMs?: number];
interface HandleBase { readonly id: number; readonly path: string; }
export interface ReadOnlyFieldHandle<T> extends HandleBase {
  readonly category: 'Field';
  read(timeoutMs?: number): RequestPromise<T>;
}
export interface FieldHandle<T> extends ReadOnlyFieldHandle<T> {
  write(value: T, timeoutMs?: number): RequestPromise<undefined>;
}
export interface CommandHandle<Q> extends HandleBase {
  readonly category: 'Command';
  call(...args: CallArguments<Q>): RequestPromise<undefined>;
}
export interface ServiceHandle<Q, R> extends HandleBase {
  readonly category: 'Service';
  call(...args: CallArguments<Q>): RequestPromise<R>;
}
"""

WEB_PATH_TYPES = """export type Path = keyof Handles;
type PathsWhere<C> = {[P in Path]: Handles[P] extends C ? P : never}[Path];
export type FieldPath = PathsWhere<{readonly category: 'Field'}>;
export type WritablePath = PathsWhere<{write(value: never, timeoutMs?: number): unknown}>;
export type CallPath = PathsWhere<{readonly category: 'Command' | 'Service'}>;
export type ValueAt<P extends FieldPath> = Handles[P] extends ReadOnlyFieldHandle<infer T> ? T : never;
export type CallArgumentsAt<P extends CallPath> =
  Handles[P] extends {call(...args: infer A): unknown} ? A : never;
export type CallResultAt<P extends CallPath> =
  Handles[P] extends {call(...args: never[]): infer R} ? R : never;
"""


def _web_tree_type(schema, node, depth):
    pad = "  " * (depth + 1)
    parts = []
    for symbol, child in node.members:
        if isinstance(child, Leaf):
            target = f"Handles[{_q(child.path)}]" if child.registered else _handle_type(schema, child.endpoint)
            parts.append(f"{pad}readonly {symbol}: {target};")
        else:
            parts.append(f"{pad}readonly {symbol}: Readonly<{{\n" + _web_tree_type(schema, child, depth + 1) +
                         f"\n{pad}}}>;")
    return "\n".join(parts)


def _device_interface(schema, roots, leaves):
    rows = ["export interface Handles {"]
    rows += [f"  readonly {_q(leaf.path)}: {_handle_type(schema, leaf.endpoint)};" for leaf in leaves if leaf.registered]
    rows.append("}")
    rows.append(WEB_PATH_TYPES.rstrip("\n"))
    rows.append("export interface Device {")
    for symbol, node in roots:
        rows.append(f"  readonly {symbol}: Readonly<{{\n" + _web_tree_type(schema, node, 1) + "\n  }>;")
    rows += ["  endpoint<P extends Path>(path: P): Handles[P];",
             "  read<P extends FieldPath>(path: P, timeoutMs?: number): RequestPromise<ValueAt<P>>;",
             "  write<P extends WritablePath>(path: P, value: ValueAt<P>, timeoutMs?: number): RequestPromise<undefined>;",
             "  call<P extends CallPath>(path: P, ...args: CallArgumentsAt<P>): CallResultAt<P>;",
             "}"]
    return "\n".join(rows)


def _web_tree_value(node, depth):
    pad = "  " * (depth + 2)
    parts = []
    for symbol, child in node.members:
        if isinstance(child, Leaf):
            parts.append(f"{pad}[{_q(symbol)}]: handle({_q(child.path)}, api.{child.access_api}, {str(child.registered).lower()}),")
        else:
            parts.append(f"{pad}[{_q(symbol)}]: Object.freeze({{\n" + _web_tree_value(child, depth + 1) + f"\n{pad}}}),")
    return "\n".join(parts)


def web_handles(schema, typed):
    """handles.js (typed=False) or handles.ts: bindDevice(client) over the bundled client."""
    roots, leaves = handle_tree(schema)
    for leaf in leaves:
        group, _, symbol = leaf.constant.rpartition("::")
        leaf.access_api = group.replace("::", ".") + "[" + _q(symbol) + "]"
    a = (lambda text: text) if typed else (lambda text: "")
    runtime = (WEB_RUNTIME.replace("@ANY@", a(": unknown")).replace("@ERROR@", a(": Error"))
               .replace("@CLIENT@", a(": DeviceClient")).replace("@STRING@", a(": string"))
               .replace("@NUMBER@", a("?: number")).replace("@UNKNOWN@", a(": unknown"))
               .replace("@ARGS@", a(": unknown[]")).replace("@ENDPOINT@", a(": any"))
               .replace("@FIELDS@", "readonly id: number;\n  readonly path: string;\n  readonly category: string;\n"
                        "  readonly endpoint: any;\n  #client: DeviceClient;\n  " if typed else "#client;\n  "))
    if typed:
        runtime = runtime.replace("const [value, timeoutMs] = this.endpoint", "const [value, timeoutMs]: unknown[] = this.endpoint")
        runtime = runtime.replace("this.#client.read(this.endpoint, timeoutMs)",
                                  "this.#client.read(this.endpoint, timeoutMs as number | undefined)")
        runtime = runtime.replace("this.#client.command(this.endpoint, value, timeoutMs)",
                                  "this.#client.command(this.endpoint, value, timeoutMs as number | undefined)")
        runtime = runtime.replace("this.#client.service(this.endpoint, value, timeoutMs)",
                                  "this.#client.service(this.endpoint, value, timeoutMs as number | undefined)")
    rows = ["// Generated endpoint handles. SPDX-License-Identifier: MIT.",
            "// device.adc.channel1.Gain and device.endpoint('adc/channel1/Gain') are the same handle."]
    if typed:
        rows += ["import type {DeviceClient, RequestPromise} from './client.js';"]
    rows += ["import * as api from './device.js';", runtime.rstrip("\n")]
    if typed:
        rows += [WEB_TYPES.rstrip("\n"), _device_interface(schema, roots, leaves)]
    rows += [f"export function bindDevice(client{a(': DeviceClient')}){a(': Device')} {{",
             f"  const handles = new Map{a('<string, EndpointHandle>')}();",
             f"  const handle = (path{a(': string')}, endpoint{a(': unknown')}, registered{a(': boolean')}) => {{",
             "    const result = new EndpointHandle(client, path, endpoint);",
             "    if (registered) handles.set(path, result);",
             "    return result;",
             "  };",
             f"  const endpoint = (path{a(': unknown')}) => {{",
             "    const found = typeof path === 'string' ? handles.get(path) : undefined;",
             "    if (found === undefined) throw noEndpointError(path);",
             "    return found;",
             "  };",
             "  const device = Object.freeze({"]
    for symbol, node in roots:
        rows.append(f"    [{_q(symbol)}]: Object.freeze({{\n" + _web_tree_value(node, 1) + "\n    }),")
    rows += ["    endpoint,",
             f"    read: (path{a(': unknown')}, timeoutMs{a('?: number')}) => endpoint(path).read(timeoutMs),",
             f"    write: (path{a(': unknown')}, value{a(': unknown')}, timeoutMs{a('?: number')}) => endpoint(path).write(value, timeoutMs),",
             f"    call: (path{a(': unknown')}, ...args{a(': unknown[]')}) => endpoint(path).call(...args),",
             "  });",
             "  return device" + a(" as unknown as Device") + ";",
             "}"]
    return "\n".join(rows) + "\n"


def web_handles_declarations(schema):
    roots, leaves = handle_tree(schema)
    rows = ["// Generated endpoint handles. SPDX-License-Identifier: MIT.",
            "import type {DeviceClient, RequestPromise} from './client.js';",
            "import type * as api from './device.js';",
            "export declare function noEndpointError(path: unknown): Error;",
            WEB_TYPES.rstrip("\n"), _device_interface(schema, roots, leaves),
            "export declare function bindDevice(client: DeviceClient): Device;"]
    return "\n".join(rows) + "\n"
