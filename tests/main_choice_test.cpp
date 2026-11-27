#include "splendor/turn.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using splendor::Catalog;
using splendor::Decision;
using splendor::DecisionPhase;
using splendor::MainOutcome;
using splendor::PrimaryFamily;
using splendor::State;
using splendor::TurnChoice;

Catalog load_catalog() { return Catalog::load(SPLENDOR_GAME_CATALOG); }

std::vector<std::string> main_keys(
    const State& state, const std::uint32_t max_game_plies, const Catalog& catalog) {
    std::vector<std::string> keys;
    for (const TurnChoice& choice :
         splendor::decision_choices(Decision{state, DecisionPhase::Main}, max_game_plies, catalog)) {
        keys.push_back(splendor::choice_key(choice));
    }
    return keys;
}

bool has_key(const std::vector<std::string>& keys, const std::string& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

// A turn start whose actor holds exactly the cost of a scoring market card, `prestige`
// short of nothing: its purchase reaches the target. The tokens come from the bank.
struct ScoringFixture {
    State state;
    TurnChoice buy;
};

ScoringFixture scoring_fixture(
    const Catalog& catalog, const std::uint32_t max_game_plies, const std::uint32_t game_ply) {
    using namespace splendor;
    for (std::uint64_t seed = 0; seed < 100; ++seed) {
        State state = initial_state(catalog, 2, max_game_plies, seed);
        state.game_ply = game_ply;
        state.current_player = static_cast<std::uint8_t>(
            (state.starting_player + game_ply) % state.player_count);
        for (const auto card_id : state.market_card_ids) {
            const Card& card = catalog.card(*card_id);
            std::size_t token_total = 0;
            bool affordable = card.prestige > 0;
            for (std::size_t color = 0; color < kGemColorCount; ++color) {
                token_total += card.cost[color];
                affordable = affordable && card.cost[color] <= state.bank_tokens[color];
            }
            if (!affordable || token_total > 10) continue;
            PlayerState& actor = state.players[state.current_player];
            actor.prestige = static_cast<std::uint16_t>(kTargetPrestige - card.prestige);
            for (std::size_t color = 0; color < kGemColorCount; ++color) {
                actor.tokens[color] = card.cost[color];
                state.bank_tokens[color] -= card.cost[color];
            }
            for (const TurnChoice& choice :
                 decision_choices(Decision{state, DecisionPhase::Main}, max_game_plies, catalog)) {
                if (choice.primary.family == PrimaryFamily::BuyVisible
                    && choice.primary.target_card_id == card_id) {
                    return {state, choice};
                }
            }
            throw std::logic_error("an affordable scoring card has no purchase");
        }
    }
    throw std::logic_error("no scoring fixture");
}

}  // namespace

