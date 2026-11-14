"""Games on disk: every started match rewrites its full log after each change.

A file is a complete match log, analysis included, so an interrupted game restarts from
it and a finished one is reviewed and counted from it.
"""

from __future__ import annotations

import json
import re
from collections.abc import Callable, Mapping
from pathlib import Path
from typing import Any

_GAME_ID = re.compile(r"[A-Za-z0-9._-]+")


class Archive:
    """`log_contract` names the logs kept here (`MatchSetup.log_contract`); `identity` maps a
    log's `model` summary to the actor identity records group by."""

    def __init__(
        self,
        root: str | Path,
        *,
        log_contract: str,
        identity: Callable[[Mapping[str, Any]], str],
    ) -> None:
        self.root = Path(root)
        self._log_contract = log_contract
        self._identity = identity

    def save(self, record: Mapping[str, Any]) -> None:
        """Replace the game's file atomically, so a crash never leaves half a log."""
        self.root.mkdir(parents=True, exist_ok=True)
        path = self._path(record["game_id"])
        partial = path.with_name(path.name + ".partial")
        partial.write_text(json.dumps(record, indent=1), encoding="utf-8")
        partial.replace(path)

    def load(self, game_id: str) -> dict[str, Any]:
        path = self._path(game_id)
        if not path.is_file():
            raise ValueError(f"no archived game {game_id}")
        return json.loads(path.read_text(encoding="utf-8"))

    def listing(self) -> dict[str, Any]:
        """Every archived game, newest first, and each model's ordinary record."""
        games = []
        for path in sorted(self.root.glob("*.json"), reverse=True):
            try:
                games.append(
                    _summary(json.loads(path.read_text(encoding="utf-8")), self._log_contract)
                )
            except (OSError, ValueError, KeyError, TypeError) as error:
                games.append({"game_id": path.stem, "unreadable": str(error)})
        return {"games": games, "records": _records(games, self._identity)}

    def _path(self, game_id: str) -> Path:
        if not isinstance(game_id, str) or not _GAME_ID.fullmatch(game_id):
            raise ValueError("invalid game id")
        return self.root / f"{game_id}.json"


def _summary(log: Mapping[str, Any], log_contract: str) -> dict[str, Any]:
    if log.get("log_contract") != log_contract:
        raise ValueError(f"not a {log_contract} log")
    result = log["result"]
    human = log["human_seat"]
    if result is None:
        outcome = None
    elif len(result["winners"]) != 1:
        outcome = "draw"
    else:
        outcome = "human" if result["winners"] == [human] else "model"
    return {
        "game_id": log["game_id"],
        "started_at": log["started_at"],
        "model": log["model"],
        "human_first": log["human_first"],
        "time_control": log["time_control"],
        "ponder": log["ponder"],
        "show_analysis": log["show_analysis"],
        "decisions": sum(not step["forced"] for step in log["steps"]),
        "result": result,
        "outcome": outcome,
        # Assisted games and timeout forfeits are not ordinary results for strength.
        "ordinary": outcome is not None
        and not log["show_analysis"]
        and result["reason"] != "timeout",
    }


def _records(
    games: list[Mapping[str, Any]], identity: Callable[[Mapping[str, Any]], str]
) -> list[dict[str, Any]]:
    records: dict[str, dict[str, Any]] = {}
    for game in games:
        if "unreadable" in game:
            continue
        digest = identity(game["model"])
        record = records.setdefault(
            digest,
            {
                "model": game["model"],
                "human_wins": 0,
                "model_wins": 0,
                "draws": 0,
                "other_finished": 0,
                "unfinished": 0,
            },
        )
        if game["outcome"] is None:
            record["unfinished"] += 1
        elif not game["ordinary"]:
            record["other_finished"] += 1
        else:
            key = {"human": "human_wins", "model": "model_wins", "draw": "draws"}
            record[key[game["outcome"]]] += 1
    return list(records.values())


__all__ = ["Archive"]
