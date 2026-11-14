"""The play host's agent contract: what a match asks of the seats it does not drive itself.

An agent decides; how is its own. The host starts one job at a time for an agent seat (its
own move, a pondering of a human's decision, or a finished game's review); a move returns a
choice index, and reports are optional evidence for display.
"""
from __future__ import annotations

import threading
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from typing import Any, Protocol

from splendor_game import Clock, Episode

# Publishes a job's progress for display: a JSON report object as text, and the work units
# (simulations, for a search) spent since the previous report.
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
    """One job of an agent seat: `kind` is `move`, `ponder` or `review`; `root_actor` is the
    actor of the job's decision (the seat itself for its own moves, a human it ponders, or
    either in a finished game's review); `position` is a review's history position.

    The host ends a job that is no longer due; `ended` says so, and every callback given to
    `on_end` runs once at the end, from the ending thread, so an agent can interrupt work in
    progress.
    """

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
        """Run `callback` when the job ends, at once if it already has."""
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
    """A move job's selection: an index into the legal choices of the job's decision, and the
    agent's final JSON report object as text, if it reports."""

    choice: int
    report: str | None = None


class Thought(Protocol):
    """One started job, run off the host's lock. It returns a `Move` for a move job and
    `None` otherwise, once it decides or its job ends; `publish` shows progress meanwhile."""

    def run(self, publish: Publish) -> Move | None: ...


class Agent(Protocol):
    """One actor's play at one seat of one match, driven by the host.

    `warm` prepares the agent (a model session, caches) before any clock runs. `start` runs
    under the host's lock: it takes what the job needs from read-only views of the job's
    decision and the match clock at `now_ms` -- views that expire when it returns -- and
    returns the `Thought` the host then runs. The host binds a move to the job's decision,
    so a stale job's move is discarded.
    """

    def warm(self, episode: Episode) -> None: ...
    def start(self, job: Job, episode: Episode, clock: Clock, now_ms: float) -> Thought: ...


# An agent for an offered actor at a seat.
AgentFactory = Callable[[PlayerRef, int], Agent]

__all__ = [
    "Agent", "AgentFactory", "Job", "MatchSetup", "Move", "PlayerRef", "Publish", "Thought",
]
