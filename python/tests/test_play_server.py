"""The match API over seats of any kind: a human and an agent alternate and the log
restarts exactly; agents play each other to the end and a finished game analyzes review;
humans share a board from their own perspectives; a spent bank loses on time; archives
count agents against humans; the board is served only from its own directory, to its own
origin."""

from __future__ import annotations

import copy
import json
import time
import urllib.error
import urllib.request

import pytest

HUMAN = {"kind": "human"}
TURN = {"kind": "turn", "human_seconds": 600, "agent_seconds": 1, "timeout": "forfeit"}


def _get(url: str) -> dict:
    with urllib.request.urlopen(url) as response:
        return json.load(response)


def _post(url: str, body: dict) -> tuple[int, dict]:
    request = urllib.request.Request(
        url, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def _agent(url: str, ponder: bool = False) -> dict:
    return {"kind": "agent", "actor": _get(url + "api/config")["actors"][0]["label"], "ponder": ponder}


def _new_match(url: str, seats: list[dict], time_control: dict, *, start: bool = True) -> int:
    status, body = _post(url + "api/match", {
        "seats": seats, "time_control": time_control, "show_analysis": True, "seed": "20261007",
    })
    assert status == 200, body
    match = _get(url + "api/state")["match_id"]
    if start:
        assert _post(url + "api/start", {"match_id": match})[0] == 200
    return match


def _until(predicate, url: str, query: str = "", timeout: float = 60) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        state = _get(url + "api/state" + query)
        if predicate(state):
            return state
        assert time.monotonic() < deadline, state["status"]
        time.sleep(0.05)


def _paused_game(url: str, time_control: dict) -> dict:
    """An unfinished human-versus-agent game of three human decisions, paused and exported."""
    match = _new_match(url, [HUMAN, _agent(url)], time_control)
    for _ in range(3):
        state = _until(lambda state: state["choices"], url, f"?match={match}")
        body = {"match_id": match, "key": state["choices"][0]["key"], "revision": state["revision"]}
        assert _post(url + "api/play", body)[0] == 200
    _until(lambda state: state["choices"], url, f"?match={match}")
    assert _post(url + "api/pause", {"match_id": match})[0] == 200
    return _get(url + "api/export.json")


@pytest.fixture(scope="module")
def paused_log(board_url: str) -> dict:
    return _paused_game(board_url, TURN)


def test_an_agent_answers_each_human_move_with_its_reported_search(
    board_url, board_archive, paused_log
) -> None:
    archived = json.loads((board_archive / f"{paused_log['game_id']}.json").read_text())
    for log in (paused_log, archived):
        assert [seat["kind"] for seat in log["seats"]] == ["human", "agent"]
        assert log["log_schema"] == "splendor-match-log-v1"
        moves = [step for step in log["steps"] if step["actor"] == 1 and not step["forced"]]
        assert len(moves) == 3
        for step in moves:
            assert step["search"]["kind"] == "move" and step["search"]["seat"] == 1
    # The archive keeps every report whole; the human's export withholds the agent's
    # reports made while it held a card the human could not identify.
    for mine, theirs in zip(paused_log["steps"], archived["steps"], strict=True):
        if theirs["search"] is None:
            continue
        assert theirs["search"]["simulations"] >= 1
        if theirs["search"]["concealed"]:
            assert mine["search"] == {"kind": "move", "seat": 1, "concealed": True}
        else:
            assert mine["search"] == theirs["search"]


def test_an_exact_log_restarts_paused_with_its_reports(board_url, paused_log) -> None:
    assert _post(board_url + "api/restore", {"log": paused_log})[0] == 200
    state = _get(board_url + "api/state")
    assert state["status"] == "paused"
    restored = _get(board_url + "api/export.json")
    assert restored["steps"] == paused_log["steps"]
    assert restored["pending_elapsed_ms"] >= paused_log["pending_elapsed_ms"]


def test_a_replayed_step_cannot_claim_another_seat(board_url, paused_log) -> None:
    forged = copy.deepcopy(paused_log)
    forged["steps"][0]["actor"] = 1
    status, body = _post(board_url + "api/restore", {"log": forged})
    assert status == 409 and "replay" in body["error"]


def test_a_spent_bank_loses_on_time_for_that_seat_alone(board_url) -> None:
    match = _new_match(board_url, [HUMAN, _agent(board_url)], {"kind": "bank", "seconds": 1})
    state = _until(lambda state: state["status"] == "finished", board_url, f"?match={match}")
    assert state["result"]["reason"] == "timeout"
    assert state["result"]["winners"] == [1] and state["result"]["timed_out_seat"] == 0


def test_agents_play_each_other_and_a_finished_game_analyzes_review(short_board_url) -> None:
    url = short_board_url
    match = _new_match(url, [_agent(url), _agent(url)], TURN)
    state = _until(lambda state: state["status"] == "finished", url, f"?match={match}")
    assert state["result"]["reason"] == "max_game_plies"
    assert all(step["search"] for step in state["log"] if not step["forced"])
    assert _post(url + "api/analyze", {"match_id": match, "history_index": 0})[0] == 200
    review = _until(lambda state: state["thinking"]["review"], url, f"?match={match}")["thinking"]["review"]
    assert review["position"] == 0 and review["kind"] == "move"
    status, body = _post(url + "api/analyze", {"match_id": match, "history_index": 10_000})
    assert status == 409 and "no such position" in body["error"]


def test_humans_share_a_board_from_their_own_perspectives(board_url) -> None:
    match = _new_match(board_url, [HUMAN, HUMAN], TURN)
    for revision in range(4):
        actor = _get(board_url + f"api/state?match={match}")["board"]["current_player"]
        other = _get(board_url + f"api/state?match={match}&seat={1 - actor}")
        assert other["perspective"] == 1 - actor and other["choices"] == []
        mine = _get(board_url + f"api/state?match={match}&seat={actor}")
        assert [seat["name"] for seat in mine["seats"]] == ["Human 1", "Human 2"]
        body = {"match_id": match, "key": mine["choices"][0]["key"], "revision": revision}
        assert _post(board_url + "api/play", body)[0] == 200
    assert _get(board_url + f"api/state?match={match}")["thinking"]["kind"] is None


def test_the_archive_counts_agents_against_humans(board_url, paused_log) -> None:
    listing = _get(board_url + "api/archive")
    games = {game["game_id"]: game for game in listing["games"]}
    assert paused_log["game_id"] in games
    records = {record["identity"]: record for record in listing["records"]}
    assert records["scripted-v1"]["unfinished"] >= 1
    assert _post(board_url + "api/restore", {"game_id": paused_log["game_id"]})[0] == 200


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
