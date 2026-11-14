#include "splendor/transition.hpp"

#include "splendor/seed.hpp"
#include "splendor/detail/transition.hpp"
#include "splendor/detail/turn.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace splendor {
namespace {

const CandidateDescriptor& authoritative_candidate(
    const State& state,
    const std::uint32_t max_game_plies,
    const Catalog& catalog,
    const CandidateDescriptor& candidate,
    CandidateBatch& storage) {
    storage = generate_candidates(state, max_game_plies, catalog);
    const auto found = std::lower_bound(
        storage.rows.begin(),
        storage.rows.end(),
        candidate.action_key,
        [](const CandidateDescriptor& row, const std::string& key) {
            return row.action_key < key;
        });
    if (found == storage.rows.end() || found->action_key != candidate.action_key
        || *found != candidate) {
        throw std::invalid_argument("candidate is not authoritative for the supplied state");
    }
    return *found;
}

}  // namespace

void apply_token_delta(
    State& state,
    const std::uint8_t seat,
    const std::array<std::int8_t, kTokenColorCount>& delta) {
    PlayerState& player = state.players[seat];
    for (std::size_t color = 0; color < delta.size(); ++color) {
        const int value = delta[color];
        if (value > 0) {
            if (state.bank_tokens[color] < value) throw std::logic_error("candidate overdrew bank");
            state.bank_tokens[color] -= static_cast<std::uint8_t>(value);
            player.tokens[color] += static_cast<std::uint8_t>(value);
        } else if (value < 0) {
            const auto returned = static_cast<std::uint8_t>(-value);
            if (player.tokens[color] < returned) throw std::logic_error("candidate over-returned tokens");
            player.tokens[color] -= returned;
            state.bank_tokens[color] += returned;
        }
    }
}

void apply_payment(
    State& state,
    const std::uint8_t seat,
    const std::array<std::uint8_t, kTokenColorCount>& payment) {
    PlayerState& player = state.players[seat];
    for (std::size_t color = 0; color < payment.size(); ++color) {
        if (player.tokens[color] < payment[color]) throw std::logic_error("candidate overpaid tokens");
        player.tokens[color] -= payment[color];
        state.bank_tokens[color] += payment[color];
    }
}

std::size_t market_index(const std::uint8_t tier, const std::uint8_t slot) {
    return (static_cast<std::size_t>(tier) - 1) * kMarketSlotsPerTier + slot;
}

void claim_noble(
    State& state,
    const Catalog& catalog,
    const std::uint8_t seat,
    const std::optional<std::uint8_t> chosen_slot) {
    if (!chosen_slot) return;
    const std::size_t slot = *chosen_slot;
    if (!state.noble_ids[slot]) throw std::logic_error("candidate noble is absent");
    const Noble& noble = catalog.noble(*state.noble_ids[slot]);
    const PlayerState& player = state.players[seat];
    for (std::size_t color = 0; color < kGemColorCount; ++color) {
        if (player.bonuses[color] < noble.requirements[color]) {
            throw std::logic_error("candidate noble is not eligible");
        }
    }
    state.players[seat].prestige += noble.prestige;
    ++state.players[seat].noble_count;
    state.noble_ids[slot].reset();
}

std::optional<std::uint8_t> first_empty_reserve(const PlayerState& player) {
    for (std::size_t slot = 0; slot < player.reserves.size(); ++slot) {
        if (!player.reserves[slot]) return static_cast<std::uint8_t>(slot);
    }
    return std::nullopt;
}

