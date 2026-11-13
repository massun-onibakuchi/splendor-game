#include "splendor/episode.hpp"
#include "splendor/match.hpp"
#include "splendor/replay.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

TEST(MatchTest, TimeoutUsesSeatPolicyAndPreservesThePendingBoard) {
    using namespace splendor;
    const Catalog catalog = Catalog::load(
        SPLENDOR_GAME_CATALOG);
    State state = initial_state(catalog, 120, 7);
    state.current_player = 1;
    state.game_ply = 1;
    state.players[0].prestige = 3;
    state.players[1].prestige = 8;
    state.players[1].purchased_cards = 4;
    const State before = state;

    GameClock bank({ClockKind::Bank, {100, 100}, {false, false}});
    EXPECT_FALSE(adjudicate_timeout(bank, state, 0));
    bank.run(1, 1, 10);
    EXPECT_FALSE(adjudicate_timeout(bank, state, 109));
    const auto result = adjudicate_timeout(bank, state, 110);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->reason, MatchTermination::Timeout);
    EXPECT_EQ(result->timed_out_seat, 1);
    EXPECT_EQ(result->winner_seat, 0);  // wins on time despite lower prestige
    EXPECT_EQ(result->final_prestige, (std::array<std::uint16_t, 2>{3, 8}));
    EXPECT_EQ(result->purchased_cards, (std::array<std::uint8_t, 2>{0, 4}));
    EXPECT_EQ(state, before);

    GameClock flagged({ClockKind::Turn, {100, 100}, {true, false}});
    flagged.run(1, 1, 0);
    EXPECT_FALSE(adjudicate_timeout(flagged, state, 1000));
    GameClock forfeit({ClockKind::Turn, {100, 100}, {false, true}});
    forfeit.run(1, 1, 0);
    forfeit.stop(40);
    EXPECT_FALSE(adjudicate_timeout(forfeit, state, 1000));
    forfeit.run(1, 1, 1000);
    EXPECT_FALSE(adjudicate_timeout(forfeit, state, 1059));
    EXPECT_EQ(adjudicate_timeout(forfeit, state, 1060), result);

    GameClock restored({ClockKind::Turn, {100, 100}, {false, true}});
    restored.record(1, 1, 100);
    EXPECT_EQ(adjudicate_timeout(restored, state, 5000), result);
}

TEST(MatchTest, FinishedGameCannotBecomeATimeout) {
    using namespace splendor;
    const Catalog catalog = Catalog::load(
        SPLENDOR_GAME_CATALOG);
    TurnEpisode episode(catalog, 1, 7, 8);
    GameClock clock({ClockKind::Bank, {100, 100}, {}});
    clock.run(episode.current_player(), episode.game_ply(), 0);
    while (!episode.is_terminal()) episode.select(std::size_t{0});
    EXPECT_FALSE(episode.adjudicate_timeout(clock, 1000));
    EXPECT_EQ(episode.terminal().reason, "max_game_plies");
}

TEST(MatchTest, ReplayRequiresAllGeneratedFactsButNotRenderedChoices) {
    using namespace splendor;
    const Catalog catalog = Catalog::load(
        SPLENDOR_GAME_CATALOG);
    TurnEpisode source(catalog, 12, 7, 8);
    std::vector<TurnTraceStep> expected = source.drain_trace();
    std::vector<std::string> keys;
    while (!source.is_terminal()) {
        keys.push_back(source.choices().back());
        source.select(keys.back());
        auto steps = source.drain_trace();
        expected.insert(expected.end(), steps.begin(), steps.end());
    }
    for (auto& step : expected) step.choice = "rendering is not a game fact";
    TraceReplay verifier(expected);
    TurnEpisode replay(catalog, 12, 7, 8);
    verifier.verify(replay.drain_trace());
    EXPECT_THROW(verifier.finish(), std::invalid_argument);
    for (const auto& key : keys) {
        replay.select(key);
        verifier.verify(replay.drain_trace());
    }
    verifier.finish();
    EXPECT_EQ(replay.world(), source.world());
    ASSERT_FALSE(expected.empty());
    auto altered = expected;
    altered.front().actor = 1 - altered.front().actor;
    TraceReplay mismatch(std::move(altered));
    EXPECT_THROW(mismatch.verify(expected), std::invalid_argument);
    EXPECT_THROW(verifier.verify(expected), std::invalid_argument);
}

}  // namespace
