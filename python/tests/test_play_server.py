"""The match API: restart adopts only an exact log; a finished game analyzes review;
a tournament clock's banks survive restart and running one out loses; the board is
served only from its own directory, to its own origin."""

from __future__ import annotations

import copy
import json
import time
import urllib.error
import urllib.request

import pytest

from splendor_game import STANDARD_CATALOG, Catalog, Episode
from splendor_play import describe_choice, load_catalog_view


def _get(url: str) -> dict:
    with urllib.request.urlopen(url) as response:
        return json.load(response)


def _post(url: str, body: dict) -> tuple[int, dict]:
    request = urllib.request.Request(
        url,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def _new_match(board_url: str, time_control: dict) -> int:
    """Start a match in which the human moves first and the model never ponders."""
    status, _ = _post(
        board_url + "api/match",
        {
            "model": _get(board_url + "api/config")["models"][0]["label"],
            "human_first": True,
            "time_control": time_control,
            "ponder": False,
            "show_analysis": True,
            "seed": "20261007",
        },
    )
    assert status == 200
    match = _get(board_url + "api/state")["match_id"]
    assert _post(board_url + "api/start", {"match_id": match})[0] == 200
    return match


def _paused_game(board_url: str, time_control: dict) -> dict:
    """An unfinished game of three human decisions, paused and exported."""
    match = _new_match(board_url, time_control)
    played = 0
    deadline = time.monotonic() + 60
    while played < 3:
        assert time.monotonic() < deadline
        state = _get(board_url + f"api/state?match={match}")
        if state["choices"]:
            body = {
                "match_id": match,
                "key": state["choices"][0]["key"],
                "revision": state["revision"],
            }
            assert _post(board_url + "api/play", body)[0] == 200
            played += 1
        else:
            time.sleep(0.1)
    while not _get(board_url + f"api/state?match={match}")["choices"]:
        assert time.monotonic() < deadline
        time.sleep(0.1)
    assert _post(board_url + "api/pause", {"match_id": match})[0] == 200
    return _get(board_url + "api/export.json")


@pytest.fixture(scope="module")
def paused_log(board_url: str) -> dict:
    return _paused_game(
        board_url,
        {"kind": "turn", "human_seconds": 600, "model_seconds": 1, "timeout": "forfeit"},
    )


def test_a_tournament_clock_restarts_with_the_time_each_player_spent(board_url) -> None:
    log = _paused_game(board_url, {"kind": "bank", "seconds": 30})
    # Each turn's last logged decision holds its turn-cumulative time.
    turns = {
        (step["actor"], step["game_ply"]): step["elapsed_ms"]
        for step in log["steps"]
        if not step["forced"]
    }
    board = _get(board_url + "api/state")["board"]
    turns[(board["current_player"], board["game_ply"])] = log["pending_elapsed_ms"]
    spent = [0, 0]
    for (actor, _), elapsed in turns.items():
        spent[actor] += elapsed
    assert spent[1 - log["human_seat"]] > 0

    assert _restore(board_url, log)[0] == 200
    clock = _get(board_url + "api/state")["clock"]
    assert clock["kind"] == "bank"
    assert clock["running"] is False
    assert clock["remaining_ms"] == [30_000 - used for used in spent]


def test_a_spent_tournament_bank_loses_on_time(board_url) -> None:
    match = _new_match(board_url, {"kind": "bank", "seconds": 1})
    deadline = time.monotonic() + 10
    state = _get(board_url + f"api/state?match={match}")
    while state["status"] == "running":
        assert time.monotonic() < deadline
        time.sleep(0.1)
        state = _get(board_url + f"api/state?match={match}")
    human = state["settings"]["human_seat"]
    assert state["result"] == {
        "reason": "timeout",
        "winners": [1 - human],
        "final_prestige": [0, 0],
        "purchased_cards": [0, 0],
        "timed_out_seat": human,
    }


@pytest.mark.parametrize(
    ("control", "elapsed", "finished"),
    [
        ({"kind": "bank", "seconds": 1}, 500, False),
        ({"kind": "bank", "seconds": 1}, 2_000, True),
        ({"kind": "turn", "human_seconds": 1, "model_seconds": 1, "timeout": "forfeit"}, 2_000, True),
        ({"kind": "turn", "human_seconds": 1, "model_seconds": 1, "timeout": "flag"}, 2_000, False),
    ],
)
def test_replacement_preserves_pending_time_and_adjudicates_expiry(
    board_url, monkeypatch, control, elapsed, finished
) -> None:
    import splendor_play.match

    now = 0.0
    monkeypatch.setattr(splendor_play.match, "_now", lambda: now)
    _new_match(board_url, control)
    original = _get(board_url + "api/export.json")
    # No state poll or match command observes the deadline before replacement.
    now = elapsed
    _new_match(board_url, {"kind": "bank", "seconds": 60})
    assert _post(board_url + "api/restore", {"game_id": original["game_id"]})[0] == 200
    state = _get(board_url + "api/state")
    assert state["status"] == ("finished" if finished else "paused")
    assert state["clock"]["remaining_ms"][original["human_seat"]] == 1_000 - elapsed
    assert state["log"] == original["steps"]
    assert state["result"] == (
        {
            "reason": "timeout",
            "winners": [1 - original["human_seat"]],
            "final_prestige": [0, 0],
            "purchased_cards": [0, 0],
            "timed_out_seat": original["human_seat"],
        }
        if finished else None
    )


def _restore(board_url: str, log: dict) -> tuple[int, dict]:
    return _post(board_url + "api/restore", {"log": log})


def test_an_exact_log_restarts_paused_with_its_reports(board_url, paused_log) -> None:
    status, _ = _restore(board_url, paused_log)
    assert status == 200
    state = _get(board_url + "api/state")
    assert state["status"] == "paused"
    assert state["log"] == paused_log["steps"]


def test_a_replayed_step_cannot_claim_another_actor(board_url, paused_log) -> None:
    log = copy.deepcopy(paused_log)
    chosen = next(step for step in log["steps"] if not step["forced"])
    chosen["actor"] = 1 - chosen["actor"]
    assert _restore(board_url, log)[0] == 409
    assert _get(board_url + "api/state") == {"match_id": None}


def test_replay_regenerates_presentation_from_game_facts(board_url, paused_log) -> None:
    log = copy.deepcopy(paused_log)
    for step in log["steps"]:
        step["text"] = "a different presentation"
        step["choice"] = {"display": "not authoritative"}
    assert _restore(board_url, log)[0] == 200
    assert _get(board_url + "api/state")["log"] == paused_log["steps"]


@pytest.mark.parametrize(
    "result",
    [
        # An unfinished board claimed as an ordinary win.
        lambda log: {
            "reason": "prestige",
            "winners": [log["human_seat"]],
            "final_prestige": [15, 0],
            "purchased_cards": [0, 0],
            "timed_out_seat": None,
        },
        # A forfeit awarded to the wrong side.
        lambda log: {
            "reason": "timeout",
            "winners": [log["human_seat"]],
            "final_prestige": [0, 0],
            "purchased_cards": [0, 0],
            "timed_out_seat": 1 - log["human_seat"],
        },
    ],
)
def test_a_result_the_replayed_game_did_not_reach_is_rejected(
    board_url, paused_log, result
) -> None:
    log = copy.deepcopy(paused_log)
    log["result"] = result(log)
    status, body = _restore(board_url, log)
    assert status == 409
    assert "result" in body["error"]


def test_a_forfeit_at_the_human_decision_restores_finished(board_url, paused_log) -> None:
    assert _restore(board_url, paused_log)[0] == 200
    players = _get(board_url + "api/state")["board"]["players"]
    log = copy.deepcopy(paused_log)
    log["result"] = {
        "reason": "timeout",
        "winners": [1 - log["human_seat"]],
        "final_prestige": [player["prestige"] for player in players],
        "purchased_cards": [player["purchased_cards"] for player in players],
        "timed_out_seat": log["human_seat"],
    }
    status, _ = _restore(board_url, log)
    assert status == 200
    state = _get(board_url + "api/state")
    assert state["status"] == "finished"
    assert state["result"] == log["result"]


def _finished_by_forfeit(board_url: str, paused_log: dict) -> dict:
    """Restore the paused game as forfeited at the human's decision: it is finished."""
    assert _restore(board_url, paused_log)[0] == 200
    players = _get(board_url + "api/state")["board"]["players"]
    log = copy.deepcopy(paused_log)
    log["result"] = {
        "reason": "timeout",
        "winners": [1 - log["human_seat"]],
        "final_prestige": [player["prestige"] for player in players],
        "purchased_cards": [player["purchased_cards"] for player in players],
        "timed_out_seat": log["human_seat"],
    }
    assert _restore(board_url, log)[0] == 200
    return _get(board_url + "api/state")


def _review(board_url: str, match: int, history_index: int, deadline: float) -> dict:
    while True:
        assert time.monotonic() < deadline
        review = _get(board_url + f"api/state?match={match}")["thinking"]["review"]
        if review is not None and review["history_index"] == history_index:
            return review
        time.sleep(0.05)


def test_a_finished_game_analyzes_the_reviewed_position(board_url, paused_log) -> None:
    match = _finished_by_forfeit(board_url, paused_log)["match_id"]
    deadline = time.monotonic() + 30
    analyze = board_url + "api/analyze"
    # The human moved first: position 1 is the model's decision, 0 the human's.
    assert _post(analyze, {"match_id": match, "history_index": 1})[0] == 200
    review = _review(board_url, match, 1, deadline)
    assert review["kind"] == "move"
    assert review["root_visits"] > 0

    # Another position replaces it, valued from the model's side.
    assert _post(analyze, {"match_id": match, "history_index": 0})[0] == 200
    assert _review(board_url, match, 0, deadline)["kind"] == "ponder"

    # Stopping clears it; the analysis never enters the log.
    assert _post(analyze, {"match_id": match, "history_index": None})[0] == 200
    after = _get(board_url + f"api/state?match={match}")
    assert after["thinking"]["review"] is None
    assert not after["thinking"]["reviewing"]
    assert _get(board_url + "api/export.json")["steps"] == after["log"]


def test_analysis_waits_for_the_game_to_end(board_url, paused_log) -> None:
    assert _restore(board_url, paused_log)[0] == 200
    match = _get(board_url + "api/state")["match_id"]
    body = {"match_id": match, "history_index": 0}
    status, response = _post(board_url + "api/analyze", body)
    assert status == 409
    assert "over" in response["error"]


@pytest.mark.parametrize("history_index", [-1, 10_000, True, "0"])
def test_analysis_needs_a_position_of_the_game(
    board_url, paused_log, history_index
) -> None:
    match = _finished_by_forfeit(board_url, paused_log)["match_id"]
    body = {"match_id": match, "history_index": history_index}
    assert _post(board_url + "api/analyze", body)[0] == 409


def _steps_from_keys(log: dict, keys: list[str], model_seat: int) -> list[dict]:
    """The steps a match regenerates for `keys`, as `Match._record` writes them, with a
    full model report on every model decision (the archive form)."""
    catalog = load_catalog_view(STANDARD_CATALOG)
    episode = Episode(
        Catalog.standard(), 2, log["max_game_plies"], int(log["setup_seed"]), int(log["episode_seed"])
    )
    steps: list[dict] = []
    for history_index, key in enumerate(keys, start=1):
        assert key in episode.choices()
        episode.select(key)
        for position, step in enumerate(episode.drain_trace()):
            choice = json.loads(step["choice"])
            text = describe_choice(choice, catalog)
            if step["refill_card"] is not None:
                text += f"; market refilled with card #{step['refill_card']}"
            chosen = position == 0 and not step["forced"]
            model = chosen and step["actor"] == model_seat
            steps.append(
                {
                    "index": len(steps) + 1,
                    "game_ply": step["game_ply"],
                    "actor": step["actor"],
                    "phase": choice["phase"],
                    "action_key": step["action_key"],
                    "forced": step["forced"],
                    "choice": choice,
                    "refill_slot": step["refill_slot"],
                    "refill_card": step["refill_card"],
                    "text": text,
                    "elapsed_ms": 7 if chosen else None,
                    "overtime": False,
                    "search": {
                        "selected_key": key,
                        "root_value": 0.25,
                        "root_evaluator_value": 0.125,
                        "model_value": 0.25,
                        "root_visits": 9,
                        "reused_tree": False,
                        "candidate_count": 3,
                        "top_candidates": [],
                        "concealed": True,
                        "simulations": 9,
                        "pondered_simulations": 0,
                        "think_ms": 7,
                    }
                    if model
                    else None,
                    "ponder": None,
                    "history_index": history_index,
                }
            )
    return steps


def test_model_reports_made_with_a_concealed_card_are_withheld_until_the_end(
    board_url, board_archive, paused_log
) -> None:
    """The model's own search is its knowledge: while it holds a face-down card the
    human cannot identify, the human sees only that a report exists; the archive keeps
    the full report."""
    log = copy.deepcopy(paused_log)
    model_seat = 1 - log["human_seat"]
    human_take = next(
        step["action_key"]
        for step in paused_log["steps"]
        if not step["forced"] and step["actor"] == log["human_seat"]
    )
    blind = "main|reserve_blind|d=0,0,0,0,0,1|p=0,0,0,0,0,0|z=deck|t=2"
    log["steps"] = _steps_from_keys(log, [human_take, blind], model_seat)
    log["result"] = None
    status, body = _restore(board_url, log)
    assert status == 200, body
    state = _get(board_url + "api/state")
    assert state["status"] == "paused"
    model_step = next(step for step in state["log"] if step["search"] is not None)
    assert model_step["action_key"] == blind
    assert model_step["search"] == {"concealed": True}
    assert state["last_search"] == {"concealed": True}
    assert _get(board_url + "api/export.json")["steps"][model_step["index"] - 1]["search"] == {
        "concealed": True
    }
    # The model's reserve is face down on the human's board, and nothing else of it.
    reserves = state["board"]["players"][model_seat]["reserves"]
    assert reserves == [{"tier": 2, "card_id": None, "concealed": True}]

    # Resumed: the model decides again holding that card, so its new report is withheld
    # as well.
    match = state["match_id"]
    assert _post(board_url + "api/resume", {"match_id": match})[0] == 200
    deadline = time.monotonic() + 60
    while True:
        assert time.monotonic() < deadline
        state = _get(board_url + f"api/state?match={match}")
        if state["choices"]:
            break
        time.sleep(0.1)
    body = {"match_id": match, "key": state["choices"][0]["key"], "revision": state["revision"]}
    assert _post(board_url + "api/play", body)[0] == 200
    while True:
        assert time.monotonic() < deadline
        state = _get(board_url + f"api/state?match={match}")
        if state["choices"] and state["revision"] > body["revision"] + 1:
            break
        time.sleep(0.1)
    model_step = [step for step in state["log"] if not step["forced"]][-1]
    assert model_step["actor"] == model_seat
    assert model_step["search"] == {"concealed": True}
    assert state["last_search"] == {"concealed": True}

    archived = json.loads((board_archive / f"{log['game_id']}.json").read_text())
    full = next(step for step in archived["steps"] if step["index"] == model_step["index"])
    assert full["search"]["concealed"] is True
    assert "top_candidates" in full["search"]


def _status(url: str, headers: dict[str, str] | None = None) -> int:
    request = urllib.request.Request(url, headers=headers or {})
    try:
        with urllib.request.urlopen(request) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


def test_the_board_is_served_from_its_own_directory_only(board_url, board_static) -> None:
    with urllib.request.urlopen(board_url) as response:
        assert response.read() == (board_static / "index.html").read_bytes()
        assert response.headers["Content-Type"] == "text/html; charset=utf-8"
    assert _status(board_url + "app.js") == 200
    for path in ("nested/hidden.js", "../index.html", "%2e%2e/pyproject.toml", "missing.js"):
        assert _status(board_url + path) == 404


def test_a_foreign_host_or_origin_cannot_drive_the_match(board_url) -> None:
    port = board_url.rsplit(":", 1)[1].rstrip("/")
    assert _status(board_url + "api/config", {"Host": f"evil.example:{port}"}) == 403
    assert _status(board_url + "api/config", {"Origin": "http://evil.example"}) == 403
    request = urllib.request.Request(
        board_url + "api/match", data=b"{}", headers={"Content-Type": "text/plain"}, method="POST"
    )
    try:
        urllib.request.urlopen(request)
    except urllib.error.HTTPError as error:
        assert error.code == 415
    else:
        raise AssertionError("a non-JSON mutation was accepted")