std::optional<TerminalFacts> terminal_facts(
    const State& state,
    const std::uint32_t max_game_plies) {
    const bool ordinary = state.endgame_triggered && state.game_ply > 0
        && state.current_player == state.starting_player;
    const bool ply_limit = state.game_ply >= max_game_plies;
    if (!ordinary && !ply_limit) return std::nullopt;

    TerminalFacts facts;
    facts.reason = ordinary
        ? TerminalReason::TargetScoreEqualTurns
        : TerminalReason::MaxGamePlies;
    facts.player_count = state.player_count;
    for (std::size_t seat = 0; seat < state.player_count; ++seat) {
        facts.final_prestige[seat] = state.players[seat].prestige;
        facts.purchased_cards[seat] = state.players[seat].purchased_cards;
    }
    if (facts.reason == TerminalReason::MaxGamePlies) return facts;
    // Greatest prestige, then fewest purchased cards; whoever remains shares the win.
    std::size_t best = 0;
    for (std::size_t seat = 1; seat < state.player_count; ++seat) {
        if (facts.final_prestige[seat] > facts.final_prestige[best]
            || (facts.final_prestige[seat] == facts.final_prestige[best]
                && facts.purchased_cards[seat] < facts.purchased_cards[best])) {
            best = seat;
        }
    }
    for (std::size_t seat = 0; seat < state.player_count; ++seat) {
        if (facts.final_prestige[seat] == facts.final_prestige[best]
            && facts.purchased_cards[seat] == facts.purchased_cards[best]) {
            facts.winners = static_cast<SeatMask>(facts.winners | (1U << seat));
        }
    }
    return facts;
}

std::optional<std::uint8_t> sole_winner(const SeatMask winners) noexcept {
    if (winners == 0 || (winners & (winners - 1)) != 0) return std::nullopt;
    std::uint8_t seat = 0;
    while (!contains(winners, seat)) ++seat;
    return seat;
}

std::array<double, kMaxPlayers> seat_utilities(const std::uint8_t player_count, const SeatMask winners) {
    if (!supported_player_count(player_count)) throw std::invalid_argument("player count is unsupported");
    const double seats = static_cast<double>(player_count);
    // No winner is a tie of every seat.
    const SeatMask sharing = winners == 0 ? static_cast<SeatMask>((1U << player_count) - 1) : winners;
    std::size_t count = 0;
    for (std::size_t seat = 0; seat < player_count; ++seat) count += contains(sharing, seat) ? 1 : 0;
    std::array<double, kMaxPlayers> utility{};
    for (std::size_t seat = 0; seat < player_count; ++seat) {
        const double share = contains(sharing, seat) ? 1.0 / static_cast<double>(count) : 0.0;
        utility[seat] = (seats * share - 1.0) / (seats - 1.0);
    }
    return utility;
}

TransitionResult apply_candidate(
    const State& state,
    const std::uint32_t max_game_plies,
    const Catalog& catalog,
    const CandidateDescriptor& candidate) {
    CandidateBatch batch;
    const CandidateDescriptor& action = authoritative_candidate(
        state, max_game_plies, catalog, candidate, batch);
    return apply_generated_candidate(state, max_game_plies, catalog, action);
}

TransitionResult apply_generated_candidate(
    const State& state,
    const std::uint32_t max_game_plies,
    const Catalog& catalog,
    const CandidateDescriptor& candidate) {
    // A leaf fixes its return before any draw; a blind reservation's return is chosen
    // after its owner sees the card, so no leaf blind reserves.
    if (candidate.family == CandidateFamily::ReserveBlind) {
        throw std::invalid_argument("a blind reservation is not a complete effective leaf");
    }
    // The staged turn's own effects, the leaf's precommitted return and noble included:
    // a leaf and the staged path it realizes are one implementation.
    TransitionResult result;
    result.state = state;
    const std::uint8_t seat = state.current_player;
    const std::optional<OwedDraw> owed = detail::apply_primary_effect(result.state, catalog, candidate);
    claim_noble(result.state, catalog, seat, candidate.chosen_noble_slot);
    result.terminal = detail::finish_turn_state(result.state, max_game_plies);
    if (owed && !result.terminal) {
        result.refill_tier = owed->tier;
        result.refill_market_slot = owed->slot;
    }
    return result;
}

