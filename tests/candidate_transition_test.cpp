#include "splendor/candidate.hpp"
#include "splendor/catalog.hpp"
#include "splendor/state.hpp"
#include "splendor/transition.hpp"
#include "splendor/detail/transition.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace {

const splendor::CandidateDescriptor& first_family(
    const splendor::CandidateBatch& batch,
    const splendor::CandidateFamily family) {
    const auto found = std::find_if(batch.rows.begin(), batch.rows.end(), [&](const auto& row) {
        return row.family == family;
    });
    if (found == batch.rows.end()) throw std::runtime_error("missing candidate family");
    return *found;
}

struct ScoringFixture {
    splendor::State state;
    splendor::CandidateDescriptor candidate;
};

ScoringFixture scoring_fixture(
    const splendor::Catalog& catalog,
    const std::uint32_t max_game_plies,
    const std::uint32_t game_ply) {
    using namespace splendor;
    for (std::uint64_t seed = 0; seed < 100; ++seed) {
        State state = initial_state(catalog, max_game_plies, seed);
        state.game_ply = game_ply;
        state.current_player = static_cast<std::uint8_t>(
            (state.starting_player + game_ply) % kPlayerCount);
        for (const auto card_id : state.market_card_ids) {
            if (!card_id) continue;
            const Card& card = catalog.card(*card_id);
            std::size_t token_total = 0;
            bool affordable = card.prestige > 0;
            for (std::size_t color = 0; color < kGemColorCount; ++color) {
                token_total += card.cost[color];
                affordable = affordable
                    && card.cost[color] <= state.bank_tokens[color];
            }
            if (!affordable || token_total > 10) continue;

            PlayerState& actor = state.players[state.current_player];
            actor.prestige = static_cast<std::uint16_t>(
                kTargetPrestige - card.prestige);
            for (std::size_t color = 0; color < kGemColorCount; ++color) {
                actor.tokens[color] = card.cost[color];
                state.bank_tokens[color] -= card.cost[color];
            }
            if (!validate_state(state, max_game_plies, catalog)) {
                throw std::logic_error("scoring fixture is not a valid decision state");
            }
            const CandidateBatch candidates = generate_candidates(
                state, max_game_plies, catalog);
            const auto found = std::find_if(
                candidates.rows.begin(), candidates.rows.end(), [&](const auto& candidate) {
                    return candidate.family == CandidateFamily::BuyVisible
                        && candidate.target_card_id == card_id;
                });
            if (found != candidates.rows.end()) return {std::move(state), *found};
            throw std::logic_error("affordable scoring card has no buy candidate");
        }
    }
    throw std::logic_error("failed to construct an affordable scoring fixture");
}

}  // namespace

