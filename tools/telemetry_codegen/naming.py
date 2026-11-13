"""Deterministic, ASCII-only symbols shared by all SDK backends.

Wire names are never rewritten in the parsed descriptor. Numeric suffixes are
allocated against the complete scope, including already suffixed user names.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""

import re


KEYWORDS = frozenset("""
alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto
bitand bitor bool break case catch char char8_t char16_t char32_t class compl
concept const consteval constexpr constinit const_cast continue co_await co_return
co_yield decltype default delete do double dynamic_cast else enum explicit export
extern false float for friend goto if inline int long mutable namespace new
noexcept not not_eq nullptr operator or or_eq private protected public reflexpr
register reinterpret_cast requires return short signed sizeof static static_assert
static_cast struct switch synchronized template this thread_local throw true try
typedef typeid typename union unsigned using virtual void volatile wchar_t while
xor xor_eq abstract any as asserts async await bigint boolean constructor debugger
declare from function get global implements import in infer instanceof interface
is keyof let module never null number object of out override package readonly
require set string super symbol type undefined unique unknown var with yield
""".split())

SUPPORT_NAMES = frozenset("""
types fields commands services detail codec codec_detail schema endpoint endpoints rpc encoded Operation
Category CodecStatus Status Result EncodedBindings RpcBindings TypeId PackedId
fingerprint major minor version wireBytes requestBytes responseBytes valueBytes
id operation category capabilities encode decode binding trustedRpcMapping
__proto__ prototype then toJSON manifest
descriptorSha256 wireVersion assertCompatibleFingerprint matchesHeader codecs enums
TelemetryCodecError ByteInput ValueCodec
arguments eval Array ArrayBuffer ArrayBufferView BigInt BigInt64Array BigUint64Array
Boolean DataView Error Float32Array Float64Array Int8Array Int16Array Int32Array
JSON Map Math Number Object Promise RangeError Readonly ReadonlyArray Record Set
String Symbol TypeError Uint8Array Uint8ClampedArray Uint16Array Uint32Array
std
""".split())


MACROS = frozenset("""
NULL EOF NAN INFINITY INT8_MIN INT16_MIN INT32_MIN INT64_MIN INT8_MAX INT16_MAX
INT32_MAX INT64_MAX UINT8_MAX UINT16_MAX UINT32_MAX UINT64_MAX INTMAX_MIN INTMAX_MAX
UINTMAX_MAX INTPTR_MIN INTPTR_MAX UINTPTR_MAX PTRDIFF_MIN PTRDIFF_MAX SIZE_MAX
SIG_ATOMIC_MIN SIG_ATOMIC_MAX WCHAR_MIN WCHAR_MAX WINT_MIN WINT_MAX
""".split()) | frozenset(f"{prefix}{bits}_{bound}" for prefix in ("INT_LEAST", "INT_FAST", "UINT_LEAST", "UINT_FAST")
                        for bits in (8, 16, 32, 64) for bound in ("MIN", "MAX"))


def identifier(name, protect_support=True):
    """Keep ordinary identifiers readable; escape other Unicode by code point."""
    pieces = []
    for char in name:
        if char.isascii() and (char.isalnum() or char == "_"):
            pieces.append(char)
        elif ord(char) < 128:
            pieces.append("_")
        else:
            pieces.append("u" + format(ord(char), "x") + "_")
    result = "".join(pieces).strip("_") or "item"
    result = re.sub(r"_+", "_", result)
    if result[0].isdigit():
        result = "n_" + result
    if (result in KEYWORDS or result in MACROS or re.fullmatch(r"Type_[0-9]+", result)
            or (protect_support and result in SUPPORT_NAMES)):
        result += "_value"
    return result


def allocate(names, reserved=(), protect_support=True):
    """Return unique symbols without stealing later user-provided base names."""
    bases = tuple(identifier(name, protect_support) for name in names)
    occupied = set(reserved)
    all_bases = set(bases)
    result, next_suffix = [], {}
    for base in bases:
        candidate, suffix = base, next_suffix.get(base, 2)
        while candidate in occupied or (candidate != base and candidate in all_bases):
            candidate = f"{base}_{suffix}"
            suffix += 1
        occupied.add(candidate)
        next_suffix[base] = suffix
        result.append(candidate)
    return tuple(result)


def validate_namespace(namespace):
    """A requested namespace is an explicit API, so reject rather than rename it."""
    if not isinstance(namespace, str) or not namespace:
        raise ValueError("namespace must be a nonempty C++ namespace")
    # The generated client bindings would otherwise redeclare the native
    # telemetry::client API. Only that exact namespace is reserved; a distinct
    # nested namespace such as telemetry::my_device has its own binding scope.
    if namespace == "telemetry":
        raise ValueError("namespace 'telemetry' is reserved by the native SDK; "
                         "use 'device_api' or a distinct nested namespace such as 'telemetry::my_device'")
    for part in namespace.split("::"):
        if (not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", part) or "__" in part
                or part in KEYWORDS or part in SUPPORT_NAMES or part in MACROS):
            raise ValueError("namespace must contain safe ASCII identifiers separated by ::")
    return namespace
