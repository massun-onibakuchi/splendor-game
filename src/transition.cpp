#include "splendor/transition.hpp"

#include "splendor/seed.hpp"
#include "splendor/detail/transition.hpp"
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
    for (std::size_t seat = 0; seat < kPlayerCount; ++seat) {
        facts.final_prestige[seat] = state.players[seat].prestige;
        facts.purchased_cards[seat] = state.players[seat].purchased_cards;
    }
    if (facts.reason == TerminalReason::MaxGamePlies) return facts;
    if (facts.final_prestige[0] > facts.final_prestige[1]) {
        facts.winner_seat = 0;
    } else if (facts.final_prestige[1] > facts.final_prestige[0]) {
        facts.winner_seat = 1;
    } else if (facts.purchased_cards[0] < facts.purchased_cards[1]) {
        facts.winner_seat = 0;
    } else if (facts.purchased_cards[1] < facts.purchased_cards[0]) {
        facts.winner_seat = 1;
    }
    return facts;
}

std::array<double, kPlayerCount> terminal_utilities(const TerminalFacts& terminal) {
    constexpr double kSeats = static_cast<double>(kPlayerCount);
    const double winners = terminal.winner_seat ? 1.0 : kSeats;
    std::array<double, kPlayerCount> utility{};
    for (std::size_t seat = 0; seat < kPlayerCount; ++seat) {
        const bool wins = !terminal.winner_seat || *terminal.winner_seat == seat;
        const double share = wins ? 1.0 / winners : 0.0;
        utility[seat] = (kSeats * share - 1.0) / (kSeats - 1.0);
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
    State next = state;
    const std::uint8_t acting_seat = state.current_player;
    PlayerState& player = next.players[acting_seat];
    apply_token_delta(next, acting_seat, candidate.token_delta);

    std::optional<std::uint8_t> opened_tier;
    std::optional<std::uint8_t> opened_slot;
    switch (candidate.family) {
        case CandidateFamily::Take:
        case CandidateFamily::PassForced:
            break;
        case CandidateFamily::ReserveBlind:
            // A leaf fixes its return before any draw; a blind reservation's return is
            // chosen after its owner sees the card, so no leaf blind reserves.
            throw std::invalid_argument("a blind reservation is not a complete effective leaf");
        case CandidateFamily::ReserveVisible: {
            const std::size_t index = market_index(*candidate.source_tier, *candidate.source_slot);
            if (next.market_card_ids[index] != candidate.target_card_id) {
                throw std::logic_error("reserve target changed");
            }
            const auto destination = first_empty_reserve(player);
            if (!destination) throw std::logic_error("reserve capacity exhausted");
            player.reserves[*destination] =
                Reserve{*candidate.source_tier, candidate.target_card_id, false};
            next.market_card_ids[index].reset();
            opened_tier = candidate.source_tier;
            opened_slot = candidate.source_slot;
            break;
        }
        case CandidateFamily::BuyVisible:
        case CandidateFamily::BuyReserved: {
            apply_payment(next, acting_seat, candidate.payment);
            if (candidate.family == CandidateFamily::BuyVisible) {
                const std::size_t index = market_index(
                    *candidate.source_tier, *candidate.source_slot);
                if (next.market_card_ids[index] != candidate.target_card_id) {
                    throw std::logic_error("visible buy target changed");
                }
                next.market_card_ids[index].reset();
                opened_tier = candidate.source_tier;
                opened_slot = candidate.source_slot;
            } else {
                if (candidate.reserve_owner != acting_seat
                    || !player.reserves[*candidate.reserve_slot]
                    || player.reserves[*candidate.reserve_slot]->card_id
                        != candidate.target_card_id) {
                    throw std::logic_error("reserved buy target changed");
                }
                player.reserves[*candidate.reserve_slot].reset();
            }
            const Card& card = catalog.card(*candidate.target_card_id);
            ++player.bonuses[static_cast<std::size_t>(card.bonus)];
            player.prestige += card.prestige;
            ++player.purchased_cards;
            break;
        }
    }

    claim_noble(next, catalog, acting_seat, candidate.chosen_noble_slot);
    if (!next.endgame_triggered && player.prestige >= kTargetPrestige) {
        next.endgame_triggered = true;
        next.endgame_trigger_player = acting_seat;
    }
    ++next.game_ply;
    next.current_player = static_cast<std::uint8_t>(
        (next.starting_player + next.game_ply) % kPlayerCount);

    TransitionResult result;
    result.state = std::move(next);
    result.terminal = terminal_facts(result.state, max_game_plies);
    if (opened_tier && !result.terminal
        && tier_counts(result.state, catalog).deck[*opened_tier - 1] != 0) {
        result.refill_tier = opened_tier;
        result.refill_market_slot = opened_slot;
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