TEST(CandidateTransitionTest, ValidatesCandidatesTransitionsAndChanceOutcomes) {
    using namespace splendor;
    const Catalog catalog = Catalog::load(
        SPLENDOR_GAME_CATALOG);
    constexpr std::uint32_t max_game_plies = 180;
    const State state = initial_state(catalog, max_game_plies, 4321);
    const CandidateBatch batch = generate_candidates(state, max_game_plies, catalog);
    ASSERT_TRUE(!batch.rows.empty()) << "initial candidates";
    for (std::size_t index = 0; index < batch.rows.size(); ++index) {
        const CandidateDescriptor& row = batch.rows[index];
        EXPECT_EQ(row.action_key, encode_action_key(row)) << "key encodes its descriptor";
        if (index != 0) {
            EXPECT_LT(batch.rows[index - 1].action_key, row.action_key)
                << "unique canonical key order";
        }
    }

    for (const CandidateDescriptor& candidate : batch.rows) {
        const TransitionResult authorized = apply_candidate(
            state, max_game_plies, catalog, candidate);
        const TransitionResult generated = apply_generated_candidate(
            state, max_game_plies, catalog, candidate);
        EXPECT_EQ(generated.state, authorized.state);
        EXPECT_EQ(generated.refill_tier, authorized.refill_tier);
        EXPECT_EQ(generated.refill_market_slot, authorized.refill_market_slot);
        EXPECT_EQ(generated.terminal, authorized.terminal);
        if (!generated.awaiting_refill() && !generated.terminal) {
            EXPECT_TRUE(validate_state(
                generated.state, max_game_plies, catalog).valid)
                << "generated transition preserves public-state invariants";
        }
    }

    CandidateDescriptor golden;
    golden.family = CandidateFamily::Take;
    golden.token_delta = {1, 1, 1, 0, 0, 0};
    golden.action_key = encode_action_key(golden);
    ASSERT_TRUE(golden.action_key == "take|d=1,1,1,0,0,0|p=0,0,0,0,0,0|z=none")
        << "candidate key grammar";

    const CandidateDescriptor& reserve = first_family(batch, CandidateFamily::ReserveVisible);
    CandidateDescriptor altered = reserve;
    altered.token_delta[0] = 127;
    EXPECT_THROW(
        apply_candidate(state, max_game_plies, catalog, altered),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(apply_generated_candidate(state, max_game_plies, catalog, altered)),
        std::logic_error)
        << "generated candidate bypassed transition guards";
    const std::size_t tier_deck_count = static_cast<std::size_t>(std::count_if(
        state.unknown_card_ids.begin(), state.unknown_card_ids.end(), [&](const std::uint16_t id) {
            return catalog.card(id).tier == *reserve.source_tier;
        }));
    const TransitionResult deferred = apply_candidate(
        state, max_game_plies, catalog, reserve);
    const std::vector<State> outcomes = chance_outcomes(
        deferred, max_game_plies, catalog);
    ASSERT_TRUE(outcomes.size() == tier_deck_count) << "chance outcome count";
    const std::vector<std::uint16_t> outcome_card_ids = chance_outcome_card_ids(
        deferred, catalog);
    ASSERT_TRUE(outcome_card_ids.size() == outcomes.size());
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        ASSERT_TRUE(resolve_chance_outcome(
            deferred, max_game_plies, catalog, outcome_card_ids[index]) == outcomes[index])
            << "lazy chance resolution preserves authoritative outcome order";
    }
    ASSERT_TRUE(std::is_sorted(outcomes.begin(), outcomes.end(), [&](const auto& left, const auto& right) {
        const std::size_t market = (*reserve.source_tier - 1) * kMarketSlotsPerTier
            + *reserve.source_slot;
        return *left.market_card_ids[market] < *right.market_card_ids[market];
    })) << "chance card-id order";
    const State selected = select_episode_outcome(
        deferred, max_game_plies, catalog, 99);
    ASSERT_TRUE(validate_state(selected, max_game_plies, catalog).valid)
        << "episode chance selection";

    State ten_tokens = state;
    ten_tokens.players[0].tokens = {2, 2, 2, 2, 2, 0};
    ten_tokens.bank_tokens = {2, 2, 2, 2, 2, 5};
    ASSERT_TRUE(validate_state(ten_tokens, max_game_plies, catalog).valid)
        << "ten-token fixture";
    const CandidateBatch token_batch = generate_candidates(
        ten_tokens, max_game_plies, catalog);
    const auto null_take = std::find_if(
        token_batch.rows.begin(), token_batch.rows.end(),
        [](const CandidateDescriptor& row) { return is_null_take(row); });
    ASSERT_TRUE(null_take != token_batch.rows.end()) << "ten held tokens admit the null move";
    ASSERT_TRUE(null_take->action_key == "take|d=0,0,0,0,0,0|p=0,0,0,0,0,0|z=none")
        << null_take->action_key;

    constexpr std::uint32_t short_game_max_game_plies = 1;
    State short_game = initial_state(catalog, short_game_max_game_plies, 7);
    short_game.players[0].prestige = 1;
    const CandidateBatch short_batch = generate_candidates(
        short_game, short_game_max_game_plies, catalog);
    const TransitionResult terminal = apply_candidate(
        short_game,
        short_game_max_game_plies,
        catalog,
        first_family(short_batch, CandidateFamily::Take));
    ASSERT_TRUE(terminal.terminal.has_value()) << "max-game-plies terminal";
    ASSERT_TRUE(terminal.terminal->reason == TerminalReason::MaxGamePlies)
        << "terminal reason";
    ASSERT_TRUE(!terminal.terminal->winner_seat)
        << "max-game-plies is a neutral truncation";
    ASSERT_TRUE(terminal.terminal->final_prestige[0] == 1)
        << "max-game-plies retains final evidence";

    constexpr std::uint32_t incomplete_round_max_game_plies = 1;
    const ScoringFixture incomplete_round = scoring_fixture(
        catalog, incomplete_round_max_game_plies, 0);
    const TransitionResult incomplete_terminal = apply_candidate(
        incomplete_round.state,
        incomplete_round_max_game_plies,
        catalog,
        incomplete_round.candidate);
    ASSERT_TRUE(incomplete_terminal.terminal.has_value());
    ASSERT_TRUE(incomplete_terminal.terminal->reason == TerminalReason::MaxGamePlies)
        << "reaching target without equal turns remains a max-ply truncation";
    ASSERT_TRUE(!incomplete_terminal.terminal->winner_seat)
        << "incomplete-round max-ply truncation remains neutral";

    constexpr std::uint32_t complete_round_max_game_plies = 2;
    const ScoringFixture complete_round = scoring_fixture(
        catalog, complete_round_max_game_plies, 1);
    const std::uint8_t expected_winner = complete_round.state.current_player;
    const TransitionResult ordinary_terminal = apply_candidate(
        complete_round.state,
        complete_round_max_game_plies,
        catalog,
        complete_round.candidate);
    ASSERT_TRUE(ordinary_terminal.terminal.has_value());
    ASSERT_TRUE(
        ordinary_terminal.terminal->reason == TerminalReason::TargetScoreEqualTurns)
        << "ordinary equal-turn termination takes precedence at the ply limit";
    ASSERT_TRUE(ordinary_terminal.terminal->winner_seat == expected_winner)
        << "ordinary adjudication still selects the winner";
}
