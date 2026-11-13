#pragma once

#include "splendor/action_tree.hpp"
#include "splendor/turn.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace splendor {

// One decision's legal choices as columns aligned with choice order, so a consumer stages
// or groups them at whatever granularity its policy uses (by family, card, gain, payment,
// return or noble) and selects the resulting leaf by its index. Absent fields hold
// `kNoChoiceField`; rows of a vector-valued column are `kTokenColorCount` wide.
inline constexpr std::uint8_t kNoChoiceField = 0xFF;
inline constexpr std::uint16_t kNoChoiceCard = 0xFFFF;

struct ChoiceColumns {
    std::size_t rows{};
    // `DecisionPhase`; the action tree's `ActionFamily`, intent and null-take mark.
    std::vector<std::uint8_t> phase;
    std::vector<std::uint8_t> family;
    std::vector<std::uint16_t> intent;
    std::vector<std::uint8_t> null_take;
    // MAIN: the primary's `CandidateFamily`, net token delta, payment, source tier
    // (1-3), tier-major market slot (0-11), own reserve slot, target card and the
    // bitmask of visible noble slots eligible once it applies.
    std::vector<std::uint8_t> primary;
    std::vector<std::int8_t> token_delta;
    std::vector<std::uint8_t> payment;
    std::vector<std::uint8_t> tier;
    std::vector<std::uint8_t> market_slot;
    std::vector<std::uint8_t> reserve_slot;
    std::vector<std::uint16_t> card;
    std::vector<std::uint8_t> eligible_nobles;
    // CLEANUP: the complete return and the claimed visible noble slot.
    std::vector<std::uint8_t> returned;
    std::vector<std::uint8_t> noble_slot;
};

// Rewrites `out` from `choices`, one decision's legal choices in choice order, reusing its
// storage.
void write_choice_columns(std::span<const TurnChoice> choices, ChoiceColumns& out);

}  // namespace splendor
