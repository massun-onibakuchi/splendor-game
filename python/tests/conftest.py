"""A live play host with deterministic scripted agents: no model, real native engine."""

from __future__ import annotations

import json
import threading
import time
from collections.abc import Mapping
from dataclasses import dataclass, field
from typing import Any

import pytest

from splendor_game import STANDARD_CATALOG, Clock, Episode
from splendor_play import MATCH_LOG_SCHEMA, Archive, Job, Move, Publish, make_server

MAX_GAME_PLIES = 160
# A scripted search's slices: a move ends once its budget passes, a review or a ponder
# once this many slices completed.
_SLICE_SECONDS = 0.005
_MOVE_SECONDS = 0.05
_SLICE_LIMIT = 40


@dataclass(frozen=True)
class Actor:
    label: str
    identity: str
    # A reporting actor 'searches' in reported slices; a silent one decides at once.
    reports: bool = True

    def summary(self) -> dict[str, Any]:
        return {"label": self.label, "identity": self.identity}


@dataclass(frozen=True)
class Setup:
    actors: tuple[Actor, ...]
    log_contract: str = MATCH_LOG_SCHEMA
    max_game_plies: int = MAX_GAME_PLIES
    conditions: Mapping[str, Any] = field(default_factory=lambda: {"search": "scripted"})


class _Thought:
    """Selects the decision's first legal choice: at once when silent, else after a sliced
    'search' whose move ends once its fixed budget passes."""

    def __init__(self, job: Job, keys: tuple[str, ...], reports: bool) -> None:
        self._job = job
        self._keys = keys
        self._reports = reports
        self._deadline = time.monotonic() + _MOVE_SECONDS

    def run(self, publish: Publish) -> Move | None:
        move = self._job.kind == "move"
        if not self._reports:
            return Move(0) if move else None
        slices = 0
        while True:
            time.sleep(_SLICE_SECONDS)
            slices += 1
            report = json.dumps({
                "selected_key": self._keys[0],
                "root_value": 0.25,
                "root_visits": slices,
                "top_candidates": [
                    {"action_key": key, "visits": slices if index == 0 else 0}
                    for index, key in enumerate(self._keys[:3])
                ],
            })
            publish(report, 1)
            spent = move and time.monotonic() >= self._deadline
            if self._job.ended or spent or slices >= _SLICE_LIMIT:
                return Move(0, report) if move else None


class ScriptedAgent:
    def __init__(self, actor: Actor, seat: int) -> None:
        self.actor = actor
        self.seat = seat

    def warm(self, episode: Episode) -> None:
        pass

    def start(self, job: Job, episode: Episode, clock: Clock, now_ms: float) -> _Thought:
        return _Thought(job, episode.choices(), self.actor.reports)


ACTOR = Actor("Scripted @1", "scripted-v1")
SILENT = Actor("Silent @1", "silent-v1", reports=False)


def _server(archive, static, max_game_plies: int):
    server = make_server(
        Setup((ACTOR, SILENT), max_game_plies=max_game_plies), ScriptedAgent, static=static,
        catalog_path=STANDARD_CATALOG, archive=Archive(archive, log_contract=MATCH_LOG_SCHEMA),
        host="127.0.0.1", port=0,
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


@pytest.fixture(scope="session")
def board_archive(tmp_path_factory: pytest.TempPathFactory):
    return tmp_path_factory.mktemp("match-games")


@pytest.fixture(scope="session")
def board_static(tmp_path_factory: pytest.TempPathFactory):
    static = tmp_path_factory.mktemp("board")
    (static / "index.html").write_text("<!doctype html><title>board</title>\n", encoding="utf-8")
    (static / "app.js").write_text("export {};\n", encoding="utf-8")
    (static / "nested").mkdir()
    (static / "nested" / "hidden.js").write_text("export {};\n", encoding="utf-8")
    return static


@pytest.fixture(scope="session")
def board_url(board_archive, board_static):
    server = _server(board_archive, board_static, MAX_GAME_PLIES)
    yield f"http://127.0.0.1:{server.server_address[1]}/"
    server.shutdown()
    server.server_close()


@pytest.fixture(scope="session")
def short_board_url(tmp_path_factory: pytest.TempPathFactory, board_static):
    """A host whose games end at a short ply cap, so agent-only games finish quickly."""
    server = _server(tmp_path_factory.mktemp("short-games"), board_static, 12)
    yield f"http://127.0.0.1:{server.server_address[1]}/"
    server.shutdown()
    server.server_close()
