"""A versus log must replay: its seeds and discretionary keys rebuild the logged game."""

import json
import random

import pytest

from splendor_game import Catalog, Episode

CATALOG = Catalog.standard()
MAX_GAME_PLIES = 160


def _play(setup_seed: int, episode_seed: int, policy: random.Random):
    episode = Episode(CATALOG, 2, MAX_GAME_PLIES, setup_seed, episode_seed)
    trace = episode.drain_trace()
    while not episode.is_terminal():
        details = json.loads(episode.choice_details())
        assert [choice["key"] for choice in details] == list(episode.choices())
        before = json.loads(episode.observe(0))
        episode.select(policy.choice(episode.choices()))
        steps = episode.drain_trace()
        assert steps[0]["forced"] is False
        trace += steps
        after = json.loads(episode.observe(0))
        for step in steps:
            if step["refill_slot"] is not None:
                assert (
                    before["market_card_ids"][step["refill_slot"]]
                    != step["refill_card"]
                )
        # Within a select, the market only changes through the trace's refills and the
        # visible cards its steps reserved or bought.
        refilled = {step["refill_slot"]: step["refill_card"] for step in steps}
        for slot, card in enumerate(after["market_card_ids"]):
            if card is not None and card != before["market_card_ids"][slot]:
                assert refilled.get(slot) == card
    return episode, trace


@pytest.mark.parametrize("seed", range(6))
def test_discretionary_keys_replay_the_trace_and_its_forced_steps(seed: int) -> None:
    episode, trace = _play(seed, seed + 101, random.Random(seed))
    replay = Episode(CATALOG, 2, MAX_GAME_PLIES, seed, seed + 101)
    replayed = replay.drain_trace()
    for step in trace:
        if not step["forced"]:
            replay.select(step["action_key"])
            replayed += replay.drain_trace()
    assert replayed == trace
    assert replay.observe(0) == episode.observe(0)
    assert replay.observe(1) == episode.observe(1)

    # Every noble and purchase is a traced choice, including forced cleanups that
    # `select` applies inside a MAIN step.
    board = json.loads(episode.observe(0))
    choices = [(step["actor"], json.loads(step["choice"])) for step in trace]
    for seat, player in enumerate(board["players"]):
        nobles = [
            c["noble"]
            for actor, c in choices
            if actor == seat and c["phase"] == "cleanup" and c["noble"] is not None
        ]
        bought = [
            c["card"]
            for actor, c in choices
            if actor == seat and c.get("family", "").startswith("buy")
        ]
        assert len(nobles) == player["noble_count"]
        assert len(bought) == player["purchased_cards"]
    assert board["terminal"] is not None