// MAIN keys spell their primary in the staged grammar and ascend in byte order; the
// grammar is persisted alignment metadata, so these bytes are contract.
TEST(MainChoice, KeysSpellThePrimaryAndAscend) {
    using namespace splendor;
    const Catalog catalog = load_catalog();
    constexpr std::uint32_t max_game_plies = 180;
    State state = initial_state(catalog, 2, max_game_plies, 4321);

    const std::vector<std::string> keys = main_keys(state, max_game_plies, catalog);
    ASSERT_FALSE(keys.empty());
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
    EXPECT_EQ(std::adjacent_find(keys.begin(), keys.end()), keys.end());
    EXPECT_TRUE(has_key(keys, "main|take|d=1,1,1,0,0,0|p=0,0,0,0,0,0|z=none"));
    EXPECT_TRUE(has_key(keys, "main|reserve_blind|d=0,0,0,0,0,1|p=0,0,0,0,0,0|z=deck|t=3"));
    const std::uint16_t first = *state.market_card_ids[0];
    EXPECT_TRUE(has_key(
        keys, "main|reserve_visible|d=0,0,0,0,0,1|p=0,0,0,0,0,0|z=market|t=1|s=0|c="
            + std::to_string(first)));

    // A reserved card the actor can pay exactly, drawn from the deck.
    const auto reserved = std::find_if(
        state.unknown_card_ids.begin(), state.unknown_card_ids.end(), [&](const std::uint16_t id) {
            const Card& card = catalog.card(id);
            if (card.tier != 1) return false;
            for (std::size_t color = 0; color < kGemColorCount; ++color) {
                if (card.cost[color] > state.bank_tokens[color]) return false;
            }
            return true;
        });
    ASSERT_NE(reserved, state.unknown_card_ids.end());
    const std::uint16_t card_id = *reserved;
    state.unknown_card_ids.erase(*reserved);
    PlayerState& actor = state.players[state.current_player];
    actor.reserves[0] = Reserve{1, card_id, false};
    std::string paid;
    for (std::size_t color = 0; color < kTokenColorCount; ++color) {
        const std::uint8_t cost = color < kGemColorCount ? catalog.card(card_id).cost[color] : 0;
        actor.tokens[color] = cost;
        state.bank_tokens[color] -= cost;
        paid += (color == 0 ? "" : ",") + std::to_string(cost);
    }
    ASSERT_TRUE(validate_state(state, max_game_plies, catalog));
    EXPECT_TRUE(has_key(
        main_keys(state, max_game_plies, catalog),
        "main|buy_reserved|d=0,0,0,0,0,0|p=" + paid + "|z=self_reserve|t=1|o="
            + std::to_string(state.current_player) + "|r=0|c=" + std::to_string(card_id)));

    State ten_tokens = initial_state(catalog, 2, max_game_plies, 4321);
    ten_tokens.players[ten_tokens.current_player].tokens = {2, 2, 2, 2, 2, 0};
    ten_tokens.bank_tokens = {2, 2, 2, 2, 2, 5};
    ASSERT_TRUE(validate_state(ten_tokens, max_game_plies, catalog));
    EXPECT_TRUE(has_key(
        main_keys(ten_tokens, max_game_plies, catalog),
        "main|take|d=0,0,0,0,0,0|p=0,0,0,0,0,0|z=none"))
        << "ten held tokens admit the null move";
}

// The ply cap is a neutral truncation that keeps the final evidence; reaching the target
// without equal turns stays a truncation, and equal-turn termination takes precedence over
// a simultaneous cap.
TEST(MainChoice, CapTerminationIsNeutralAndEqualTurnsTakePrecedence) {
    using namespace splendor;
    const Catalog catalog = load_catalog();

    State short_game = initial_state(catalog, 2, 1, 7);
    short_game.players[short_game.current_player].prestige = 1;
    const std::vector<TurnChoice> mains =
        decision_choices(Decision{short_game, DecisionPhase::Main}, 1, catalog);
    const auto take = std::find_if(mains.begin(), mains.end(), [](const TurnChoice& choice) {
        return choice.primary.family == PrimaryFamily::Take;
    });
    ASSERT_NE(take, mains.end());
    const MainOutcome capped = apply_main(Decision{short_game, DecisionPhase::Main}, *take, 1, catalog);
    ASSERT_TRUE(capped.terminal.has_value());
    EXPECT_EQ(capped.terminal->reason, TerminalReason::MaxGamePlies);
    EXPECT_EQ(capped.terminal->winners, 0);
    EXPECT_EQ(capped.terminal->final_prestige[short_game.current_player], 1);

    const ScoringFixture incomplete = scoring_fixture(catalog, 1, 0);
    const MainOutcome truncated =
        apply_main(Decision{incomplete.state, DecisionPhase::Main}, incomplete.buy, 1, catalog);
    ASSERT_TRUE(truncated.terminal.has_value());
    EXPECT_EQ(truncated.terminal->reason, TerminalReason::MaxGamePlies);
    EXPECT_EQ(truncated.terminal->winners, 0);

    const ScoringFixture complete = scoring_fixture(catalog, 2, 1);
    const MainOutcome ended =
        apply_main(Decision{complete.state, DecisionPhase::Main}, complete.buy, 2, catalog);
    ASSERT_TRUE(ended.terminal.has_value());
    EXPECT_EQ(ended.terminal->reason, TerminalReason::TargetScoreEqualTurns);
    EXPECT_EQ(sole_winner(ended.terminal->winners), complete.state.current_player);
}
