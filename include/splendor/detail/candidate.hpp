#pragma once

#include "splendor/candidate.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace splendor::detail {

using TokenCounts = std::array<int, kTokenColorCount>;

// Appends `encode_action_key(descriptor)` to `output`.
void append_action_key(std::string& output, const CandidateDescriptor& descriptor);

// The visible noble slots the supplied bonuses qualify for.
inline SlotMask eligible_noble_mask(
    const State& state,
    const Catalog& catalog,
    const std::array<std::uint8_t, kGemColorCount>& bonuses) {
    SlotMask mask = 0;
    for (std::size_t slot = 0; slot < state.noble_ids.size(); ++slot) {
        if (!state.noble_ids[slot]) continue;
        const Noble& noble = catalog.noble(*state.noble_ids[slot]);
        bool eligible = true;
        for (std::size_t color = 0; color < kGemColorCount; ++color) {
            eligible = eligible && bonuses[color] >= noble.requirements[color];
        }
        if (eligible) mask = static_cast<SlotMask>(mask | (1U << slot));
    }
    return mask;
}

// Appends the visible noble slots the supplied bonuses qualify for, in ascending order.
template <typename Slot>
void append_eligible_nobles(
    const State& state,
    const Catalog& catalog,
    const std::array<std::uint8_t, kGemColorCount>& bonuses,
    std::vector<Slot>& out) {
    const SlotMask mask = eligible_noble_mask(state, catalog, bonuses);
    for (std::size_t slot = 0; slot < kMaxVisibleNobles; ++slot) {
        if (contains(mask, slot)) out.push_back(static_cast<std::uint8_t>(slot));
    }
}

// Eligible noble slots for the supplied bonuses, or one absent choice when no
// noble is eligible.
inline void noble_choices(
    const State& state,
    const Catalog& catalog,
    const std::array<std::uint8_t, kGemColorCount>& bonuses,
    std::vector<std::optional<std::uint8_t>>& out) {
    out.clear();
    append_eligible_nobles(state, catalog, bonuses, out);
    if (out.empty()) out.push_back(std::nullopt);
}

// Appends every return of `left` tokens drawn from `after[color..]` to `out`, in
// ascending count order per colour. `room[color]` is the sum of `after[color..]`; a
// branch that cannot return `left` from what remains is skipped, and a branch with
// nothing left to return is complete, so only emitted returns are visited.
inline void append_discards(
    const TokenCounts& after,
    const std::array<int, kTokenColorCount + 1>& room,
    const std::size_t color,
    const int left,
    TokenCounts& current,
    std::vector<TokenCounts>& out) {
    if (left == 0) {
        out.push_back(current);
        return;
    }
    if (room[color] < left) return;
    for (int count = 0; count <= std::min(after[color], left); ++count) {
        current[color] = count;
        append_discards(after, room, color + 1, left - count, current, out);
    }
    current[color] = 0;
}

// Complete return vectors that bring the holding back to ten tokens, or one
// empty vector when the take leaves no excess.
inline void discard_variants(
    const PlayerState& player,
    const TokenCounts& take,
    std::vector<TokenCounts>& out) {
    out.clear();
    TokenCounts after{};
    for (std::size_t color = 0; color < after.size(); ++color) {
        after[color] = player.tokens[color] + take[color];
    }
    std::array<int, kTokenColorCount + 1> room{};
    for (std::size_t color = after.size(); color-- > 0;) room[color] = room[color + 1] + after[color];
    TokenCounts current{};
    append_discards(after, room, 0, std::max(0, room[0] - 10), current, out);
}

}  // namespace splendor::detail
