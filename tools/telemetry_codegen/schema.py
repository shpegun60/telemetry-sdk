"""Deeply immutable validated schema and deterministic public symbol allocation.

Type IDs remain the descriptor's original positional IDs, including when a
backend emits only reachable definitions. No native C++ offsets are inferred.
Descriptor v3.1 adds two symbol scopes, both additions to the v3.0 symbols:
named types live in a dedicated `types` scope, nested by their own scope
(`adc::Config` -> `types::adc::Config`), and a GroupTree descriptor maps every
group to a nested scope that holds its subgroups and endpoints side by side.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""

from dataclasses import dataclass
import hashlib
import struct
from types import MappingProxyType
from typing import Mapping, Optional

from .naming import SUPPORT_NAMES, allocate, validate_namespace
from .parser import Limits, parse


# Names the type scope cannot take although they are not C++/JS support names:
# `std` would shadow the standard library inside `detail`, `codecs`/`enums`
# are read inside the TS `types` namespaces, and the rest are JS object hazards.
TYPE_SCOPE_RESERVED = ("std", "codecs", "enums", "then", "toJSON", "prototype")


def _freeze(value):
    if isinstance(value, dict):
        return MappingProxyType({key: _freeze(item) for key, item in value.items()})
    if isinstance(value, (list, tuple)):
        return tuple(_freeze(item) for item in value)
    return value


@dataclass(frozen=True)
class Endpoint:
    category: int
    id: int
    catalog_index: int
    catalog_name: str
    name: str
    group: str
    symbol: str
    value_type: int = 0
    request_type: int = 0
    response_type: int = 0
    capabilities: int = 0
    value_alias: str = ""
    request_alias: str = ""
    response_alias: str = ""


@dataclass(frozen=True)
class TypeSymbol:
    """Public symbol of one named type: `scope` is "" for a name without a scope."""
    name: str
    scope: str
    symbol: str

    @property
    def detail(self):
        """C++ spelling inside `<ns>::detail`, where the type is defined."""
        return f"{self.scope}::{self.symbol}" if self.scope else self.symbol

    @property
    def cpp(self):
        """C++ spelling relative to the generated namespace."""
        return "types::" + self.detail

    @property
    def js(self):
        """JS/TS spelling relative to the generated module."""
        return "types." + (f"{self.scope}.{self.symbol}" if self.scope else self.symbol)


@dataclass(frozen=True)
class Group:
    """One group of a GroupTree descriptor, in pre-order."""
    index: int
    name: str
    path: str
    depth: int
    parent: Optional[int]
    symbol: str
    qualified: str  # the C++ path of symbols from the root, "adc::channel1"

    @property
    def js(self):
        return self.qualified.replace("::", ".")


@dataclass(frozen=True)
class Schema:
    raw: Mapping
    namespace: str
    descriptor_sha256: str
    endpoints: tuple[Endpoint, ...]
    member_names: tuple[tuple[str, ...], ...]
    enum_names: tuple[tuple[str, ...], ...]
    # Additions of v3.1; empty or None for a v3.0 descriptor.
    type_symbols: tuple[Optional[TypeSymbol], ...] = ()
    groups: Optional[tuple[Group, ...]] = None
    tree_symbols: tuple[str, ...] = ()  # per endpoint: its symbol among its group's siblings

    @property
    def types(self):
        return self.raw["types"]

    @property
    def fingerprint(self):
        return self.raw["fingerprint"]

    @property
    def minor(self):
        """0 for a v3.0 descriptor; 1 when it names a type, has attributes or a group tree (v3.1)."""
        return self.raw["minor"]

    @property
    def tree(self):
        """True for a GroupTree descriptor: one shared tree of nested groups."""
        return self.raw["groups"] is not None

    @property
    def attributes(self):
        """Every attribute of the descriptor, raw: record, id, category, offset, tag, value bytes."""
        return self.raw["attributes"]

    def type_symbol(self, type_id):
        return self.type_symbols[type_id] if self.type_symbols else None

    def endpoint_path(self, endpoint):
        """The section-15 path of an endpoint: group/.../endpoint, flat names escaped."""
        from .parser import escape_segment
        if self.groups is not None:
            return self.groups[endpoint.catalog_index].path + "/" + endpoint.name
        return escape_segment(endpoint.catalog_name) + "/" + escape_segment(endpoint.name)

    @property
    def descriptor_header(self):
        """Reproduce the exact declared header from the already validated IR."""
        counts = []
        for catalogs, endpoints in zip(self.raw["catalogs"], self.raw["endpoints"]):
            counts.extend((len(catalogs), len(endpoints)))
        return struct.pack("<4sHHHHIQ10I", b"TDS3", 3, self.minor, 64, self.raw["flags"], self.raw["total"],
                           self.fingerprint, len(self.types), *counts, *self.raw["offsets"])


def _type_symbols(raw):
    """Allocate `types::scope::Name` symbols: scopes and unscoped names share one scope.

    Support names are not protected here (a struct called Status stays Status);
    keywords, macros, `Type_N` and TYPE_SCOPE_RESERVED are.
    """
    named = [(item["id"], item["name"]) for item in raw["types"] if item.get("name")]
    if not named:
        return ()
    top, inner = [], {}
    for type_id, name in named:
        scope, _, own = name.rpartition("::")
        key = ("scope", scope) if scope else ("type", own)
        if key not in top:
            top.append(key)
        if scope:
            inner.setdefault(scope, []).append((type_id, own))
    top_symbols = dict(zip(top, allocate((wire for _, wire in top), TYPE_SCOPE_RESERVED,
                                         protect_support=False)))
    result = [None] * len(raw["types"])
    for type_id, name in named:
        scope, _, own = name.rpartition("::")
        if not scope:
            result[type_id] = TypeSymbol(name, "", top_symbols[("type", own)])
    for scope, rows in inner.items():
        symbols = allocate((own for _, own in rows), TYPE_SCOPE_RESERVED, protect_support=False)
        for (type_id, _), symbol in zip(rows, symbols):
            result[type_id] = TypeSymbol(raw["types"][type_id]["name"], top_symbols[("scope", scope)], symbol)
    return tuple(result)


def _group_tree(raw):
    """Symbols of a GroupTree: each group is a scope holding its subgroups, then its
    Fields, Commands and Services, all allocated together (they are siblings)."""
    groups = raw["groups"]
    held = {}
    for index, group in enumerate(groups):
        held.setdefault(group["parent"], []).append(("group", index, group["name"]))
    for category, rows in enumerate(raw["endpoints"]):
        for local, row in enumerate(rows):
            held.setdefault(row["id"] >> 16, []).append(("endpoint", (category, local), row["name"]))
    symbols = {}
    for items in held.values():
        for (kind, key, _), symbol in zip(items, allocate(name for _, _, name in items)):
            symbols[(kind, key)] = symbol
    result = []
    for index, group in enumerate(groups):
        own = symbols[("group", index)]
        parent = group["parent"]
        qualified = own if parent is None else result[parent].qualified + "::" + own
        result.append(Group(index, group["name"], group["path"], group["depth"], parent, own, qualified))
    return tuple(result), symbols


def load_schema(data: bytes, namespace="device_api", limits=Limits()):
    """Validate before naming, then publish only recursively immutable values."""
    namespace = validate_namespace(namespace)
    if not isinstance(data, bytes):
        raise TypeError("load_schema requires immutable bytes")
    raw = parse(data, limits)
    groups, tree_symbols = None, {}
    if raw["groups"] is not None:
        groups, tree_symbols = _group_tree(raw)
        group_symbols = None
    else:
        catalog_names = tuple(dict.fromkeys(
            catalog["name"] for catalogs in raw["catalogs"] for catalog in catalogs))
        group_symbols = dict(zip(catalog_names, allocate(catalog_names)))
    pending, siblings = [], []
    for category, catalogs in enumerate(raw["catalogs"], 1):
        for catalog in catalogs:
            rows = raw["endpoints"][category - 1][catalog["first"]:
                                                    catalog["first"] + catalog["count"]]
            symbols = allocate(row["name"] for row in rows)
            group = (groups[catalog["group"]].qualified if groups is not None
                     else group_symbols[catalog["name"]])
            for local, (row, symbol) in enumerate(zip(rows, symbols)):
                pending.append(dict(
                    category=category, id=row["id"], catalog_index=catalog["group"],
                    catalog_name=catalog["name"], name=row["name"],
                    group=group, symbol=symbol,
                    value_type=row["type"] if category == 1 else 0,
                    request_type=row["type"] if category != 1 else 0,
                    response_type=row.get("response", 0),
                    capabilities=row.get("capabilities", 0)))
                if groups is not None:
                    siblings.append(tree_symbols[("endpoint", (category - 1, catalog["first"] + local))])
    # All endpoint aliases coexist in group::types, irrespective of category.
    alias_scopes = {}
    for item in pending:
        requests, locations = alias_scopes.setdefault(item["group"], ([], []))
        if item["category"] == 1:
            requests.append(item["name"])
            locations.append((item, "value_alias"))
        else:
            requests.append(item["name"] + "Request")
            locations.append((item, "request_alias"))
            if item["category"] == 3:
                requests.append(item["name"] + "Response")
                locations.append((item, "response_alias"))
    for requests, locations in alias_scopes.values():
        for (item, key), alias in zip(locations, allocate(requests, SUPPORT_NAMES)):
            item[key] = alias
    member_names = tuple(allocate((name for _, name in item.get("members", ())),
                                  (f"Type_{item['id']}",), protect_support=False)
                         for item in raw["types"])
    enum_names = tuple(allocate((name for _, name in item.get("entries", ())),
                                (f"Type_{item['id']}",))
                       for item in raw["types"])
    return Schema(_freeze(raw), namespace, hashlib.sha256(data).hexdigest(),
                  tuple(Endpoint(**item) for item in pending), member_names, enum_names,
                  _type_symbols(raw), groups, tuple(siblings))
