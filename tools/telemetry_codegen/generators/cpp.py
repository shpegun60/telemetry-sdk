"""Emit standalone C++20 DTOs/codecs and separately selected integration helpers.

The validated schema is used only while generating source. Runtime code has no
descriptor graph, native offset table, allocation or transport state machine.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""

SCALARS = {
    1: "bool", 2: "std::uint8_t", 3: "std::int8_t", 4: "std::uint16_t",
    5: "std::int16_t", 6: "std::uint32_t", 7: "std::int32_t",
    8: "std::uint64_t", 9: "std::int64_t", 10: "float", 11: "double",
}


def _type(type_id):
    return f"detail::Type_{type_id}"


def _reachable(schema):
    pending = [0]
    for endpoint in schema.endpoints:
        pending.extend((endpoint.value_type, endpoint.request_type, endpoint.response_type))
    found = set()
    while pending:
        index = pending.pop()
        if index in found:
            continue
        found.add(index)
        item = schema.types[index]
        if item["kind"] == 2:
            pending.append(item["underlying"])
        elif item["kind"] == 3:
            pending.extend(child for child, _ in item["members"])
        elif item["kind"] == 4:
            pending.append(item["element"])
    return tuple(sorted(found))


def _literal(value):
    if value == -(1 << 63):
        return "(-9223372036854775807LL - 1)"
    return f"{value}{'ULL' if value >= 0 else 'LL'}"


def _string(value):
    # Escape every UTF-8 byte; neither compiler encoding nor adjacent hex digits
    # may change the descriptor's original dictionary labels.
    return '"' + ''.join(f"\\x{byte:02x}" for byte in value.encode("utf-8")) + '"'


def _header(schema, name, includes):
    return ("// Generated from a validated Telemetry Descriptor v3. Do not edit.\n"
            f"// Descriptor SHA-256: {schema.descriptor_sha256}\n"
            "// SPDX-License-Identifier: MIT\n#pragma once\n\n" + includes +
            f"\nnamespace {schema.namespace} {{\n")


def _types(schema, reachable):
    rows = [_header(schema, "Types", "#include <array>\n#include <cstdint>\n"), "namespace detail {\n"]
    for index in reachable:
        item = schema.types[index]
        name = f"Type_{index}"
        kind = item["kind"]
        # A named type is defined under its own symbol, in its scope's namespace;
        # Type_N stays as an alias, so every existing spelling keeps compiling.
        named = schema.type_symbol(index)
        defined = named.symbol if named else name
        if named and named.scope:
            rows.append(f"namespace {named.scope} {{\n")
        if kind == 0:
            rows.append(f"using {name} = void;\n")
        elif kind == 1:
            rows.append(f"using {name} = {SCALARS[item['code']]};\n")
        elif kind == 2:
            rows.append(f"enum class {defined} : Type_{item['underlying']} {{\n")
            for (value, _), symbol in zip(item["entries"], schema.enum_names[index]):
                rows.append(f"    {symbol} = {_literal(value)},\n")
            rows.append("};\n")
        elif kind == 3:
            rows.append(f"struct {defined} {{\n")
            for (child, _), symbol in zip(item["members"], schema.member_names[index]):
                rows.append(f"    Type_{child} {symbol};\n")
            rows.append("};\n")
        elif kind == 4:
            rows.append(f"using {name} = std::array<Type_{item['element']}, {item['count']}>;\n")
        if named and named.scope:
            rows.append(f"}} // namespace {named.scope}\n")
        if named:
            rows.append(f"using {name} = {named.detail};\n")
    rows.append("} // namespace detail\n")
    aliases = {}
    for endpoint in schema.endpoints:
        group = aliases.setdefault(endpoint.group, {})
        for alias, index in ((endpoint.value_alias, endpoint.value_type),
                             (endpoint.request_alias, endpoint.request_type),
                             (endpoint.response_alias, endpoint.response_type)):
            if alias:
                group[alias] = index
    for group, entries in aliases.items():
        rows.append(f"\nnamespace {group}::types {{\n")
        for alias, index in entries.items():
            rows.append(f"using {alias} = {_type(index)};\n")
        rows.append(f"}} // namespace {group}::types\n")
    rows.append(_named_types(schema, reachable))
    rows.append(f"}} // namespace {schema.namespace}\n")
    return "".join(rows)


def _named_types(schema, reachable):
    """The public `types` scope of named types, nested by their own scope (v3.1)."""
    named = [schema.type_symbol(index) for index in reachable if schema.type_symbol(index)]
    if not named:
        return ""
    rows = ["\n// Named types (descriptor v3.1), by their wire scope: adc::Config is types::adc::Config.\n",
            "namespace types {\n"]
    scopes = {}
    for symbol in named:
        scopes.setdefault(symbol.scope, []).append(symbol)
    for scope, symbols in scopes.items():
        if scope:
            rows.append(f"namespace {scope} {{\n")
        for symbol in symbols:
            rows.append(f"using {symbol.symbol} = ::{schema.namespace}::detail::{symbol.detail};\n")
        if scope:
            rows.append(f"}} // namespace {scope}\n")
    rows.append("} // namespace types\n")
    return "".join(rows)


def _schema(schema):
    header = schema.descriptor_header
    values = ", ".join(f"std::byte{{0x{byte:02x}}}" for byte in header)
    return (_header(schema, "Schema", "#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <span>\n") +
            f"namespace schema {{\ninline constexpr std::uint16_t major = 3, minor = {schema.minor};\n"
            f"inline constexpr std::uint64_t fingerprint = 0x{schema.fingerprint:016x}ULL;\n"
            f'inline constexpr char descriptorSha256[] = "{schema.descriptor_sha256}";\n'
            f"inline constexpr std::array<std::byte, 64> descriptorHeader{{{values}}};\n"
            "// Declared header identity only: no body hash check or peer authentication.\n"
            "[[nodiscard]] constexpr bool matchesHeader(std::span<const std::byte> bytes) noexcept\n"
            "{\n    if (bytes.size() != descriptorHeader.size()) return false;\n"
            "    for (std::size_t i = 0; i < bytes.size(); ++i)\n"
            "        if (bytes[i] != descriptorHeader[i]) return false;\n"
            "    return true;\n}\n} // namespace schema\n"
            f"}} // namespace {schema.namespace}\n")


ENDPOINTS = r"""
namespace endpoint {
enum class Category : std::uint8_t { Field = 1, Command = 2, Service = 3 };
enum class Operation : std::uint8_t { FieldWrite = 1, Command = 2, Service = 3, FieldRead = 4 };
template<class T> struct Field {
    static_assert(!std::is_void_v<T>);
    using Value = T;
    static constexpr Category category = Category::Field;
    static constexpr Operation readOperation = Operation::FieldRead, writeOperation = Operation::FieldWrite;
    std::uint32_t id, valueTypeId, valueBytes;
    std::uint8_t capabilities;
    std::uint64_t fingerprint = schema::fingerprint;
    [[nodiscard]] constexpr bool readable() const noexcept { return (capabilities & 1) != 0; }
    [[nodiscard]] constexpr bool writable() const noexcept { return (capabilities & 2) != 0; }
};
template<class Q = void> struct Command {
    using Request = Q;
    using Response = void;
    static constexpr Category category = Category::Command;
    static constexpr Operation operation = Operation::Command;
    std::uint32_t id, requestTypeId, requestBytes;
    static constexpr std::uint32_t responseBytes = 0;
    std::uint64_t fingerprint = schema::fingerprint;
};
template<class Q, class R> struct Service {
    using Request = Q;
    using Response = R;
    static constexpr Category category = Category::Service;
    static constexpr Operation operation = Operation::Service;
    std::uint32_t id, requestTypeId, responseTypeId, requestBytes, responseBytes;
    std::uint64_t fingerprint = schema::fingerprint;
};
} // namespace endpoint
"""


def _endpoints(schema):
    rows = [_header(schema, "Endpoints", '#include "Types.hpp"\n#include "Schema.hpp"\n#include <type_traits>\n'), ENDPOINTS]
    for endpoint in schema.endpoints:
        category = ("", "fields", "commands", "services")[endpoint.category]
        rows.append(f"\nnamespace {endpoint.group}::{category} {{\n")
        if endpoint.category == 1:
            index = endpoint.value_type
            signature = f"Field<{_type(index)}>"
            arguments = f"{endpoint.id}, {index}, {schema.types[index]['wire']}, {endpoint.capabilities}"
        elif endpoint.category == 2:
            index = endpoint.request_type
            signature = f"Command<{_type(index)}>"
            arguments = f"{endpoint.id}, {index}, {schema.types[index]['wire']}"
        else:
            q, r = endpoint.request_type, endpoint.response_type
            signature = f"Service<{_type(q)}, {_type(r)}>"
            arguments = f"{endpoint.id}, {q}, {r}, {schema.types[q]['wire']}, {schema.types[r]['wire']}"
        rows.append(f"inline constexpr endpoint::{signature} {endpoint.symbol}{{{arguments}}};\n")
        rows.append(f"}} // namespace {endpoint.group}::{category}\n")
    rows.append(_group_endpoints(schema))
    rows.append(f"}} // namespace {schema.namespace}\n")
    return "".join(rows)


def _group_endpoints(schema):
    """One namespace per group, its endpoints as constants in it, for every descriptor.

    A GroupTree descriptor nests its groups; a catalog descriptor's catalogs of
    one name are one root group of all three families, where two endpoints of
    different families sharing a name take a kind suffix (Reset_Field and
    Reset_Command) and have no path. The fields::/commands::/services::
    constants above stay beside them for now; each endpoint is also a constant
    beside the group's subgroups, the path's form.
    """
    if schema.groups is not None:
        rows = ["\n// Group tree (descriptor v3.1): api::adc::channel1::Gain is the path adc/channel1/Gain.\n"]
    else:
        rows = ["\n// Groups: the catalogs of one name are one group; api::motor::Config is the path motor/Config.\n"]
    members = {}
    for endpoint, group, symbol in zip(schema.endpoints, schema.api_group_of, schema.api_symbols):
        members.setdefault(group, []).append((endpoint, symbol))
    for group in schema.api_groups:
        rows.append(f"namespace {group.qualified} {{\n")
        for endpoint, symbol in members.get(group.index, ()):
            family = ("", "fields", "commands", "services")[endpoint.category]
            rows.append(f"inline constexpr auto {symbol} = {family}::{endpoint.symbol};\n")
        rows.append(f"}} // namespace {group.qualified}\n")
    return "".join(rows)


PRIMITIVES = r"""
static_assert(CHAR_BIT == 8, "Canonical bytes require eight-bit char");
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559 &&
              std::numeric_limits<float>::digits == 24, "IEEE binary32 required");
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::digits == 53, "IEEE binary64 required");
namespace codec_detail {
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t cursor = 0;
    template<class U> U integer() noexcept {
        if constexpr (std::endian::native == std::endian::little) {
            // Leaf copies permit unaligned bytes; aggregate padding is never copied.
            U value;
            std::memcpy(&value, bytes.data() + cursor, sizeof(U));
            cursor += sizeof(U);
            return value;
        }
        U value = 0;
        for (std::size_t i = 0; i < sizeof(U); ++i)
            value = static_cast<U>(value | (static_cast<U>(std::to_integer<unsigned>(bytes[cursor++])) << (8 * i)));
        return value;
    }
};
struct Writer {
    std::span<std::byte> bytes;
    std::size_t cursor = 0;
    template<class U> void integer(U value) noexcept {
        if constexpr (std::endian::native == std::endian::little) {
            std::memcpy(bytes.data() + cursor, &value, sizeof(U));
            cursor += sizeof(U);
            return;
        }
        for (std::size_t i = 0; i < sizeof(U); ++i)
            bytes[cursor++] = std::byte(static_cast<unsigned char>(value >> (8 * i)));
    }
};
inline bool overlap(std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
    if (a.empty() || b.empty()) return false;
    const auto before = std::less<const std::byte*>{};
    return before(a.data(), b.data() + b.size()) && before(b.data(), a.data() + a.size());
}
template<class T> struct Adapter {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>, "Unsupported codec type");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
    static_assert(!std::is_signed_v<T> || std::numeric_limits<T>::lowest() == -std::numeric_limits<T>::max() - 1,
                  "Two's-complement integer range required");
    using U = std::make_unsigned_t<T>;
    static constexpr std::size_t bytes = sizeof(T);
    static void write(const T& value, Writer& out) noexcept {
        if constexpr (std::is_signed_v<T>) out.integer(std::bit_cast<U>(value));
        else out.integer(static_cast<U>(value));
    }
    static bool valid(Reader& in) noexcept { in.cursor += bytes; return true; }
    static void read(Reader& in, T& value) noexcept {
        const U bits = in.integer<U>();
        if constexpr (std::is_signed_v<T>) value = std::bit_cast<T>(bits);
        else value = static_cast<T>(bits);
    }
};
template<> struct Adapter<void> { static constexpr std::size_t bytes = 0; };
template<> struct Adapter<bool> {
    static constexpr std::size_t bytes = 1;
    static void write(const bool& value, Writer& out) noexcept { out.integer<std::uint8_t>(value ? 1 : 0); }
    static bool valid(Reader& in) noexcept { return std::to_integer<std::uint8_t>(in.bytes[in.cursor++]) <= 1; }
    static void read(Reader& in, bool& value) noexcept { value = in.integer<std::uint8_t>() != 0; }
};
template<class T, class U> struct Floating {
    static constexpr std::size_t bytes = sizeof(U);
    static void write(const T& value, Writer& out) noexcept { out.integer(std::bit_cast<U>(value)); }
    static bool valid(Reader& in) noexcept { in.cursor += bytes; return true; }
    static void read(Reader& in, T& value) noexcept { value = std::bit_cast<T>(in.integer<U>()); }
};
template<> struct Adapter<float> : Floating<float, std::uint32_t> {};
template<> struct Adapter<double> : Floating<double, std::uint64_t> {};
template<class T, std::size_t N> struct Adapter<std::array<T, N>> {
    static constexpr std::size_t bytes = N * Adapter<T>::bytes;
    static void write(const std::array<T, N>& value, Writer& out) noexcept {
        for (const auto& element : value) Adapter<T>::write(element, out);
    }
    static bool valid(Reader& in) noexcept {
        for (std::size_t i = 0; i < N; ++i) if (!Adapter<T>::valid(in)) return false;
        return true;
    }
    static void read(Reader& in, std::array<T, N>& value) noexcept {
        for (auto& element : value) Adapter<T>::read(in, element);
    }
};
"""


CODEC = r"""
} // namespace codec_detail
namespace codec {
enum class Status : std::uint8_t { Ok, LengthMismatch, InvalidValue, Overlap, InvalidState };
template<class T> inline constexpr std::size_t wireSize = codec_detail::Adapter<T>::bytes;
template<class T> [[nodiscard]] Status encode(const T& value, std::span<std::byte> output) noexcept {
    static_assert(!std::is_volatile_v<T> && !std::is_void_v<T>);
    if (output.size() != wireSize<T>) return Status::LengthMismatch;
    if (codec_detail::overlap(std::as_bytes(std::span<const T>{std::addressof(value), 1}), output))
        return Status::Overlap;
    codec_detail::Writer writer{output};
    codec_detail::Adapter<T>::write(value, writer);
    return writer.cursor == output.size() ? Status::Ok : Status::InvalidState;
}
// Input is stable for the whole call. Validate completely before modifying output.
template<class T> [[nodiscard]] Status decode(std::span<const std::byte> input, T& output) noexcept {
    static_assert(!std::is_const_v<T> && !std::is_volatile_v<T> && !std::is_void_v<T>);
    if (input.size() != wireSize<T>) return Status::LengthMismatch;
    if (codec_detail::overlap(input, std::as_bytes(std::span<T>{std::addressof(output), 1})))
        return Status::Overlap;
    codec_detail::Reader validator{input};
    if (!codec_detail::Adapter<T>::valid(validator)) return Status::InvalidValue;
    if (validator.cursor != input.size()) return Status::InvalidState;
    codec_detail::Reader reader{input};
    codec_detail::Adapter<T>::read(reader, output);
    return reader.cursor == input.size() ? Status::Ok : Status::InvalidState;
}
template<class T> [[nodiscard]] Status encode(endpoint::Field<T>, const T& value, std::span<std::byte> output) noexcept {
    return encode(value, output);
}
template<class Q> requires (!std::is_void_v<Q>)
[[nodiscard]] Status encode(endpoint::Command<Q>, const Q& value, std::span<std::byte> output) noexcept {
    return encode(value, output);
}
inline Status encode(endpoint::Command<void>, std::span<std::byte> output) noexcept {
    return output.empty() ? Status::Ok : Status::LengthMismatch;
}
template<class Q, class R> requires (!std::is_void_v<Q>)
[[nodiscard]] Status encode(endpoint::Service<Q, R>, const Q& value, std::span<std::byte> output) noexcept {
    return encode(value, output);
}
template<class R> [[nodiscard]] Status encode(endpoint::Service<void, R>, std::span<std::byte> output) noexcept {
    return output.empty() ? Status::Ok : Status::LengthMismatch;
}
template<class T> [[nodiscard]] Status decodeResponse(endpoint::Field<T>, std::span<const std::byte> input, T& output) noexcept {
    return decode(input, output);
}
template<class Q, class R> requires (!std::is_void_v<R>)
[[nodiscard]] Status decodeResponse(endpoint::Service<Q, R>, std::span<const std::byte> input, R& output) noexcept {
    return decode(input, output);
}
template<class Q> [[nodiscard]] Status decodeResponse(endpoint::Service<Q, void>, std::span<const std::byte> input) noexcept {
    return input.empty() ? Status::Ok : Status::LengthMismatch;
}
} // namespace codec
"""


PRESERVING_FLOAT = r"""    static void write(const T& value, Writer& out) noexcept {
        const U bits = std::bit_cast<U>(value);
        if (!out.original.empty()) {
            Reader seed{out.original, out.cursor};
            const U old = seed.integer<U>();
            constexpr U fraction = (U{1} << (sizeof(U) == 4 ? 23 : 52)) - U{1};
            constexpr U exponent = static_cast<U>((std::numeric_limits<U>::max() >> 1) ^ fraction);
            if ((bits & exponent) == exponent && (bits & fraction) != 0 &&
                (old & exponent) == exponent && (old & fraction) != 0) {
                out.integer(old);
                return;
            }
        }
        out.integer(bits);
    }"""


PRESERVING_CODEC = r"""// The original canonical payload is stable throughout this call. Its bytes and
// the native value must both be disjoint from output; no scratch or heap is used.
// All refusal checks precede output writes. Existing float bytes are retained
// only when both their original representation and the new value are NaN.
template<class T> [[nodiscard]] Status encodePreserving(const T& value,
    std::span<const std::byte> original, std::span<std::byte> output) noexcept {
    static_assert(!std::is_volatile_v<T> && !std::is_void_v<T>);
    if (original.size() != wireSize<T> || output.size() != wireSize<T>)
        return Status::LengthMismatch;
    if (codec_detail::overlap(original, output) ||
        codec_detail::overlap(std::as_bytes(std::span<const T>{std::addressof(value), 1}), output))
        return Status::Overlap;
    codec_detail::Reader validator{original};
    if (!codec_detail::Adapter<T>::valid(validator)) return Status::InvalidValue;
    if (validator.cursor != original.size()) return Status::InvalidState;
    codec_detail::Writer writer{output, 0, original};
    codec_detail::Adapter<T>::write(value, writer);
    return writer.cursor == output.size() ? Status::Ok : Status::InvalidState;
}
"""


def _codec(schema, reachable, wire_editing=False):
    includes = ('#include "Endpoints.hpp"\n#include <bit>\n#include <climits>\n#include <cstddef>\n#include <cstring>\n'
                '#include <functional>\n#include <limits>\n#include <memory>\n#include <span>\n#include <type_traits>\n')
    primitives = PRIMITIVES
    codec = CODEC
    if wire_editing:
        primitives = primitives.replace("struct Writer {\n    std::span<std::byte> bytes;\n    std::size_t cursor = 0;",
                                        "struct Writer {\n    std::span<std::byte> bytes;\n    std::size_t cursor = 0;\n    std::span<const std::byte> original{};")
        primitives = primitives.replace("    static void write(const T& value, Writer& out) noexcept { out.integer(std::bit_cast<U>(value)); }",
                                        PRESERVING_FLOAT)
        codec = codec.replace("// Input is stable for the whole call.",
                              PRESERVING_CODEC + "// Input is stable for the whole call.")
    rows = [_header(schema, "Codec", includes), primitives]
    for index in reachable:
        item = schema.types[index]
        kind = item["kind"]
        if kind not in (2, 3):
            continue
        name = _type(index)
        rows.append(f"template<> struct Adapter<{name}> {{\n"
                    f"    static constexpr std::size_t bytes = {item['wire']};\n")
        if kind == 2:
            underlying = _type(item["underlying"])
            rows.extend((f"    static void write(const {name}& value, Writer& out) noexcept {{ Adapter<{underlying}>::write(static_cast<{underlying}>(value), out); }}\n",
                         f"    static bool valid(Reader& in) noexcept {{ return Adapter<{underlying}>::valid(in); }}\n",
                         f"    static void read(Reader& in, {name}& value) noexcept {{ {underlying} raw; Adapter<{underlying}>::read(in, raw); value = static_cast<{name}>(raw); }}\n"))
        else:
            members = tuple(zip(item["members"], schema.member_names[index]))
            # Keep writer cursor updates local across byte stores, which may
            # otherwise alias its caller's state in an outlined helper.
            rows.append(f"    static void write(const {name}& value, Writer& destination) noexcept {{\n"
                        "        Writer out = destination;\n")
            rows.append("        (void)value; (void)out;\n")
            for (child, _), symbol in members:
                rows.append(f"        Adapter<{_type(child)}>::write(value.{symbol}, out);\n")
            rows.append("        destination.cursor = out.cursor;\n    }\n    static bool valid(Reader& in) noexcept {\n        (void)in;\n        return ")
            rows.append(" && ".join(f"Adapter<{_type(child)}>::valid(in)" for (child, _), _ in members) or "true")
            rows.append(f";\n    }}\n    static void read(Reader& in, {name}& value) noexcept {{\n        (void)in; (void)value;\n")
            for (child, _), symbol in members:
                rows.append(f"        Adapter<{_type(child)}>::read(in, value.{symbol});\n")
            rows.append("    }\n")
        rows.append("};\n")
    rows.extend((codec, f"}} // namespace {schema.namespace}\n"))
    return "".join(rows)


RPC = r"""
namespace rpc {
// The caller supplies the explicit trust assertion after its own schema admission.
// Existing RpcClient performs encoding/decoding through Telemetry Codec, not this standalone codec.
template<class T> [[nodiscard]] auto binding(endpoint::Field<T> endpoint,
    ::telemetry::client::rpc::TrustedRpcMapping trust) noexcept {
    static_assert(::telemetry::wireSize<T> == codec::wireSize<T>);
    return ::telemetry::client::rpc::FieldEndpoint<T>{endpoint.id, trust};
}
template<class Q> [[nodiscard]] auto binding(endpoint::Command<Q> endpoint,
    ::telemetry::client::rpc::TrustedRpcMapping trust) noexcept {
    static_assert(::telemetry::wireSize<Q> == codec::wireSize<Q>);
    return ::telemetry::client::rpc::CommandEndpoint<Q>{endpoint.id, trust};
}
template<class Q, class R> [[nodiscard]] auto binding(endpoint::Service<Q, R> endpoint,
    ::telemetry::client::rpc::TrustedRpcMapping trust) noexcept {
    static_assert(::telemetry::wireSize<Q> == codec::wireSize<Q> && ::telemetry::wireSize<R> == codec::wireSize<R>);
    return ::telemetry::client::rpc::ServiceEndpoint<Q, R>{endpoint.id, trust};
}
} // namespace rpc
"""


ATTRIBUTE_CLASSES = {"advisory": "SDK advisory", "application": "application"}


def _bindings_refusal(schema):
    """The descriptor carries attributes, which a native model cannot re-emit yet."""
    first = {}
    for attribute in schema.attributes:
        first.setdefault(attribute["tag"], attribute)
    from ..parser import attribute_tag_class
    tags = ", ".join(
        f"0x{tag:02x} ({ATTRIBUTE_CLASSES[attribute_tag_class(tag)]}, first on {item['record']} "
        + (f"0x{item['id']:08x}" if item["record"] in ("field", "command", "service") else str(item["id"])) + ")"
        for tag, item in sorted(first.items()))
    return ("// Generated optional Telemetry Codec interoperability. SPDX-License-Identifier: MIT\n"
            "#pragma once\n"
            "// Refused: a native model built from these DTOs would emit a descriptor without the\n"
            "// input's attribute area, so its bytes and fingerprint would differ from the device's.\n"
            "// RpcBindings.hpp includes this file and is refused with it.\n"
            f'#error "TelemetryBindings.hpp is refused: the descriptor carries attribute tag{"s" if len(first) > 1 else ""} '
            f'{tags}, which a native model built from these DTOs cannot re-emit yet"\n')


def _telemetry_bindings(schema, reachable):
    if schema.attributes:
        return _bindings_refusal(schema)
    # v3.1: every enum and struct carries its exact wire name, or explicitly none,
    # so a native model built from these DTOs re-emits the input descriptor.
    names = schema.minor >= 1
    rows = ["// Generated optional Telemetry Codec interoperability. SPDX-License-Identifier: MIT\n",
            "#pragma once\n#include \"Types.hpp\"\n#include <telemetry/reflection/Enum.hpp>\n" +
            ("#include <telemetry/reflection/TypeName.hpp>\n" if names else "") + "\n",
            "// Include before using generated enums with Telemetry Codec.\n",
            "namespace telemetry::reflection {\n"]
    for index in reachable:
        item = schema.types[index]
        name = "::" + schema.namespace + "::" + _type(index)
        if item["kind"] == 2:
            rows.append(f"template<> struct EnumReflection<{name}> {{\n")
            if not item["entries"]:
                rows.append(f"    inline static constexpr auto entries = enumEntries<{name}>();\n")
            else:
                rows.append("    inline static constexpr auto entries = enumEntries(\n")
                entries = [f"        enumEntry({name}::{symbol}, {_string(label)})"
                           for (_, label), symbol in zip(item["entries"], schema.enum_names[index])]
                rows.append(",\n".join(entries) + ");\n")
            rows.append("};\n")
        if names and item["kind"] in (2, 3):
            label = f"typeName({_string(item['name'])})" if item.get("name") else "noTypeName()"
            rows.append(f"template<> struct TypeReflection<{name}> {{\n"
                        f"    static constexpr auto name = {label};\n}};\n")
    rows.append("} // namespace telemetry::reflection\n")
    return "".join(rows)


ENCODED = r"""
namespace encoded {
// No schema admission or transport behavior is implied. Buffers/Workspace remain caller owned.
// Wire buffers must be disjoint from the complete Workspace storage, including live leases.
// Native request/response objects may occupy live leases when the wire buffers are separate.
struct Outcome {
    endpoint::Operation operation;
    bool dispatched = false;
    ::telemetry::DispatchStatus dispatch = ::telemetry::DispatchStatus::InternalError;
    std::uint8_t endpointStatus = 0;
    std::uint32_t written = 0;
    ::@NS@::codec::Status codec = ::@NS@::codec::Status::Ok;
    [[nodiscard]] bool ok() const noexcept {
        return dispatched && dispatch == ::telemetry::DispatchStatus::Ok && endpointStatus == 0 &&
               codec == ::@NS@::codec::Status::Ok;
    }
    // Accepted schedules command work; it does not establish physical completion.
    [[nodiscard]] bool commandAccepted() const noexcept {
        return operation == endpoint::Operation::Command && dispatched &&
               dispatch == ::telemetry::DispatchStatus::Ok &&
               endpointStatus == static_cast<std::uint8_t>(::telemetry::CommandResult::Accepted) &&
               codec == ::@NS@::codec::Status::Ok;
    }
};
inline Outcome from(endpoint::Operation operation, ::telemetry::EncodedReadResult result) noexcept {
    return {operation, true, result.dispatch, 0, result.written};
}
inline Outcome from(endpoint::Operation operation, ::telemetry::EncodedWriteResult result) noexcept {
    return {operation, true, result.dispatch, static_cast<std::uint8_t>(result.endpointStatus), 0};
}
inline Outcome from(endpoint::Operation operation, ::telemetry::EncodedCommandResult result) noexcept {
    return {operation, true, result.dispatch, static_cast<std::uint8_t>(result.endpointStatus), 0};
}
inline Outcome from(::telemetry::EncodedCallResult result) noexcept {
    return {endpoint::Operation::Service, true, result.dispatch, static_cast<std::uint8_t>(result.endpointStatus), result.written};
}
template<class T> Outcome decode(Outcome result, std::span<const std::byte> bytes, T& output) noexcept {
    if (!result.ok()) return result;
    if (result.written != codec::wireSize<T> || result.written > bytes.size())
        result.codec = codec::Status::LengthMismatch;
    else result.codec = codec::decode(bytes.first(result.written), output);
    return result;
}
inline Outcome decodeVoid(Outcome result) noexcept {
    if (result.ok() && result.written != 0) result.codec = codec::Status::LengthMismatch;
    return result;
}
template<class T> Outcome readField(::telemetry::ModelView model, endpoint::Field<T> endpoint,
    std::span<std::byte> wireOutput, ::telemetry::Workspace& workspace, T& output) noexcept {
    Outcome result{endpoint::Operation::FieldRead};
    if (codec_detail::overlap(wireOutput, workspace.storage()) ||
        codec_detail::overlap(wireOutput, std::as_bytes(std::span<T>{std::addressof(output), 1}))) {
        result.codec = codec::Status::Overlap; return result;
    }
    return decode(from(result.operation, ::telemetry::readFieldEncoded(model, endpoint.id, wireOutput, workspace)), wireOutput, output);
}
template<class T> Outcome writeField(::telemetry::ModelView model, endpoint::Field<T> endpoint,
    const T& value, std::span<std::byte> wireInput, ::telemetry::Workspace& workspace) noexcept {
    Outcome result{endpoint::Operation::FieldWrite};
    if (codec_detail::overlap(wireInput, workspace.storage())) {
        result.codec = codec::Status::Overlap; return result;
    }
    result.codec = codec::encode(value, wireInput);
    if (result.codec != codec::Status::Ok) return result;
    return from(result.operation, ::telemetry::writeFieldEncoded(model, endpoint.id, wireInput, workspace));
}
template<class Q> requires (!std::is_void_v<Q>)
Outcome executeCommand(::telemetry::ModelView model, endpoint::Command<Q> endpoint,
    const Q& request, std::span<std::byte> wireInput, ::telemetry::Workspace& workspace) noexcept {
    Outcome result{endpoint::Operation::Command};
    if (codec_detail::overlap(wireInput, workspace.storage())) {
        result.codec = codec::Status::Overlap; return result;
    }
    result.codec = codec::encode(request, wireInput);
    if (result.codec != codec::Status::Ok) return result;
    return from(result.operation, ::telemetry::executeCommandEncoded(model, endpoint.id, wireInput, workspace));
}
inline Outcome executeCommand(::telemetry::ModelView model, endpoint::Command<void> endpoint,
    ::telemetry::Workspace& workspace) noexcept {
    return from(endpoint::Operation::Command, ::telemetry::executeCommandEncoded(model, endpoint.id, {}, workspace));
}
template<class Q, class R> requires (!std::is_void_v<Q> && !std::is_void_v<R>)
Outcome callService(::telemetry::ModelView model, endpoint::Service<Q, R> endpoint,
    const Q& request, std::span<std::byte> wireInput, std::span<std::byte> wireOutput,
    ::telemetry::Workspace& workspace, R& output) noexcept {
    Outcome result{endpoint::Operation::Service};
    const auto object = std::as_bytes(std::span<R>{std::addressof(output), 1});
    if (codec_detail::overlap(wireInput, workspace.storage()) ||
        codec_detail::overlap(wireOutput, workspace.storage()) ||
        codec_detail::overlap(wireInput, object) || codec_detail::overlap(wireOutput, object) ||
        codec_detail::overlap(wireOutput, std::as_bytes(std::span<const Q>{std::addressof(request), 1}))) {
        result.codec = codec::Status::Overlap; return result;
    }
    result.codec = codec::encode(request, wireInput);
    if (result.codec != codec::Status::Ok) return result;
    return decode(from(::telemetry::callServiceEncoded(model, endpoint.id, wireInput, wireOutput, workspace)), wireOutput, output);
}
template<class R> requires (!std::is_void_v<R>)
Outcome callService(::telemetry::ModelView model, endpoint::Service<void, R> endpoint,
    std::span<std::byte> wireOutput, ::telemetry::Workspace& workspace, R& output) noexcept {
    Outcome result{endpoint::Operation::Service};
    if (codec_detail::overlap(wireOutput, workspace.storage()) ||
        codec_detail::overlap(wireOutput, std::as_bytes(std::span<R>{std::addressof(output), 1}))) {
        result.codec = codec::Status::Overlap; return result;
    }
    return decode(from(::telemetry::callServiceEncoded(model, endpoint.id, {}, wireOutput, workspace)), wireOutput, output);
}
template<class Q> requires (!std::is_void_v<Q>)
Outcome callService(::telemetry::ModelView model, endpoint::Service<Q, void> endpoint,
    const Q& request, std::span<std::byte> wireInput, ::telemetry::Workspace& workspace) noexcept {
    Outcome result{endpoint::Operation::Service};
    if (codec_detail::overlap(wireInput, workspace.storage())) {
        result.codec = codec::Status::Overlap; return result;
    }
    result.codec = codec::encode(request, wireInput);
    if (result.codec != codec::Status::Ok) return result;
    return decodeVoid(from(::telemetry::callServiceEncoded(model, endpoint.id, wireInput, {}, workspace)));
}
inline Outcome callService(::telemetry::ModelView model, endpoint::Service<void, void> endpoint,
    ::telemetry::Workspace& workspace) noexcept {
    return decodeVoid(from(::telemetry::callServiceEncoded(model, endpoint.id, {}, {}, workspace)));
}
} // namespace encoded
"""


def generate(schema, wire_editing=False):
    """Return deterministic relative-path/source pairs, without writing files."""
    reachable = _reachable(schema)
    root = "cpp/" + schema.namespace.replace("::", "/") + "/"
    suffix = f"}} // namespace {schema.namespace}\n"
    result = {
        root + "Types.hpp": _types(schema, reachable),
        root + "Schema.hpp": _schema(schema),
        root + "Endpoints.hpp": _endpoints(schema),
        root + "Codec.hpp": _codec(schema, reachable, wire_editing),
        root + "TelemetryBindings.hpp": _telemetry_bindings(schema, reachable),
        root + "RpcBindings.hpp": _header(schema, "RpcBindings", '#include "Codec.hpp"\n#include "TelemetryBindings.hpp"\n#include <client/rpc/RpcClient.hpp>\n') + RPC + suffix,
        root + "EncodedBindings.hpp": _header(schema, "EncodedBindings", '#include "Codec.hpp"\n#include <telemetry/model/Adapter.hpp>\n') + ENCODED.replace("@NS@", schema.namespace) + suffix,
    }
    from . import client_cpp
    result.update(client_cpp.generate(schema))
    return result
