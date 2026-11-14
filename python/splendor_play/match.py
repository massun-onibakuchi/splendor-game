"""One human-versus-model match: turn order, clock, pause, model search, and its log.

The match runs the canonical staged-action turn. Everything the human receives is the
human seat's observation; the model's concealed cards reach the human only through what
the rules reveal. The native `GameClock` keeps time; the agent's timer ends each of the
model's searches.
"""

from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, Mapping

from splendor_game import (
    GAME_CONTRACT, STAGED_ACTION_CONTRACT, Catalog, Clock, Episode, derive64,
)
from splendor_play._fields import closed, uint
from splendor_play.actions import describe_choice
from splendor_play.agents import Agent, MatchSetup, SearchJob

if TYPE_CHECKING:
    from splendor_play.archive import Archive

# The log schema this host writes and restores. A composer persists logs under its own
# contract name for this schema (`MatchSetup.log_contract`).
VERSUS_LOG_SCHEMA = "splendor-versus-log-v1"
SETUP_SEED_DOMAIN = "versus-setup"
EPISODE_SEED_DOMAIN = "versus-episode"
_TIMEOUT_POLICIES = ("flag", "forfeit")


def _now() -> float:
    """The clock's time: monotonic milliseconds."""
    return time.monotonic() * 1000.0


@dataclass(frozen=True)
class MatchSettings:
    model: str
    human_first: bool
    # `{kind: turn, human_seconds, model_seconds, timeout}` or `{kind: bank, seconds}`.
    time_control: Mapping[str, Any]
    ponder: bool
    show_analysis: bool
    seed: int


def _resolve_time_control(raw: Any) -> dict[str, Any]:
    """A fresh allowance every turn, the human's overtime flagged or forfeit; or one bank
    per player for the whole game, with no increment, run out to lose (docs/specs/play.md,
    clocks). The native clock checks the values."""
    if not isinstance(raw, Mapping) or raw.get("kind") not in ("turn", "bank"):
        raise ValueError("time_control kind must be turn or bank")
    if raw["kind"] == "bank":
        closed(raw, {"kind", "seconds"}, "bank time control")
        return {"kind": "bank", "seconds": uint(raw["seconds"], 32, "seconds", positive=True)}
    closed(raw, {"kind", "human_seconds", "model_seconds", "timeout"}, "turn time control")
    if raw["timeout"] not in _TIMEOUT_POLICIES:
        raise ValueError("timeout must be flag or forfeit")
    return {
        "kind": "turn",
        "human_seconds": uint(raw["human_seconds"], 32, "human_seconds", positive=True),
        "model_seconds": uint(raw["model_seconds"], 32, "model_seconds", positive=True),
        "timeout": raw["timeout"],
    }


def _time_control_text(control: Mapping[str, Any]) -> str:
    if control["kind"] == "bank":
        return f"bank {control['seconds']} s each, no increment, out of time loses"
    return (
        f"human {control['human_seconds']} s, model {control['model_seconds']} s per turn, "
        f"timeout {control['timeout']}"
    )


def resolve_match_settings(raw: Any, setup: MatchSetup) -> MatchSettings:
    closed(
        raw,
        {"model", "human_first", "time_control", "ponder", "show_analysis", "seed"},
        "match settings",
    )
    setup.model(raw["model"])
    for name in ("human_first", "ponder", "show_analysis"):
        if not isinstance(raw[name], bool):
            raise ValueError(f"{name} must be boolean")
    seed = raw["seed"]
    # A browser cannot carry every uint64 as a JSON number; a decimal string is exact.
    if isinstance(seed, str) and seed.isascii() and seed.isdecimal():
        seed = int(seed)
    return MatchSettings(
        model=raw["model"],
        human_first=raw["human_first"],
        time_control=_resolve_time_control(raw["time_control"]),
        ponder=raw["ponder"],
        show_analysis=raw["show_analysis"],
        seed=uint(seed, 64, "seed"),
    )


