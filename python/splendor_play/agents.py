"""The play host's agent contract: what a match asks of the side it does not own."""
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
    payload digest): game IDs carry it and archives group results by it."""

    label: str
    identity: str

    def summary(self) -> dict[str, Any]: ...


class MatchSetup(Protocol):
    """What every match of one host shares. `log_contract` is the name the composer persists
    logs of `match.VERSUS_LOG_SCHEMA` under (that name itself when it has no other);
    `search` and `leaf_batch` are the agent's configuration, logged verbatim as play
    conditions."""

    log_contract: str
    max_game_plies: int
    search: Mapping[str, Any]
    leaf_batch: int
    models: Sequence[PlayerRef]

    def model(self, label: str) -> PlayerRef: ...

    def logged_model(self, summary: Mapping[str, Any]) -> PlayerRef:
        """The offered actor a log's `model` summary names; rejects any other."""
        ...


class SearchJob:
    """One open-ended search, run as short slices on one retained tree.

    Each slice reports the search so far, so the agent's view is shown live; `end` stops
    the running slice and the slices after it. A move's `timer` ends its search by the
    agent's time allocation and the search's own progress.
    """

    def __init__(
        self,
        kind: str,
        revision: int,
        details: Mapping[str, Mapping[str, Any]],
        *,
        model_to_move: bool,
        concealed: bool,
        history_index: int | None = None,
    ) -> None:
        self.kind = kind
        self.revision = revision
        # The decision's legal choices by key, as `Episode.choice_details` renders them.
        self.details = details
        self.model_to_move = model_to_move
        # The model decides holding a card the human cannot identify: its report is the
        # model's knowledge, withheld from the human until the game ends.
        self.concealed = concealed
        # A review search's position, and the simulations it has completed.
        self.history_index = history_index
        self.simulations = 0
        self.done = threading.Event()
        self.slice: Stop | None = None
        self.timer: Timer | None = None

    def end(self) -> None:
        self.done.set()
        running = self.slice
        if running is not None:
            running.request()


class Agent(Protocol):
    """One model's play in one match, driven by the host.

    `root` detaches the agent's view of the episode's current decision, which outlives
    later selections; `search` runs on such a root until `job` ends, publishing each
    slice's JSON report with the simulations it completed, and returns the last report.
    `apply` plays the agent's retained selection on the live episode: the host has
    already discarded stale searches, and the agent must reject a decision its root was
    not taken from. Reports are display and evidence only, never commands.
    """

    def warm(self, episode: Episode) -> None: ...
    def root(self, episode: Episode) -> object: ...
    def timer(self, clock: Clock, now_ms: float) -> Timer: ...
    def search(self, job: SearchJob, root: object,
               publish: Callable[[dict[str, Any], int], None]) -> dict[str, Any]: ...
    def apply(self, episode: Episode) -> None: ...


AgentFactory = Callable[[PlayerRef], Agent]

__all__ = ["Agent", "AgentFactory", "MatchSetup", "PlayerRef", "SearchJob", "Stop", "Timer"]
