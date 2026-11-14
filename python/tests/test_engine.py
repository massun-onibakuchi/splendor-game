"""The Python engine surface: columnar choices agree with the rendered ones, index
selection plays the same game as key selection, and an episode owns its catalog."""

from __future__ import annotations

import gc
import json

import pytest

from splendor_game import (
    ACTION_FAMILIES, NO_CARD, NO_FIELD, PHASES, PRIMARY_FAMILIES, Catalog, Episode, derive64,
)

MAX_GAME_PLIES = 160


def _episode(seed: int) -> Episode:
    return Episode(
        Catalog.standard(), 2, MAX_GAME_PLIES, derive64(seed, "setup"), derive64(seed, "episode")
    )


def _field(value: int, absent: int = NO_FIELD) -> int | None:
    return None if value == absent else value


def test_the_choice_table_is_the_rendered_choices_column_by_column() -> None:
    episode = _episode(3)
    decisions = 0
    while not episode.is_terminal():
        table = {name: column.tolist() for name, column in episode.choice_table().items()}
        details = json.loads(episode.choice_details())
        assert len(details) == episode.choice_count() == len(table["phase"])
        for row, choice in enumerate(details):
            assert PHASES[table["phase"][row]] == choice["phase"]
            if choice["phase"] == "cleanup":
                assert ACTION_FAMILIES[table["family"][row]] == "cleanup"
                assert table["returned"][row] == choice["returned"]
                assert _field(table["noble_slot"][row]) == choice["noble_slot"]
                assert table["primary"][row] == NO_FIELD
                continue
            assert PRIMARY_FAMILIES[table["primary"][row]] == choice["family"]
            assert table["token_delta"][row] == choice["delta"]
            assert table["payment"][row] == choice["payment"]
            assert _field(table["tier"][row]) == choice["tier"]
            assert _field(table["market_slot"][row]) == choice["market_slot"]
            assert _field(table["reserve_slot"][row]) == choice["reserve_slot"]
            assert _field(table["card"][row], NO_CARD) == choice["card"]
            assert bool(table["null_take"][row]) == (
                choice["family"] == "take" and not any(choice["delta"])
            )
        # One intent never spans two families.
        families = {}
        for family, intent in zip(table["family"], table["intent"], strict=True):
            assert families.setdefault(intent, family) == family
        episode.select_index(episode.decision_seed() % episode.choice_count())
        decisions += 1
    assert decisions > 20


def test_index_selection_plays_the_key_selected_game() -> None:
    by_index, by_key = _episode(11), _episode(11)
    while not by_index.is_terminal():
        index = by_index.decision_seed() % by_index.choice_count()
        key = by_key.choices()[index]
        by_index.select_index(index)
        by_key.select(key)
        assert by_index.observe(0) == by_key.observe(0)
    assert by_key.is_terminal()
    assert by_index.terminal() == by_key.terminal()
    # A finished episode has no decision: no keys, and no table to stage.
    assert by_index.choices() == ()
    with pytest.raises(ValueError, match="finished episode"):
        by_index.choice_table()


def test_an_episode_keeps_its_catalog_alive() -> None:
    episode = _episode(5)
    gc.collect()
    episode.select_index(0)
    assert episode.catalog.payload_sha256 == Catalog.standard().payload_sha256