def load_catalog_view(path: str | Path) -> dict[str, Any]:
    """A catalog the engine accepts, as display records indexed by card and noble ID."""
    raw = json.loads(Path(path).read_bytes())
    cards = [
        {
            "id": card_id,
            "tier": tier,
            "bonus": bonus,
            "prestige": prestige,
            "cost": cost,
        }
        for card_id, tier, bonus, prestige, cost in raw["cards"]
    ]
    nobles = [
        {"id": noble_id, "prestige": prestige, "requirements": requirements}
        for noble_id, prestige, requirements in raw["nobles"]
    ]
    if [card["id"] for card in cards] != list(range(len(cards))) or [
        noble["id"] for noble in nobles
    ] != list(range(len(nobles))):
        raise ValueError("catalog IDs must be dense and ordered")
    return {"cards": cards, "nobles": nobles}


class Match:
    """Every method holds `_lock`; the model searches a detached copy of a decision.

    `revision` counts completed decisions. A human submission names the revision it was
    composed at, so a stale or repeated confirmation never applies to a later decision.

    One search runs at a time: each search thread first joins its predecessor, outside the
    lock, and only the current `_job` may publish its result. The model's move search is
    stopped when its turn's clock runs out; pondering is stopped when the human commits.
    Pausing stops either and discards its selection; the retained tree carries the work.
    Each search slice publishes `_live`, the model's current view of the decision.
    A finished game has no live search, so a review search of any past position may
    take the player; it publishes `_review` and is never logged.

    A started match rewrites its full log to the archive after every change, so it can
    be restarted from that log with `Match.restore`.
    """

    def __init__(
        self,
        setup: MatchSetup,
        settings: MatchSettings,
        *,
        catalog: Catalog,
        catalog_view: Mapping[str, Any],
        agent: Agent,
        archive: Archive | None,
        game_id: str | None = None,
        started_at: str | None = None,
        expected_trace: list[Mapping[str, Any]] | None = None,
    ) -> None:
        model = setup.model(settings.model)
        self._lock = threading.RLock()
        self._setup = setup
        self._settings = settings
        self._catalog = catalog_view
        self._model = model
        self._archive = archive
        now = time.time()
        self._started_at = started_at or time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime(now)
        )
        self._game_id = game_id or (
            time.strftime("%Y%m%dT%H%M%S", time.gmtime(now))
            + f"{round(now * 1000) % 1000:03d}Z-{model.identity}-{settings.seed}"
        )
        self._setup_seed = derive64(settings.seed, SETUP_SEED_DOMAIN)
        self._episode_seed = derive64(settings.seed, EPISODE_SEED_DOMAIN)
        self._game_catalog = catalog
        self._episode = Episode(
            catalog,
            setup.max_game_plies,
            self._setup_seed,
            self._episode_seed,
        )
        if expected_trace is not None:
            self._episode.expect_trace(expected_trace)
        self._agent = agent
        # Agent session initialization (model compilation, caches) precedes every clock.
        self._agent.warm(self._episode)
        # The starting seat acts at the first decision; the board is always the human's view.
        starting = self._episode.current_player()
        self._human_seat = starting if settings.human_first else 1 - starting
        self._board = json.loads(self._episode.observe(self._human_seat))
        seat_count = len(self._board["players"])
        self._nobles: list[list[int]] = [[] for _ in range(seat_count)]
        self._purchases: list[list[int]] = [[] for _ in range(seat_count)]
        self._steps: list[dict[str, Any]] = []
        self._history = [self._snapshot()]
        self._status = "ready"
        self._result: dict[str, Any] | None = None
        self._version = 0
        self._pauses = 0
        # Seat-indexed allowances: one bank each, or each side's turn allowance.
        control = settings.time_control
        if control["kind"] == "bank":
            allowance = [control["seconds"]] * seat_count
        else:
            allowance = [
                control["human_seconds"] if seat == self._human_seat else control["model_seconds"]
                for seat in range(seat_count)
            ]
        self._clock = Clock(
            control["kind"],
            [seconds * 1000 for seconds in allowance],
            [
                control["kind"] == "turn"
                and control["timeout"] == "forfeit"
                and seat == self._human_seat
                for seat in range(seat_count)
            ],
        )
        self._thinking_since: float | None = None
        self._job: SearchJob | None = None
        self._last_thread: threading.Thread | None = None
        # Simulations spent on the decision the model is to move at, across paused
        # segments, and spent pondering since the model's previous move.
        self._move_work = 0
        self._ponder_work = 0
        # The latest slice's summary of the current decision, cleared when it is decided.
        self._live: dict[str, Any] | None = None
        self._last_search: dict[str, Any] | None = None
        # The latest slice of a finished game's review search, with its position.
        self._review: dict[str, Any] | None = None
        self._error: str | None = None
        self._record(self._episode.drain_trace(), elapsed_ms=None, search=None)

    @classmethod
    def restore(
        cls,
        setup: MatchSetup,
        log: Mapping[str, Any],
        *,
        catalog: Catalog,
        catalog_view: Mapping[str, Any],
        agent: Agent,
        archive: Archive | None,
    ) -> Match:
        """Rebuild a logged match by replaying its keys; an unfinished one is paused.

        The clock resumes from its archived turn-cumulative time; the search tree is lost.
        """
        if log.get("log_contract") != setup.log_contract:
            raise ValueError(f"not a {setup.log_contract} log")
        # Resume identity: the restarted game continues under the logged ply cap, which
        # decides when it ends. Rules and catalog need no comparison: `_replay` requires
        # the keys to regenerate the log's own steps.
        if log["max_game_plies"] != setup.max_game_plies:
            raise ValueError("the log was played under a different ply cap")
        settings = resolve_match_settings(
            {
                "model": setup.logged_model(log["model"]).label,
                **{
                    name: log[name]
                    for name in (
                        "human_first",
                        "time_control",
                        "ponder",
                        "show_analysis",
                        "seed",
                    )
                },
            },
            setup,
        )
        match = cls(
            setup,
            settings,
            catalog=catalog,
            catalog_view=catalog_view,
            agent=agent,
            archive=None,
            game_id=log["game_id"],
            started_at=log["started_at"],
            expected_trace=log["steps"],
        )
        match._replay(log)
        match._archive = archive
        match._persist()
        return match

    # Commands -----------------------------------------------------------------

    def start(self) -> None:
        with self._lock:
            if self._status != "ready":
                raise ValueError("the match has already started")
            self._status = "running"
            self._continue()

    def pause(self) -> None:
        with self._lock:
            self._expire()
            if self._status != "running":
                raise ValueError("only a running match can pause")
            self._halt()
            self._clock.stop(_now())
            self._status = "paused"
            self._pauses += 1
            self._touch()
            self._persist()

    def resume(self) -> None:
        with self._lock:
            if self._status != "paused":
                raise ValueError("only a paused match can resume")
            self._status = "running"
            self._error = None
            self._continue()

    def close(self) -> None:
        """Stop any search and wait for it, so no evaluation outlives the match."""
        with self._lock:
            self._halt()
            self._clock.stop(_now())
            self._expire()
            if self._status == "running":
                self._status = "paused"
            self._touch()
            self._persist()
            thread = self._last_thread
        # Outside the lock: the finishing search takes it to discard its result.
        if thread is not None:
            thread.join()

    def play(self, action_key: str, revision: int) -> None:
        """Commit the human's complete decision; composing it is the client's."""
        with self._lock:
            self._expire()
            if self._status != "running":
                raise ValueError("the match is not running")
            # Decision identity, not metadata: action keys recur across turns (the same
            # take is often legal again), so without it a repeated or late confirmation
            # would be applied to a later decision the human never saw.
            if revision != self._revision():
                raise ValueError(
                    "the decision has changed since this action was composed"
                )
            if self._episode.current_player() != self._human_seat:
                raise ValueError("it is not the human player's turn")
            if action_key not in self._episode.choices():
                raise ValueError("illegal action for this decision")
            # The model's pondered view of this decision, recorded with the human's move.
            ponder = (
                self._live
                if self._live is not None and self._live["kind"] == "ponder"
                else None
            )
            self._halt()
            self._select(
                action_key, elapsed_ms=self._decided(), search=None, ponder=ponder
            )
            self._continue()

    def analyze(self, history_index: Any) -> None:
        """Search one reviewed position of a finished game; `None` stops the search.

        Display only, never logged. The game is over, so its retained tree has no
        further use: the search reuses it where the position matches, else starts fresh.
        """
        with self._lock:
            if self._status != "finished":
                raise ValueError("positions are analyzed once the game is over")
            if history_index is not None and (
                not isinstance(history_index, int)
                or isinstance(history_index, bool)
                or not 0 <= history_index < len(self._history)
            ):
                raise ValueError("no such position")
            self._halt()
            self._review = None
            if history_index is None:
                return
            episode = self._episode_at(history_index)
            if not episode.is_terminal():
                self._launch("review", episode, history_index=history_index)

    # Views --------------------------------------------------------------------

    def state(self) -> dict[str, Any]:
        with self._lock:
            self._expire()
            human_to_move = (
                self._status == "running"
                and self._episode.current_player() == self._human_seat
            )
            return {
                "version": self._version,
                "revision": self._revision(),
                "status": self._status,
                "error": self._error,
                "settings": self._settings_view(),
                "seats": self._seats(),
                "search": self._setup.search,
                "max_game_plies": self._setup.max_game_plies,
                "board": self._board,
                "choices": (
                    json.loads(self._episode.choice_details()) if human_to_move else []
                ),
                "takes": json.loads(self._episode.token_takes())
                if human_to_move
                else [],
                "clock": self._clock_view(),
                "thinking": {
                    "active": self._thinking_since is not None,
                    "pondering": self._job is not None and self._job.kind == "ponder",
                    "elapsed_ms": (
                        None
                        if self._thinking_since is None
                        else round((time.monotonic() - self._thinking_since) * 1000)
                    ),
                    # The model's view so far: its own decision, or the pondered one.
                    "live": self._player_report(self._live) if self._analysis_visible() else None,
                    # A finished game's analysis of a reviewed position; whether it is
                    # still searching.
                    "review": self._player_report(self._review),
                    "reviewing": self._job is not None and self._job.kind == "review",
                },
                "last_search": (
                    self._player_report(self._last_search) if self._analysis_visible() else None
                ),
                # Copies: entries are never mutated once published, the lists grow.
                "log": [self._visible_step(step) for step in self._steps],
                "history": list(self._history),
                "pauses": self._pauses,
                "result": self._result,
            }

    def export(self) -> dict[str, Any]:
        """Replayable log: the seeds and every discretionary key reproduce the match.

        Search reports stay redacted while the match hides analysis from the human, and a
        model report made with a card the human cannot see is withheld until the end.
        """
        with self._lock:
            return self._log(player=True)

    def _log(self, *, player: bool) -> dict[str, Any]:
        return {
            "log_contract": self._setup.log_contract,
            "game_contract": GAME_CONTRACT,
            "candidate_contract": STAGED_ACTION_CONTRACT,
            "catalog": {
                "contract": self._game_catalog.contract,
                "payload_sha256": self._game_catalog.payload_sha256,
            },
            "max_game_plies": self._setup.max_game_plies,
            "seed_domains": {
                "setup_seed": SETUP_SEED_DOMAIN,
                "episode_seed": EPISODE_SEED_DOMAIN,
            },
            **self._settings_view(),
            "seats": self._seats(),
            "search": self._setup.search,
            "leaf_batch": self._setup.leaf_batch,
            "pauses": self._pauses,
            "steps": [self._visible_step(step) if player else step for step in self._steps],
            "pending_elapsed_ms": (
                None if self._episode.is_terminal() else self._clock.used_ms(_now())
            ),
            "result": self._result,
        }

    def export_text(self) -> str:
        export = self.export()
        names = [seat["name"] for seat in export["seats"]]
        lines = [
            f"Splendor: Human vs {export['model']['label']} ({self._model.identity})",
            f"seed {export['seed']} (setup {export['setup_seed']}, "
            f"episode {export['episode_seed']}); {_time_control_text(export['time_control'])}; "
            f"pondering {'on' if export['ponder'] else 'off'}; analysis shown during play: "
            f"{'yes' if export['show_analysis'] else 'no'}; pauses {export['pauses']}",
        ]
        for step in export["steps"]:
            if (
                step["forced"]
                and step["choice"]["phase"] == "cleanup"
                and not (
                    any(step["choice"]["returned"])
                    or step["choice"]["noble"] is not None
                )
            ):
                continue
            note = " (forced)" if step["forced"] else ""
            if step["overtime"]:
                note += " (over time)"
            lines.append(
                f"Turn {step['game_ply'] + 1} {names[step['actor']]}: {step['text']}{note}"
            )
        result = export["result"]
        if result is not None:
            lines.append(f"Result: {self._result_text(result, names)}")
        return "\n".join(lines) + "\n"

    # Internals ----------------------------------------------------------------

    def _revision(self) -> int:
        return len(self._history) - 1

    def _analysis_visible(self) -> bool:
        return self._settings.show_analysis or self._status == "finished"

    def _visible_step(self, step: Mapping[str, Any]) -> Mapping[str, Any]:
        if not self._analysis_visible():
            return _redacted(step)
        search = self._player_report(step["search"])
        return step if search is step["search"] else {**step, "search": search}

    def _player_report(self, report: Mapping[str, Any] | None) -> Mapping[str, Any] | None:
        """`report` as the human may see it: a model report made while the model held a
        card the human cannot identify is only its presence until the game is finished,
        when every report is shown in full."""
        if report is None or self._status == "finished" or not report.get("concealed"):
            return report
        return {
            **({"kind": report["kind"]} if "kind" in report else {}),
            "concealed": True,
        }

    def _concealed_from(self, episode: Episode) -> bool:
        """Whether the model holds a reserve the human cannot identify at `episode`."""
        board = json.loads(episode.observe(self._human_seat))
        return any(
            reserve["card_id"] is None
            for seat, player in enumerate(board["players"])
            if seat != self._human_seat
            for reserve in player["reserves"]
        )

    def _persist(self) -> None:
        """Rewrite the archived log, analysis included; a match never started has none."""
        if self._archive is not None and self._status != "ready":
            self._archive.save(self._log(player=False))

    def _episode_at(self, history_index: int) -> Episode:
        """A fresh episode replayed to a history snapshot; the native one cannot rewind.

        Snapshot `k` follows the `k`-th chosen step, whose `history_index` is `k`.
        """
        episode = Episode(
            self._game_catalog,
            self._setup.max_game_plies,
            self._setup_seed,
            self._episode_seed,
        )
        for step in self._steps:
            if not step["forced"] and step["history_index"] <= history_index:
                episode.select(step["action_key"])
        return episode

    def _replay(self, log: Mapping[str, Any]) -> None:
        """Select every logged choice, then adopt the logged clock and search records."""
        logged = log["steps"]
        for index, step in enumerate(logged):
            if step["forced"]:
                continue
            if self._episode.is_terminal() or step["action_key"] not in (
                self._episode.choices()
            ):
                raise ValueError(f"log step {index + 1} is not legal at its position")
            self._select(step["action_key"], elapsed_ms=None, search=None)
        # The game binding verified every native trace batch before rendering it.
        self._episode.finish_replay()
        for mine, theirs in zip(self._steps, logged, strict=True):
            for name in _ADOPTED_STEP_FIELDS:
                mine[name] = theirs[name]
            if not mine["forced"]:
                # Each decision's turn-cumulative time rebuilds the banks.
                self._clock.record(
                    mine["actor"], mine["game_ply"], uint(mine["elapsed_ms"], 64, "elapsed_ms")
                )
        searches = [step["search"] for step in self._steps if step["search"]]
        self._last_search = searches[-1] if searches else None
        self._pauses = log["pauses"]
        if self._episode.is_terminal():
            if log["result"] is not None and not _same_json(
                log["result"], self._board["terminal"]
            ):
                raise ValueError("the log's result is not its replayed game's")
            self._status, self._result = "finished", self._board["terminal"]
            self._touch()
            return
        # Align with the pending turn, then adopt its time since the last decision too.
        now = _now()
        self._clock.run(self._episode.current_player(), self._episode.game_ply(), now)
        self._clock.stop(now)
        pending = uint(log["pending_elapsed_ms"], 64, "pending_elapsed_ms")
        if pending < self._clock.used_ms(now):
            raise ValueError("pending time precedes the turn's last logged decision")
        self._clock.record(self._episode.current_player(), self._episode.game_ply(), pending)
        if log["result"] is not None:
            # An unfinished game can only have ended on the clock, at its pending decision.
            loser = self._episode.current_player()
            if not (
                self._clock.loses_on_time(loser)
                and _same_json(log["result"], self._episode.timeout_result(loser))
            ):
                raise ValueError("the log's result is not its replayed game's")
            self._status, self._result = "finished", self._episode.timeout_result(loser)
        else:
            self._status = "paused"
        self._touch()

    def _settings_view(self) -> dict[str, Any]:
        return {
            "game_id": self._game_id,
            "started_at": self._started_at,
            "seed": str(self._settings.seed),
            "setup_seed": str(self._setup_seed),
            "episode_seed": str(self._episode_seed),
            "time_control": dict(self._settings.time_control),
            "ponder": self._settings.ponder,
            "show_analysis": self._settings.show_analysis,
            "human_first": self._settings.human_first,
            "human_seat": self._human_seat,
            "model": self._model.summary(),
        }

    def _seats(self) -> list[dict[str, Any]]:
        return [
            {"seat": seat, "kind": "human", "name": "Human"}
            if seat == self._human_seat
            else {"seat": seat, "kind": "model", "name": self._model.label}
            for seat in range(len(self._board["players"]))
        ]

    @staticmethod
    def _result_text(result: Mapping[str, Any], names: list[str]) -> str:
        winner = result["winner_seat"]
        outcome = "draw" if winner is None else f"{names[winner]} wins"
        return f"{outcome} ({result['reason']}, prestige {result['final_prestige']})"

    def _touch(self) -> None:
        self._version += 1

    def _clock_view(self) -> dict[str, Any]:
        """The moving clock a client animates: every seat's time left now, and whose
        turn runs it."""
        now = _now()
        return {
            "kind": self._settings.time_control["kind"],
            "actor": self._clock.actor(),
            "running": self._clock.running(),
            "remaining_ms": [
                self._clock.remaining_ms(seat, now) for seat in range(len(self._board["players"]))
            ],
        }

    def _decided(self) -> int:
        """Stops the mover's clock at its decision: the turn's time so far."""
        now = _now()
        self._clock.stop(now)
        return self._clock.used_ms(now)

    def _expire(self) -> None:
        """A turn past its limit ends the match on time when that loses its seat."""
        if self._status != "running":
            return
        now = _now()
        result = self._episode.adjudicate_timeout(self._clock, now)
        if result is None:
            return
        self._halt()
        self._clock.stop(now)
        self._status = "finished"
        self._result = result
        self._touch()
        self._persist()

    def _continue(self) -> None:
        """Run the mover's clock and the model's search: its move, or pondering."""
        if self._episode.is_terminal():
            self._halt()
            self._clock.stop(_now())
            self._status = "finished"
            self._result = self._board["terminal"]
            self._touch()
            self._persist()
            return
        actor = self._episode.current_player()
        # One turn's time covers its MAIN and CLEANUP decisions.
        self._clock.run(actor, self._episode.game_ply(), _now())
        if self._job is None:
            if actor != self._human_seat:
                self._launch("move")
            elif self._settings.ponder:
                self._launch("ponder")
        self._touch()
        self._persist()

    def _launch(
        self,
        kind: str,
        episode: Episode | None = None,
        *,
        history_index: int | None = None,
    ) -> None:
        """Search a decision after the previous search has finished: the current one,
        or a review search's replayed position."""
        episode = self._episode if episode is None else episode
        details = {
            choice["key"]: choice for choice in json.loads(episode.choice_details())
        }
        model_to_move = episode.current_player() != self._human_seat
        job = SearchJob(
            kind,
            self._revision(),
            details,
            model_to_move=model_to_move,
            concealed=model_to_move and self._concealed_from(episode),
            history_index=history_index,
        )
        if kind == "move":
            # The timer runs from now, so draining the previous search spends the
            # model's own time.
            job.timer = self._agent.timer(self._clock, _now())
            self._thinking_since = time.monotonic()
        thread = threading.Thread(
            target=self._run,
            args=(job, self._agent.root(episode), self._last_thread),
            daemon=True,
        )
        self._job = job
        self._last_thread = thread
        thread.start()

    def _halt(self) -> None:
        """Stop the current search; whatever it returns is discarded."""
        job, self._job = self._job, None
        self._thinking_since = None
        if job is not None:
            job.end()

    def _run(
        self,
        job: SearchJob,
        root: object,
        previous: threading.Thread | None,
    ) -> None:
        try:
            # One search at a time; the native player is not reentrant.
            if previous is not None:
                previous.join()
            summary = self._agent.search(
                job, root, lambda summary, count: self._publish_search(job, summary, count)
            )
        # Any failure surfaces in the UI; the match pauses at its current decision.
        except Exception as error:
            with self._lock:
                if self._job is not job:
                    return
                self._halt()
                self._error = f"model search failed: {error}"
                if self._status == "running":
                    self._clock.stop(_now())
                    self._status = "paused"
                self._touch()
            return
        with self._lock:
            if self._job is not job:
                return
            # Completing root readiness past a bank's end loses on time, unplayed.
            self._expire()
            if self._job is not job:
                return
            self._job = None
            self._thinking_since = None
            if job.kind in ("ponder", "review"):
                # The root-visit limit ended pondering before the human moved, or
                # filled a review search's tree.
                self._touch()
                return
            self._play_model(job, summary)
            self._continue()

    def _publish_search(self, job: SearchJob, summary: dict[str, Any], simulations: int) -> None:
        with self._lock:
            # A move search's segments all belong to its decision; pondering belongs
            # to the model's next move, whose search always starts after this one.
            if job.kind == "review":
                job.simulations += simulations
                if self._job is job:
                    self._review = {
                        **summary,
                        "concealed": job.concealed,
                        "kind": "move" if job.model_to_move else "ponder",
                        "history_index": job.history_index,
                        "simulations": job.simulations,
                    }
            else:
                if job.kind == "ponder":
                    self._ponder_work += simulations
                elif job.revision == self._revision():
                    self._move_work += simulations
                if self._job is job:
                    self._live = {
                        **summary,
                        "concealed": job.concealed,
                        "kind": job.kind,
                        "simulations": (
                            self._move_work
                            if job.kind == "move"
                            else self._ponder_work
                        ),
                    }

    def _play_model(self, job: SearchJob, summary: Mapping[str, Any]) -> None:
        elapsed_ms = self._decided()
        search = {
            **summary,
            "concealed": job.concealed,
            # This decision's own simulations, the human-turn simulations pondered
            # before it; `root_visits` includes any inherited from the tree.
            "simulations": self._move_work,
            "pondered_simulations": self._ponder_work,
            "think_ms": elapsed_ms,
        }
        self._last_search = search
        self._move_work = 0
        self._ponder_work = 0
        self._agent.apply(self._episode)
        self._selected(elapsed_ms=elapsed_ms, search=search)

    def _select(
        self,
        action_key: str,
        *,
        elapsed_ms: int | None,
        search: Mapping[str, Any] | None,
        ponder: Mapping[str, Any] | None = None,
    ) -> None:
        # Only the human's turn allowance can be overrun and played on (`flag`).
        overtime = self._clock.expired(_now()) and self._clock.actor() == self._human_seat
        self._episode.select(action_key)
        self._selected(elapsed_ms=elapsed_ms, search=search, ponder=ponder, overtime=overtime)

    def _selected(
        self, *, elapsed_ms: int | None, search: Mapping[str, Any] | None,
        ponder: Mapping[str, Any] | None = None, overtime: bool = False,
    ) -> None:
        self._board = json.loads(self._episode.observe(self._human_seat))
        self._record(
            self._episode.drain_trace(),
            elapsed_ms=elapsed_ms,
            search=search,
            ponder=ponder,
            overtime=overtime,
        )
        self._history.append(self._snapshot())
        self._live = None
        self._touch()

    def _record(
        self,
        trace: list[dict[str, Any]],
        *,
        elapsed_ms: int | None,
        search: Mapping[str, Any] | None,
        ponder: Mapping[str, Any] | None = None,
        overtime: bool = False,
    ) -> None:
        """Log a select's steps: the chosen one first, then any forced ones it caused."""
        for position, step in enumerate(trace):
            choice = json.loads(step["choice"])
            actor = step["actor"]
            if choice["phase"] == "main" and choice["family"] in {
                "buy_visible",
                "buy_reserved",
            }:
                self._purchases[actor].append(choice["card"])
            if choice["phase"] == "cleanup" and choice["noble"] is not None:
                self._nobles[actor].append(choice["noble"])
            chosen = position == 0 and not step["forced"]
            text = describe_choice(choice, self._catalog)
            if step["refill_card"] is not None:
                text += f"; market refilled with card #{step['refill_card']}"
            self._steps.append(
                {
                    "index": len(self._steps) + 1,
                    "game_ply": step["game_ply"],
                    "actor": actor,
                    "phase": choice["phase"],
                    "action_key": step["action_key"],
                    "forced": step["forced"],
                    "choice": choice,
                    "refill_slot": step["refill_slot"],
                    "refill_card": step["refill_card"],
                    "text": text,
                    "elapsed_ms": elapsed_ms if chosen else None,
                    "overtime": overtime and chosen,
                    # The model's search of its own decision, or its pondered view of
                    # the human's.
                    "search": search if chosen else None,
                    "ponder": ponder if chosen else None,
                    "history_index": len(self._history),
                }
            )

    def _snapshot(self) -> dict[str, Any]:
        return {
            "board": self._board,
            "nobles": [list(nobles) for nobles in self._nobles],
            "purchases": [list(cards) for cards in self._purchases],
        }


def _redacted(step: Mapping[str, Any]) -> Mapping[str, Any]:
    if step["search"] is None and step["ponder"] is None:
        return step
    return {**step, "search": None, "ponder": None}


# Clock and agent evidence are adopted; game facts and presentation are regenerated.
_ADOPTED_STEP_FIELDS = ("elapsed_ms", "overtime", "search", "ponder")




def _same_json(left: Any, right: Any) -> bool:
    """Equal as logged JSON, where a native tuple and its logged list agree."""
    return json.dumps(left, sort_keys=True) == json.dumps(right, sort_keys=True)


__all__ = [
    "Match",
    "MatchSettings",
    "VERSUS_LOG_SCHEMA",
    "load_catalog_view",
    "resolve_match_settings",
]
