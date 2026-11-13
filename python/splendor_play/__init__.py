"""Practical play: one human-versus-agent match host, its log, archive and loopback server."""
from splendor_play.actions import TOKEN_COLORS, describe_choice
from splendor_play.agents import Agent, AgentFactory, MatchSetup, PlayerRef, SearchJob
from splendor_play.archive import Archive
from splendor_play.match import (
    VERSUS_LOG_SCHEMA, Match, MatchSettings, load_catalog_view, resolve_match_settings,
)
from splendor_play.server import BoardServer, make_server, serve

__all__ = [
    "TOKEN_COLORS", "VERSUS_LOG_SCHEMA", "Agent", "AgentFactory", "Archive", "BoardServer", "Match",
    "MatchSettings", "MatchSetup", "PlayerRef", "SearchJob", "describe_choice",
    "load_catalog_view", "make_server", "resolve_match_settings", "serve",
]
