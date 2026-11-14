"""Games on disk: every started match rewrites its full log after each change.

A file is a complete match log, analysis included, so an interrupted game restarts from
it and a finished one is reviewed and counted from it. Listings and records are the native
host's (`splendor::play::log_summary`, `archive_records`).
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from splendor_play._native import archive_records, log_summary

_GAME_ID = re.compile(r"[A-Za-z0-9._-]+")


class Archive:
    """`log_contract` names the logs kept here (`MatchSetup.log_contract`)."""

    def __init__(self, root: str | Path, *, log_contract: str) -> None:
        self.root = Path(root)
        self._log_contract = log_contract

    def save(self, game_id: str, log: str) -> None:
        """Replace the game's file atomically, so a crash never leaves half a log."""
        self.root.mkdir(parents=True, exist_ok=True)
        path = self._path(game_id)
        partial = path.with_name(path.name + ".partial")
        partial.write_text(log, encoding="utf-8")
        partial.replace(path)

    def load(self, game_id: str) -> str:
        path = self._path(game_id)
        if not path.is_file():
            raise ValueError(f"no archived game {game_id}")
        return path.read_text(encoding="utf-8")

    def listing(self) -> str:
        """Every archived game, newest first, and each agent's ordinary record against
        humans, as JSON text."""
        entries = []
        summaries = []
        for path in sorted(self.root.glob("*.json"), reverse=True):
            try:
                summary = log_summary(path.read_text(encoding="utf-8"), self._log_contract)
            except (OSError, ValueError) as error:
                entries.append(json.dumps({"game_id": path.stem, "unreadable": str(error)}))
                continue
            entries.append(summary)
            summaries.append(summary)
        records = archive_records("[" + ",".join(summaries) + "]")
        return '{"games":[' + ",".join(entries) + '],"records":' + records + "}"

    def _path(self, game_id: str) -> Path:
        if not isinstance(game_id, str) or not _GAME_ID.fullmatch(game_id):
            raise ValueError("invalid game id")
        return self.root / f"{game_id}.json"


__all__ = ["Archive"]
