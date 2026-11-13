"""Loopback HTTP server: a static board client and the JSON match API."""

from __future__ import annotations

import ipaddress
import json
import re
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Mapping
from urllib.parse import parse_qs, urlparse

from splendor_game import Catalog
from splendor_play.agents import AgentFactory, MatchSetup
from splendor_play.archive import Archive
from splendor_play.match import (
    Match,
    load_catalog_view,
    resolve_match_settings,
)

_CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
}
_ASSET_NAME = re.compile(r"[A-Za-z0-9._-]+")
# A restored log carries every search report; other bodies are a few hundred bytes.
_MAX_BODY = 32 * 1024 * 1024


def _assets(static: Path) -> dict[str, tuple[Path, str]]:
    """The board's files by URL path: the directory's own files of a served type, with
    `index.html` at `/`. Nothing below the directory is reachable."""
    assets = {
        f"/{path.name}": (path, _CONTENT_TYPES[path.suffix])
        for path in static.iterdir()
        if path.is_file() and path.suffix in _CONTENT_TYPES and _ASSET_NAME.fullmatch(path.name)
    }
    if "/index.html" not in assets:
        raise ValueError(f"{static} has no index.html")
    assets["/"] = assets["/index.html"]
    return assets


class _Versus:
    """The server's one live match; a new or restored match replaces it."""

    def __init__(
        self, setup: MatchSetup, agents: AgentFactory, catalog_path: Path, archive: Archive
    ) -> None:
        self.setup = setup
        self.agents = agents
        self.catalog = Catalog(catalog_path)
        self.catalog_view = load_catalog_view(catalog_path)
        self.archive = archive
        self.lock = threading.Lock()
        # Every mutation holds this: creations never load two model sessions at once, and
        # no command reaches a match while it is being replaced.
        self.commands = threading.Lock()
        self.match: Match | None = None
        self.match_id = 0

    def config(self) -> dict[str, Any]:
        return {
            "models": [model.summary() for model in self.setup.models],
            "search": self.setup.search,
            "max_game_plies": self.setup.max_game_plies,
            "catalog": self.catalog_view,
        }

    def new_match(self, raw: Any) -> None:
        settings = resolve_match_settings(raw, self.setup)
        self._install(
            lambda: Match(
                self.setup,
                settings,
                catalog=self.catalog,
                catalog_view=self.catalog_view,
                agent=self.agents(self.setup.model(settings.model)),
                archive=self.archive,
            )
        )

    def restore(self, raw: Mapping[str, Any]) -> None:
        """Restart a logged match, from an uploaded log or an archived game."""
        if ("log" in raw) == ("game_id" in raw):
            raise ValueError("restore needs exactly one of log or game_id")

        def create() -> Match:
            # Read an archived log only once the live match is closed: its last
            # decision may still be landing in that same archive.
            log = raw["log"] if "log" in raw else self.archive.load(raw["game_id"])
            if not isinstance(log, dict):
                raise ValueError("the log must be a JSON object")
            return Match.restore(
                self.setup,
                log,
                catalog=self.catalog,
                catalog_view=self.catalog_view,
                agent=self.agents(self.setup.logged_model(log["model"])),
                archive=self.archive,
            )

        self._install(create)

    def _install(self, create: Any) -> None:
        """Replace the match; the caller holds `commands`.

        The current match is closed first: it stops searching, and its last archive write
        lands before a restored copy of the same game starts writing.
        """
        with self.lock:
            previous = self.match
            self.match = None
        if previous is not None:
            previous.close()
        match = create()
        with self.lock:
            self.match = match
            self.match_id += 1

    def current(self) -> tuple[int, Match]:
        with self.lock:
            if self.match is None:
                raise ValueError("no match has been set up")
            return self.match_id, self.match

    def close(self) -> None:
        with self.lock:
            match = self.match
        if match is not None:
            match.close()


