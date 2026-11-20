"""Deeply immutable validated schema and deterministic public symbol allocation.

Type IDs remain the descriptor's original positional IDs, including when a
backend emits only reachable definitions. No native C++ offsets are inferred.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""

from dataclasses import dataclass
import hashlib
import struct
from types import MappingProxyType
from typing import Mapping

from .naming import SUPPORT_NAMES, allocate, validate_namespace
from .parser import Limits, parse


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
class Schema:
    raw: Mapping
    namespace: str
    descriptor_sha256: str
    endpoints: tuple[Endpoint, ...]
    member_names: tuple[tuple[str, ...], ...]
    enum_names: tuple[tuple[str, ...], ...]

    @property
    def types(self):
        return self.raw["types"]

    @property
    def fingerprint(self):
        return self.raw["fingerprint"]

    @property
    def minor(self):
        """0 for a v3.0 descriptor; 1 when it names a type or has attributes (v3.1)."""
        return self.raw["minor"]

    @property
    def descriptor_header(self):
        """Reproduce the exact declared header from the already validated IR."""
        counts = []
        for catalogs, endpoints in zip(self.raw["catalogs"], self.raw["endpoints"]):
            counts.extend((len(catalogs), len(endpoints)))
        return struct.pack("<4sHHHHIQ10I", b"TDS3", 3, self.minor, 64, 0, self.raw["total"],
                           self.fingerprint, len(self.types), *counts, *self.raw["offsets"])


def load_schema(data: bytes, namespace="device_api", limits=Limits()):
    """Validate before naming, then publish only recursively immutable values."""
    namespace = validate_namespace(namespace)
    if not isinstance(data, bytes):
        raise TypeError("load_schema requires immutable bytes")
    raw = parse(data, limits)
    catalog_names = tuple(dict.fromkeys(
        catalog["name"] for catalogs in raw["catalogs"] for catalog in catalogs))
    group_symbols = dict(zip(catalog_names, allocate(catalog_names)))
    pending = []
    for category, catalogs in enumerate(raw["catalogs"], 1):
        for catalog in catalogs:
            rows = raw["endpoints"][category - 1][catalog["first"]:
                                                    catalog["first"] + catalog["count"]]
            symbols = allocate(row["name"] for row in rows)
            for row, symbol in zip(rows, symbols):
                pending.append(dict(
                    category=category, id=row["id"], catalog_index=catalog["group"],
                    catalog_name=catalog["name"], name=row["name"],
                    group=group_symbols[catalog["name"]], symbol=symbol,
                    value_type=row["type"] if category == 1 else 0,
                    request_type=row["type"] if category != 1 else 0,
                    response_type=row.get("response", 0),
                    capabilities=row.get("capabilities", 0)))
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
                  tuple(Endpoint(**item) for item in pending), member_names, enum_names)
