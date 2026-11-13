#pragma once

#include "splendor/state.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace splendor::detail {

// Appends every payment of the remaining `need[color..]`, gold covering each shortfall,
// in ascending coloured-token order per colour. `least_gold[c]` is the gold colours
// `c..` need however many coloured tokens they spend; counts that would leave more gold
// owed than the player holds are skipped, which removes only paymentless subtrees.
inline void append_payments(
    const PlayerState& player,
    const std::array<int, kGemColorCount>& need,
    const std::array<int, kGemColorCount + 1>& least_gold,
    const std::size_t color,
    const int gold,
    std::array<std::uint8_t, kTokenColorCount>& payment,
    std::vector<std::array<std::uint8_t, kTokenColorCount>>& out) {
    constexpr auto kGold = static_cast<std::size_t>(Color::Gold);
    const int held_gold = player.tokens[kGold];
    if (color == kGemColorCount) {
        if (gold <= held_gold) {
            payment[kGold] = static_cast<std::uint8_t>(gold);
            out.push_back(payment);
            payment[kGold] = 0;
        }
        return;
    }
    const int maximum = std::min<int>(player.tokens[color], need[color]);
    const int minimum = std::max(0, need[color] + gold + least_gold[color + 1] - held_gold);
    for (int colored = minimum; colored <= maximum; ++colored) {
        payment[color] = static_cast<std::uint8_t>(colored);
        append_payments(
            player, need, least_gold, color + 1, gold + need[color] - colored, payment, out);
    }
    payment[color] = 0;
}

inline void payment_variants(
    const PlayerState& player,
    const Card& card,
    std::vector<std::array<std::uint8_t, kTokenColorCount>>& out) {
    out.clear();
    std::array<int, kGemColorCount> need{};
    for (std::size_t color = 0; color < need.size(); ++color) {
        need[color] = std::max(0, static_cast<int>(card.cost[color]) - player.bonuses[color]);
    }
    std::array<int, kGemColorCount + 1> least_gold{};
    for (std::size_t color = kGemColorCount; color-- > 0;) {
        least_gold[color] = least_gold[color + 1]
            + std::max(0, need[color] - static_cast<int>(player.tokens[color]));
    }
    std::array<std::uint8_t, kTokenColorCount> payment{};
    append_payments(player, need, least_gold, 0, 0, payment, out);
}

}  // namespace splendor::detail
