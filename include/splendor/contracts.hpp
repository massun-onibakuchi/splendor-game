#pragma once

#include "splendor/config.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace splendor {
// Versioned identity of the staged decision surface. A change to its behavior is a new
// version.
inline constexpr std::string_view kStagedActionContract = "splendor-staged-action-v1";
// The standard catalog shipped in `data/` (any catalog of this shape loads; see catalog.hpp).
inline constexpr std::string_view kCatalogContract = "splendor-standard-catalog-v1";
inline constexpr std::string_view kCatalogPayloadSha256 = 
    "b68753ad5f030c65ed697360042c8bd6eadcfa9a3bedb6db6ae42d7259aee7c9";
// `kSeedContract`, the seed law's domain-separation tag, is a build constant (config.hpp).

// The seat counts a game may have. Every state carries its own (`State::player_count`);
// seat-indexed arrays hold `kMaxPlayers` entries, of which the first `player_count` are
// seated.
inline constexpr std::uint8_t kMinPlayers = 2;
inline constexpr std::uint8_t kMaxPlayers = 4;

constexpr bool supported_player_count(const std::uint8_t player_count) noexcept {
    return player_count >= kMinPlayers && player_count <= kMaxPlayers;
}

// Each seat count's rules are a game of their own, with their own versioned identity. A
// change to one's behavior is a new version of that game.
constexpr std::string_view game_contract(const std::uint8_t player_count) {
    switch (player_count) {
        case 2: return "splendor-game-2p-v1";
        case 3: return "splendor-game-3p-v1";
        case 4: return "splendor-game-4p-v1";
        default: throw std::invalid_argument("unsupported player count");
    }
}

inline constexpr std::size_t kGemColorCount = 5;
inline constexpr std::size_t kTokenColorCount = 6;
inline constexpr std::size_t kTierCount = 3;
inline constexpr std::size_t kMarketSlotsPerTier = 4;
inline constexpr std::size_t kMarketSlotCount = kTierCount * kMarketSlotsPerTier;
inline constexpr std::size_t kReserveSlotsPerPlayer = 3;

// Tokens of each gem colour in play: four with two seats, five with three, seven with four.
constexpr std::uint8_t gem_supply(const std::uint8_t player_count) noexcept {
    return player_count == 2 ? 4 : player_count == 3 ? 5 : 7;
}
inline constexpr std::uint8_t kGoldSupply = 5;

// Nobles revealed at setup: one more than the seats.
constexpr std::size_t visible_noble_count(const std::uint8_t player_count) noexcept {
    return static_cast<std::size_t>(player_count) + 1;
}
inline constexpr std::size_t kMaxVisibleNobles = kMaxPlayers + 1;

inline constexpr std::size_t kCardCount = 90;
inline constexpr std::size_t kNobleCount = 10;
inline constexpr std::uint16_t kTargetPrestige = 15;

// A set of seats or of visible noble slots: bit `i` is member `i`.
using SeatMask = std::uint8_t;
using SlotMask = std::uint8_t;
static_assert(kMaxPlayers <= 8 && kMaxVisibleNobles <= 8);

constexpr bool contains(const std::uint8_t mask, const std::size_t member) noexcept {
    return ((mask >> member) & 1U) != 0;
}

}  // namespace splendor
