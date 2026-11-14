"""One live match: the native match host and the threads that run its agents' jobs.

`splendor::play::Match` owns the game, clocks, turn order, the agents' job schedule, analysis
visibility, the log and its replay. This driver supplies wall and monotonic time, runs each
scheduled job on its seat's agent, and persists every changed log.
"""

from __future__ import annotations

import json
import threading
import time
from collections.abc import Mapping
from typing import TYPE_CHECKING, Any

from splendor_game import Catalog
from splendor_play._native import HostMatch
from splendor_play.agents import Agent, AgentFactory, Job, MatchSetup, Thought

if TYPE_CHECKING:
    from splendor_play.archive import Archive

# The log schema the native host writes and restores. A composer persists logs under its own
# contract name for this schema (`MatchSetup.log_contract`).
MATCH_LOG_SCHEMA = "splendor-match-log-v1"


def _now() -> float:
    """The clock's time: monotonic milliseconds."""
    return time.monotonic() * 1000.0


def host_json(setup: MatchSetup) -> str:
    """The native host description of `setup`: its log contract, rules and offered actors."""
    return json.dumps({
        "log_contract": setup.log_contract,
        "max_game_plies": setup.max_game_plies,
        "conditions": dict(setup.conditions),
        "actors": [
            {"name": actor.label, "identity": actor.identity, "summary": actor.summary()}
            for actor in setup.actors
        ],
    })


class Match:
    """Every method holds `_lock`. One job runs at a time: each job's thread first joins its
    predecessor, outside the lock, and only the native host's current job may publish or
    select. A started match rewrites its full log to the archive after every change, so it
    can be restarted with `Match.restore`.
    """

    def __init__(
        self, native: HostMatch, setup: MatchSetup, agents: AgentFactory, archive: Archive | None
    ) -> None:
        self._native = native
        self._lock = threading.RLock()
        self._archive = archive
        offered = {actor.identity: actor for actor in setup.actors}
        self._agents: dict[int, Agent] = {
            seat: agents(offered[identity], seat)
            for seat, (kind, identity) in enumerate(native.seats())
            if kind == "agent"
        }
        # Agent sessions initialize (model compilation, caches) before any clock runs; a
        # finished match runs no clock.
        episode = native.episode()
        if not episode.is_terminal():
            for agent in self._agents.values():
                agent.warm(episode)
        self._job: Job | None = None
        self._last_thread: threading.Thread | None = None
        with self._lock:
            self._sync()

    @classmethod
    def create(
        cls, setup: MatchSetup, request: Mapping[str, Any], *, catalog: Catalog,
        agents: AgentFactory, archive: Archive | None,
    ) -> Match:
        native = HostMatch.create(catalog, host_json(setup), json.dumps(request), round(time.time() * 1000))
        return cls(native, setup, agents, archive)

    @classmethod
    def restore(
        cls, setup: MatchSetup, log: str, *, catalog: Catalog, agents: AgentFactory,
        archive: Archive | None,
    ) -> Match:
        """Rebuild a logged match by replaying its keys; an unfinished one is paused.

        The clock resumes from its archived turn-cumulative time; search trees are lost.
        """
        return cls(HostMatch.restore(catalog, host_json(setup), log, _now()), setup, agents, archive)

    @property
    def default_perspective(self) -> int:
        return self._native.default_perspective

    @property
    def game_id(self) -> str:
        return self._native.game_id

    # Commands -------------------------------------------------------------------

    def start(self) -> None:
        with self._lock:
            self._native.start(_now())
            self._sync()

    def pause(self) -> None:
        with self._lock:
            self._native.pause(_now())
            self._sync()

    def resume(self) -> None:
        with self._lock:
            self._native.resume(_now())
            self._sync()

    def close(self) -> None:
        """Stop any job and wait for it, so no evaluation outlives the match."""
        with self._lock:
            self._native.close(_now())
            self._sync()
            thread = self._last_thread
        # Outside the lock: the finishing job takes it to discard its result.
        if thread is not None:
            thread.join()

    def play(self, action_key: str, revision: int) -> None:
        """Commit the acting human's complete decision; composing it is the client's."""
        with self._lock:
            self._native.play(action_key, revision, _now())
            self._sync()

    def analyze(self, position: Any) -> None:
        """Analyze one reviewed position of a finished game; `None` stops the analysis."""
        with self._lock:
            self._native.analyze(position)
            self._sync()

    # Views ------------------------------------------------------------------------

    def state(self, perspective: int, since: int | None) -> bytes:
        """The board's state JSON for a seat; reading it adjudicates an expired turn."""
        with self._lock:
            body = self._native.state(perspective, since, _now())
            self._sync()
            return body

    def export(self, perspective: int) -> str:
        """Replayable log as a seat may see it: reports stay redacted while analysis is
        hidden, and another seat's concealed-card reports are withheld until the end."""
        with self._lock:
            return self._native.export_log(perspective, _now())

    def export_text(self, perspective: int) -> str:
        with self._lock:
            return self._native.export_text(perspective, _now())

    # Jobs -------------------------------------------------------------------------

    def _sync(self) -> None:
        """Align the running job with the native schedule; persist a changed log."""
        due = self._native.job()
        running = self._job
        if running is not None and (due is None or due.id != running.id):
            running.end()
            self._job = None
        if due is not None and self._job is None:
            self._launch(due)
        if self._archive is not None and self._native.take_unsaved():
            self._archive.save(self._native.game_id, self._native.archive_log(_now()))

    def _launch(self, due: Any) -> None:
        job = Job(due.id, due.kind, due.seat, due.root_actor, due.position)
        # The agent reads the job's decision and the clock now, under the lock, so draining
        # the previous job spends a move's own time.
        thought = self._agents[due.seat].start(
            job, self._native.job_episode(), self._native.clock(), _now())
        thread = threading.Thread(target=self._run, args=(job, thought, self._last_thread), daemon=True)
        self._job = job
        self._last_thread = thread
        thread.start()

    def _run(self, job: Job, thought: Thought, previous: threading.Thread | None) -> None:
        try:
            # One job at a time; native players are not reentrant.
            if previous is not None:
                previous.join()
            move = thought.run(lambda report, count: self._publish(job, report, count))
            with self._lock:
                if self._job is not job:
                    return
                if job.kind != "move":
                    self._job = None
                    self._native.finish(job.id, _now())
                elif move is None:
                    raise ValueError("a move job ended without a selection")
                else:
                    self._job = None
                    self._native.play_agent(job.id, move.choice, move.report, _now())
                self._sync()
        # Any failure surfaces on the board; the match pauses at its current decision.
        except Exception as error:
            with self._lock:
                if self._job is job:
                    self._job = None
                self._native.fail(job.id, str(error), _now())
                self._sync()

    def _publish(self, job: Job, report: str, work: int) -> None:
        with self._lock:
            self._native.publish(job.id, report, work)


__all__ = ["MATCH_LOG_SCHEMA", "Match", "host_json"]
