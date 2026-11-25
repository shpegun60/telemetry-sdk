"""Versioned host-only seam for optional source emitters over the validated IR.

The SDK owns parsing, symbol allocation, output checks and publication. Extensions
receive the same frozen schema and return source text, never mutate output files.
Authors: Ruslan Kovtun (shpegun60), codexAi. SPDX-License-Identifier: MIT.
"""
from dataclasses import dataclass
from typing import Mapping

from . import __version__
from .schema import Schema, Endpoint
from .naming import KEYWORDS, identifier
from .generators.cpp import _reachable, _string

API_VERSION = 1
# IR 2: one group form. An Endpoint's symbol is its symbol in its group
# (Schema.api_groups/api_symbols); there are no per-family generated symbols.
IR_VERSION = 2
SDK_VERSION = __version__


@dataclass(frozen=True)
class GeneratedTarget:
    files: Mapping[str, str]
    symbol_mapping: dict


def cpp_string_literal(value: str) -> str:
    """Encode every original UTF-8 byte as an unambiguous C++ string literal."""
    return _string(value)


def reachable_type_ids(schema: Schema) -> tuple[int, ...]:
    """Sorted transitive TypeIds referenced by endpoints, including Void ID 0."""
    return _reachable(schema)


__all__ = ['API_VERSION', 'IR_VERSION', 'SDK_VERSION', 'GeneratedTarget', 'Schema',
           'Endpoint', 'KEYWORDS', 'identifier', 'cpp_string_literal', 'reachable_type_ids']
