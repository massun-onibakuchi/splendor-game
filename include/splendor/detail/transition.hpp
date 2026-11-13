#pragma once

#include "splendor/transition.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace splendor {

// The candidate must be a row from
// generate_candidates(state, max_game_plies, catalog) produced for this exact state. This
// skips redundant authorization while retaining transition-specific guards.
TransitionResult apply_generated_candidate(
    const State& state,
    std::uint32_t max_game_plies,
    const Catalog& catalog,
    const CandidateDescriptor& candidate);

// Compact chance production for native consumers that do not need to materialize
// every successor. IDs preserve the public ascending-card order.
std::vector<std::uint16_t> chance_outcome_card_ids(
    const TransitionResult& deferred,
    const Catalog& catalog);

State resolve_chance_outcome(
    const TransitionResult& deferred,
    std::uint32_t max_game_plies,
    const Catalog& catalog,
    std::uint16_t card_id);

// The episode chance law: the card a deferred refill draws under one decision seed.
std::uint16_t select_episode_chance_card(
    const TransitionResult& deferred,
    const Catalog& catalog,
    std::uint64_t decision_seed);

// The same law over a public outcome list in ascending card order.
std::uint16_t select_episode_chance_card(
    const std::vector<std::uint16_t>& card_ids,
    std::uint64_t decision_seed);

// Effect primitives the atomic and staged controllers share. Token movement is
// always paired with the bank so conservation has one authority.
void apply_token_delta(
    State& state,
    std::uint8_t seat,
    const std::array<std::int8_t, kTokenColorCount>& delta);

void apply_payment(
    State& state,
    std::uint8_t seat,
    const std::array<std::uint8_t, kTokenColorCount>& payment);

void claim_noble(
    State& state,
    const Catalog& catalog,
    std::uint8_t seat,
    std::optional<std::uint8_t> chosen_slot);

std::optional<std::uint8_t> first_empty_reserve(const PlayerState& player);

std::size_t market_index(std::uint8_t tier, std::uint8_t slot);

}  // namespace splendor