def _handler(
    versus: _Versus, assets: Mapping[str, tuple[Path, str]], allowed_hosts: frozenset[str]
) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "splendor-play"

        def log_message(self, format: str, *args: Any) -> None:
            pass

        def _send(
            self, status: int, body: bytes, content_type: str, **headers: str
        ) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for name, value in headers.items():
                self.send_header(name.replace("_", "-"), value)
            self.end_headers()
            self.wfile.write(body)

        def _json(self, value: Any, status: int = HTTPStatus.OK) -> None:
            self._send(status, json.dumps(value).encode(), "application/json")

        def _error(self, status: int, message: str) -> None:
            self._json({"error": message}, status)

        def _trusted(self) -> bool:
            # A page from another origin must not drive the local match (DNS rebinding,
            # cross-site form posts): pin Host, and Origin when a browser sends one.
            host = self.headers.get("Host", "")
            origin = self.headers.get("Origin")
            if host not in allowed_hosts or (
                origin is not None and origin != f"http://{host}"
            ):
                self._error(HTTPStatus.FORBIDDEN, "untrusted host or origin")
                return False
            return True

        def do_GET(self) -> None:
            if not self._trusted():
                return
            url = urlparse(self.path)
            if url.path in assets:
                path, content_type = assets[url.path]
                self._send(HTTPStatus.OK, path.read_bytes(), content_type)
            elif url.path == "/api/config":
                self._json(versus.config())
            elif url.path == "/api/state":
                self._state(parse_qs(url.query))
            elif url.path in {"/api/export.json", "/api/export.txt"}:
                self._export(url.path.endswith(".json"))
            elif url.path == "/api/archive":
                self._json(versus.archive.listing())
            else:
                self._error(HTTPStatus.NOT_FOUND, "not found")

        def _state(self, query: Mapping[str, list[str]]) -> None:
            try:
                match_id, match = versus.current()
            except ValueError:
                self._json({"match_id": None})
                return
            state = match.state()
            known = (query.get("match") or [None])[0], (query.get("since") or [None])[0]
            if known == (str(match_id), str(state["version"])):
                # Unchanged: only the moving parts a client animates between moves.
                self._json(
                    {
                        "match_id": match_id,
                        "version": state["version"],
                        "clock": state["clock"],
                        "thinking": state["thinking"],
                    }
                )
                return
            self._json({"match_id": match_id, **state})

        def _export(self, as_json: bool) -> None:
            try:
                _, match = versus.current()
            except ValueError as error:
                self._error(HTTPStatus.CONFLICT, str(error))
                return
            export = match.export()
            name = f"splendor-versus-{export['seed']}"
            if as_json:
                body = json.dumps(export, indent=2).encode()
                self._send(
                    HTTPStatus.OK,
                    body,
                    "application/json",
                    Content_Disposition=f'attachment; filename="{name}.json"',
                )
            else:
                self._send(
                    HTTPStatus.OK,
                    match.export_text().encode(),
                    "text/plain; charset=utf-8",
                    Content_Disposition=f'attachment; filename="{name}.txt"',
                )

        def do_POST(self) -> None:
            if not self._trusted():
                return
            if self.headers.get("Content-Type", "").split(";")[0] != "application/json":
                self._error(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "JSON body required")
                return
            length_header = self.headers.get("Content-Length") or "0"
            if not length_header.isascii() or not length_header.isdecimal():
                self._error(HTTPStatus.BAD_REQUEST, "invalid Content-Length")
                return
            length = int(length_header)
            if length > _MAX_BODY:
                self._error(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "body too large")
                return
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
                if not isinstance(body, dict):
                    raise ValueError("the body must be a JSON object")
                path = urlparse(self.path).path
                with versus.commands:
                    if path == "/api/match":
                        versus.new_match(body)
                    elif path == "/api/restore":
                        versus.restore(body)
                    else:
                        match_id, match = versus.current()
                        if body.get("match_id") != match_id:
                            raise ValueError("this match has been replaced")
                        if path == "/api/start":
                            match.start()
                        elif path == "/api/pause":
                            match.pause()
                        elif path == "/api/resume":
                            match.resume()
                        elif path == "/api/play":
                            match.play(body["key"], body["revision"])
                        elif path == "/api/analyze":
                            match.analyze(body["history_index"])
                        else:
                            self._error(HTTPStatus.NOT_FOUND, "not found")
                            return
            except (KeyError, TypeError, ValueError) as error:
                self._error(HTTPStatus.CONFLICT, str(error))
                return
            self._json({"ok": True})

    return Handler


class BoardServer(ThreadingHTTPServer):
    """Closing the server also stops the live match's search."""

    versus: _Versus

    def server_close(self) -> None:
        super().server_close()
        self.versus.close()


def make_server(
    setup: MatchSetup,
    agents: AgentFactory,
    *,
    static: str | Path,
    catalog_path: str | Path,
    archive: Archive,
    host: str,
    port: int,
) -> BoardServer:
    """Bind the board in `static` on an IPv4 loopback address; port 0 picks a free one.
    `agents` builds the model side of each new or restored match."""
    address = ipaddress.ip_address(host)
    if address.version != 4 or not address.is_loopback:
        raise ValueError("the versus server binds only to an IPv4 loopback address")
    assets = _assets(Path(static))
    versus = _Versus(setup, agents, Path(catalog_path), archive)
    server = BoardServer((host, port), _handler(versus, assets, frozenset()))
    server.versus = versus
    bound = server.server_address[1]
    names = {host, "localhost"} if host == "127.0.0.1" else {host}
    server.RequestHandlerClass = _handler(
        versus, assets, frozenset(f"{name}:{bound}" for name in names)
    )
    return server


def serve(server: BoardServer) -> None:
    """Serve a `make_server` board until interrupted, then stop its live match."""
    host, port = server.server_address[:2]
    print(
        f"Splendor versus board: http://{host}:{port}/ "
        f"(games archived in {server.versus.archive.root.absolute()})",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


__all__ = ["BoardServer", "make_server", "serve"]
