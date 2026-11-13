"""A live play host with a deterministic scripted agent: no model, real native engine."""

from __future__ import annotations

import threading
import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from typing import Any

import pytest

from splendor_game import STANDARD_CATALOG, Clock, Episode
from splendor_play import VERSUS_LOG_SCHEMA, Archive, SearchJob, make_server

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

    def summary(self) -> dict[str, Any]:
        return {"label": self.label, "identity": self.identity}


@dataclass(frozen=True)
class Setup:
    models: tuple[Actor, ...]
    log_contract: str = VERSUS_LOG_SCHEMA
    max_game_plies: int = MAX_GAME_PLIES
    search: Mapping[str, Any] = field(default_factory=lambda: {"kind": "scripted"})
    leaf_batch: int = 1

    def model(self, label: str) -> Actor:
        for model in self.models:
            if model.label == label:
                return model
        raise ValueError(f"unknown model: {label}")

    def logged_model(self, summary: Mapping[str, Any]) -> Actor:
        for model in self.models:
            if model.identity == summary["identity"]:
                return model
        raise ValueError(f"this server does not offer the model {summary['identity']}")


class _Budget:
    """Ends a move's search once its fixed budget has passed."""

    def __init__(self) -> None:
        self._deadline = time.monotonic() + _MOVE_SECONDS

    def end(self) -> str | None:
        return "budget" if time.monotonic() >= self._deadline else None


class ScriptedAgent:
    """Selects a decision's first legal choice after a sliced, reported 'search'."""

    def __init__(self, actor: Actor) -> None:
        self.actor = actor
        self._selection: tuple[tuple[int, str, tuple[str, ...]], str] | None = None

    @staticmethod
    def _decision(episode: Episode) -> tuple[int, str, tuple[str, ...]]:
        return episode.game_ply(), episode.phase(), episode.choices()

    def warm(self, episode: Episode) -> None:
        pass

    def root(self, episode: Episode) -> object:
        return self._decision(episode)

    def timer(self, clock: Clock, now_ms: float) -> _Budget:
        return _Budget()

    def search(self, job: SearchJob, root: object,
               publish: Callable[[dict[str, Any], int], None]) -> dict[str, Any]:
        _, _, keys = root
        slices = 0
        while True:
            time.sleep(_SLICE_SECONDS)
            slices += 1
            report = {
                "selected_key": keys[0],
                "root_value": 0.25,
                "root_evaluator_value": 0.125,
                "model_value": 0.25 if job.model_to_move else -0.25,
                "root_visits": slices,
                "reused_tree": False,
                "candidate_count": len(keys),
                "top_candidates": [
                    {"action_key": key, "visits": slices if index == 0 else 0}
                    for index, key in enumerate(keys[:3])
                ],
            }
            publish(report, 1)
            ended = job.timer is not None and job.timer.end() is not None
            if job.done.is_set() or ended or slices >= _SLICE_LIMIT:
                self._selection = (root, keys[0])
                return report

    def apply(self, episode: Episode) -> None:
        if self._selection is None or self._selection[0] != self._decision(episode):
            raise ValueError("the selection belongs to another decision")
        episode.select(self._selection[1])


ACTOR = Actor("Scripted @1", "scripted-v1")


@pytest.fixture(scope="session")
def board_archive(tmp_path_factory: pytest.TempPathFactory):
    return tmp_path_factory.mktemp("versus-games")


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
    server = make_server(
        Setup((ACTOR,)), ScriptedAgent, static=board_static, catalog_path=STANDARD_CATALOG,
        archive=Archive(
            board_archive, log_contract=VERSUS_LOG_SCHEMA,
            identity=lambda summary: summary["identity"],
        ),
        host="127.0.0.1", port=0,
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{server.server_address[1]}/"
    server.shutdown()
    server.server_close()