std::vector<State> chance_outcomes(
    const TransitionResult& deferred,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    const std::vector<std::uint16_t> card_ids = chance_outcome_card_ids(
        deferred, catalog);
    std::vector<State> outcomes;
    outcomes.reserve(card_ids.size());
    for (const std::uint16_t card_id : card_ids) {
        outcomes.push_back(resolve_chance_outcome(
            deferred, max_game_plies, catalog, card_id));
    }
    return outcomes;
}

std::vector<std::uint16_t> chance_outcome_card_ids(
    const TransitionResult& deferred,
    const Catalog& catalog) {
    if (!deferred.awaiting_refill() || deferred.terminal || !deferred.refill_market_slot) {
        throw std::invalid_argument("transition is not awaiting a refill");
    }
    const std::size_t market = (static_cast<std::size_t>(*deferred.refill_tier) - 1)
        * kMarketSlotsPerTier + *deferred.refill_market_slot;
    if (deferred.state.market_card_ids[market]) {
        throw std::invalid_argument("deferred market slot is already filled");
    }

    std::vector<std::uint16_t> card_ids;
    for (const std::uint16_t id : deferred.state.unknown_card_ids) {
        if (catalog.card(id).tier != *deferred.refill_tier) continue;
        card_ids.push_back(id);
    }
    if (card_ids.empty()) throw std::logic_error("deferred refill has no outcomes");
    return card_ids;
}

State resolve_chance_outcome(
    const TransitionResult& deferred,
    const std::uint32_t max_game_plies,
    const Catalog& catalog,
    const std::uint16_t card_id) {
    if (!deferred.awaiting_refill() || deferred.terminal || !deferred.refill_market_slot) {
        throw std::invalid_argument("transition is not awaiting a refill");
    }
    const auto found = std::lower_bound(
        deferred.state.unknown_card_ids.begin(), deferred.state.unknown_card_ids.end(), card_id);
    if (found == deferred.state.unknown_card_ids.end()
        || *found != card_id
        || catalog.card(card_id).tier != *deferred.refill_tier) {
        throw std::invalid_argument("card is not a public refill outcome");
    }
    const std::size_t market = (static_cast<std::size_t>(*deferred.refill_tier) - 1)
        * kMarketSlotsPerTier + *deferred.refill_market_slot;
    if (deferred.state.market_card_ids[market]) {
        throw std::invalid_argument("deferred market slot is already filled");
    }
    State outcome = deferred.state;
    outcome.market_card_ids[market] = card_id;
    outcome.unknown_card_ids.erase(outcome.unknown_card_ids.begin()
        + std::distance(deferred.state.unknown_card_ids.begin(), found));
    if (const ValidationResult validation = validate_state(
            outcome, max_game_plies, catalog);
        !validation) {
        throw std::logic_error("chance produced invalid state: " + validation.message);
    }
    return outcome;
}

std::uint16_t select_episode_chance_card(
    const TransitionResult& deferred,
    const Catalog& catalog,
    const std::uint64_t decision_seed) {
    return select_episode_chance_card(chance_outcome_card_ids(deferred, catalog), decision_seed);
}

std::uint16_t select_episode_chance_card(
    const std::vector<std::uint16_t>& card_ids,
    const std::uint64_t decision_seed) {
    if (card_ids.empty()) throw std::invalid_argument("a refill has no outcome to draw");
    const std::uint64_t event_seed = derive64(decision_seed, "episode-chance");
    const std::size_t index = std::min<std::size_t>(
        card_ids.size() - 1,
        static_cast<std::size_t>(std::floor(card_ids.size() * u01(event_seed))));
    return card_ids[index];
}

State select_episode_outcome(
    const TransitionResult& transition,
    const std::uint32_t max_game_plies,
    const Catalog& catalog,
    const std::uint64_t decision_seed) {
    if (!transition.awaiting_refill()) return transition.state;
    return resolve_chance_outcome(
        transition,
        max_game_plies,
        catalog,
        select_episode_chance_card(transition, catalog, decision_seed));
}

}  // namespace splendor
