"""Practical play: one match between seats of any kind, its log, archive and loopback server."""
from splendor_play.agents import Agent, AgentFactory, MatchSetup, PlayerRef, SearchJob
from splendor_play.archive import Archive
from splendor_play.match import MATCH_LOG_SCHEMA, Match
from splendor_play.server import BoardServer, make_server, serve

__all__ = [
    "MATCH_LOG_SCHEMA", "Agent", "AgentFactory", "Archive", "BoardServer", "Match", "MatchSetup",
    "PlayerRef", "SearchJob", "make_server", "serve",
]
