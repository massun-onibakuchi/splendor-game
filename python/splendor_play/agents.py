"""The play host's agent contract: what a match asks of the seats it does not drive itself."""
from __future__ import annotations

import threading
from collections.abc import Callable, Mapping, Sequence
from typing import Any, Protocol

from splendor_game import Clock, Episode


class Stop(Protocol):
    def request(self) -> None: ...


class Timer(Protocol):
    def end(self) -> str | None: ...


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


class SearchJob:
    """One open-ended search of an agent seat, run as short slices on one retained tree.

    Each slice reports the search so far, so the agent's view is shown live; `end` stops
    the running slice and the slices after it. A move's `timer` ends its search by the
    agent's time allocation and the search's own progress. `root_actor` is the searched
    decision's actor: the seat itself for its own moves, a human it ponders, or either in a
    finished game's review.
    """

    def __init__(self, id: int, kind: str, seat: int, root_actor: int,
                 position: int | None = None) -> None:
        self.id = id
        self.kind = kind
        self.seat = seat
        self.root_actor = root_actor
        self.position = position
        self.done = threading.Event()
        self.slice: Stop | None = None
        self.timer: Timer | None = None

    def end(self) -> None:
        self.done.set()
        running = self.slice
        if running is not None:
            running.request()


class Agent(Protocol):
    """One actor's play at one seat of one match, driven by the host.

    `root` detaches the agent's view of a decision from a read-only episode view, so the
    search outlives later selections; `search` runs on such a root until `job` ends,
    publishing each slice's JSON report text with the simulations it completed, and returns
    the last report. `selection` names the retained move's index among the live episode's
    legal choices and must reject an episode its root was not taken from. Reports are
    display and evidence only, never commands.
    """

    def warm(self, episode: Episode) -> None: ...
    def root(self, episode: Episode) -> object: ...
    def timer(self, clock: Clock, now_ms: float) -> Timer: ...
    def search(self, job: SearchJob, root: object, publish: Callable[[str, int], None]) -> str: ...
    def selection(self, episode: Episode) -> int: ...


# An agent for an offered actor at a seat.
AgentFactory = Callable[[PlayerRef, int], Agent]

__all__ = ["Agent", "AgentFactory", "MatchSetup", "PlayerRef", "SearchJob", "Stop", "Timer"]
