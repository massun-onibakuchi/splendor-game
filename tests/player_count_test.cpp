#include "splendor/episode.hpp"
#include "splendor/seed.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace {

using namespace splendor;

Catalog load_catalog() { return Catalog::load(SPLENDOR_GAME_CATALOG); }

TEST(PlayerCount, SetupFollowsEachSeatCountsSupplyAndNobles) {
    const Catalog catalog = load_catalog();
    const State two = initial_state(catalog, 2, 100, 42);
    for (const std::uint8_t players : {2, 3, 4}) {
        const State state = initial_state(catalog, players, 100, 42);
        ASSERT_TRUE(validate_state(state, 100, catalog));
        EXPECT_EQ(state.player_count, players);
        for (std::size_t color = 0; color < kGemColorCount; ++color) {
            EXPECT_EQ(state.bank_tokens[color], gem_supply(players));
        }
        EXPECT_EQ(state.bank_tokens[static_cast<std::size_t>(Color::Gold)], kGoldSupply);
        const auto nobles = static_cast<std::size_t>(std::count_if(
            state.noble_ids.begin(), state.noble_ids.end(), [](const auto& id) { return id.has_value(); }));
        EXPECT_EQ(nobles, visible_noble_count(players));
        // One seed deals one layout; more seats reveal more of the same noble order.
        EXPECT_EQ(state.market_card_ids, two.market_card_ids);
        EXPECT_EQ(state.unknown_card_ids, two.unknown_card_ids);
        EXPECT_TRUE(std::equal(two.noble_ids.begin(), two.noble_ids.begin() + 3, state.noble_ids.begin()));
    }
    EXPECT_EQ(gem_supply(2), 4);
    EXPECT_EQ(gem_supply(3), 5);
    EXPECT_EQ(gem_supply(4), 7);
    EXPECT_THROW(initial_state(catalog, 1, 100, 42), std::invalid_argument);
    EXPECT_THROW(initial_state(catalog, 5, 100, 42), std::invalid_argument);

    State seated_material = two;
    seated_material.players[2].prestige = 1;
    EXPECT_FALSE(validate_state(seated_material, 100, catalog));
}

// Uniform playouts at three and four seats: every exposed decision and every seat's view
// of it is valid, every seat decides, and the result follows the ranking law.
TEST(PlayerCount, MultiplayerGamesPlayToTheRankingLaw) {
    const Catalog catalog = load_catalog();
    for (const std::uint8_t players : {3, 4}) {
        for (std::uint64_t game = 0; game < 20; ++game) {
            TurnEpisode episode(catalog, players, 400, derive64(game, "setup"), derive64(game, "episode"));
            SeatMask actors = 0;
            while (!episode.is_terminal()) {
                const Decision& world = episode.world();
                ASSERT_TRUE(validate_decision(world, 400, catalog)) << int(players);
                actors = static_cast<SeatMask>(actors | (1U << world.state.current_player));
                for (std::uint8_t seat = 0; seat < players; ++seat) {
                    const PlayerView view = episode.view(seat);
                    ASSERT_TRUE(observed_by(view.state(), seat));
                    ASSERT_TRUE(validate_state(
                        view.state(), 400, catalog, true,
                        world.phase == DecisionPhase::Main ? StateBoundary::Stable
                                                          : StateBoundary::Intermediate));
                }
                episode.select(derive64(episode.decision_seed(), "pick") % episode.legal_choices().size());
            }
            EXPECT_EQ(actors, (1U << players) - 1);
            const TerminalFacts& terminal = *episode.terminal();
            ASSERT_EQ(terminal.player_count, players);
            if (terminal.reason == TerminalReason::MaxGamePlies) {
                EXPECT_EQ(terminal.winners, 0);
                continue;
            }
            const auto key = [&](const std::size_t seat) {
                return std::pair<int, int>{terminal.final_prestige[seat], -terminal.purchased_cards[seat]};
            };
            std::pair<int, int> best{-1, 0};
            for (std::size_t seat = 0; seat < players; ++seat) best = std::max(best, key(seat));
            for (std::size_t seat = 0; seat < players; ++seat) {
                EXPECT_EQ(contains(terminal.winners, seat), key(seat) == best);
            }
        }
    }
}

TEST(PlayerCount, SharedWinsSplitTheUtility) {
    const Catalog catalog = load_catalog();
    State state = initial_state(catalog, 3, 100, 9);
    state.endgame_triggered = true;
    state.endgame_trigger_player = 1;
    state.game_ply = 3;
    state.players[0].prestige = 15;
    state.players[1].prestige = 15;
    state.players[2].prestige = 15;
    state.players[2].purchased_cards = 1;
    const std::optional<TerminalFacts> terminal = terminal_facts(state, 100);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->winners, 0b011);  // fewer purchased cards break the prestige tie
    EXPECT_FALSE(sole_winner(terminal->winners));
    EXPECT_EQ(terminal_utilities(*terminal), (std::array<double, kMaxPlayers>{0.25, 0.25, -0.5, 0.0}));
    EXPECT_EQ(seat_utilities(3, 0b100), (std::array<double, kMaxPlayers>{-0.5, -0.5, 1.0, 0.0}));
    EXPECT_EQ(seat_utilities(4, 0), (std::array<double, kMaxPlayers>{}));
    EXPECT_EQ(sole_winner(0b100), 2);
}

}  // namespace
