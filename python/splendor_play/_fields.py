"""Field decoders for match requests and logs."""
from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def closed(value: Any, fields: set[str], name: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping) or set(value) != fields:
        raise ValueError(f"{name} fields do not match its contract")
    return value


def uint(value: Any, bits: int, name: str, *, positive: bool = False) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be a uint{bits}")
    if value < (1 if positive else 0) or value >= 1 << bits:
        raise ValueError(f"{name} is outside uint{bits}{' > 0' if positive else ''}")
    return value
