"""Log text for native choice details (`Episode.choice_details` records)."""

from __future__ import annotations

from typing import Any, Mapping, Sequence

TOKEN_COLORS = ("white", "blue", "green", "red", "black", "gold")


def _tokens(vector: Sequence[int], sign: int = 1) -> str:
    parts = [
        f"{sign * count} {color}"
        for color, count in zip(TOKEN_COLORS, vector)
        if sign * count > 0
    ]
    return ", ".join(parts) if parts else "nothing"


def describe_choice(choice: Mapping[str, Any], catalog: Mapping[str, Any]) -> str:
    """One log line for a MAIN or CLEANUP choice."""
    if choice["phase"] == "cleanup":
        parts = []
        if any(choice["returned"]):
            parts.append(f"returned {_tokens(choice['returned'])}")
        if choice["noble"] is not None:
            parts.append(f"claimed noble #{choice['noble']}")
        return "; ".join(parts) if parts else "ended the turn"
    family = choice["family"]
    if family == "take":
        if not any(choice["delta"]):
            return "took no tokens (the null move at ten tokens)"
        taken = _tokens(choice["delta"])
        returned = _tokens(choice["delta"], -1)
        return f"took {taken}" + (
            f" and returned {returned}" if returned != "nothing" else ""
        )
    if family == "pass_forced":
        return "passed (no legal action)"
    if family == "reserve_blind":
        return f"reserved a tier-{choice['tier']} card from the deck" + (
            " +1 gold" if choice["delta"][5] > 0 else ""
        )
    card = catalog["cards"][choice["card"]]
    label = f"tier-{card['tier']} {card['bonus']} card #{card['id']} ({card['prestige']} pt)"
    if family == "reserve_visible":
        return f"reserved {label}" + (" +1 gold" if choice["delta"][5] > 0 else "")
    source = "from reserve" if family == "buy_reserved" else "from market"
    return f"bought {label} {source} paying {_tokens(choice['payment'])}"


__all__ = ["TOKEN_COLORS", "describe_choice"]

