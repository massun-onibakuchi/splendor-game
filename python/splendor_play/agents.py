"""The play host's agent contract: what a match asks of the seats it does not drive itself."""
from __future__ import annotations

import threading
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from typing import Any, Protocol

from splendor_game import Clock, Episode

Publish = Callable[[str, int], None]


class PlayerRef(Protocol):
    """One offered actor. `identity` is stable across labels and paths (for example a
    payload digest), made of `[A-Za-z0-9._-]`: game IDs carry it and archives group results
    by it."""

    label: str
    identity: str

    def summary(self) -> dict[str, Any]: ...


class MatchSetup(Protocol):
    """What every match of one host shares. `log_contract` is the name the composer persists
    logs of `match.MATCH_LOG_SCHEMA` under (that name itself when it has no other);
    `conditions` (an agent's search configuration, for example) is logged verbatim."""

    log_contract: str
    max_game_plies: int
    conditions: Mapping[str, Any]
    actors: Sequence[PlayerRef]


class Job:
    """One job of an agent seat. `root_actor` is the job's decision's actor: the seat itself
    for its own moves, a human it ponders, or either in a finished game's review. Callbacks
    given to `on_end` run when the host ends the job."""

    def __init__(self, id: int, kind: str, seat: int, root_actor: int,
                 position: int | None = None) -> None:
        self.id = id
        self.kind = kind
        self.seat = seat
        self.root_actor = root_actor
        self.position = position
        self._ended = threading.Event()
        self._lock = threading.Lock()
        self._on_end: list[Callable[[], None]] = []

    @property
    def ended(self) -> bool:
        return self._ended.is_set()

    def on_end(self, callback: Callable[[], None]) -> None:
        with self._lock:
            if not self._ended.is_set():
                self._on_end.append(callback)
                return
        callback()

    def end(self) -> None:
        with self._lock:
            self._ended.set()
            callbacks, self._on_end = self._on_end, []
        for callback in callbacks:
            callback()


@dataclass(frozen=True)
class Move:
    """`choice` indexes the legal choices of the job's decision."""

    choice: int
    report: str | None = None


class Thought(Protocol):
    """A started job, run off the host's lock: a move job returns a `Move`, others `None`."""

    def run(self, publish: Publish) -> Move | None: ...


class Agent(Protocol):
    """One actor's play at one seat of one match, driven by the host.

    `start` runs under the host's lock with read-only views that expire when it returns.
    Reports are display and evidence only, never commands.
    """

    def warm(self, episode: Episode) -> None: ...
    def start(self, job: Job, episode: Episode, clock: Clock, now_ms: float) -> Thought: ...


# An agent for an offered actor at a seat.
AgentFactory = Callable[[PlayerRef, int], Agent]

__all__ = [
    "Agent", "AgentFactory", "Job", "MatchSetup", "Move", "PlayerRef", "Publish", "Thought",
]
