#pragma once

#include "splendor/config.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace splendor {
// Versioned identities of the rules and the staged decision surface. A change to either's
// behavior is a new version.
inline constexpr std::string_view kGameContract = "splendor-game-2p-v1";
inline constexpr std::string_view kStagedActionContract = "splendor-staged-action-v1";
// The standard catalog shipped in `data/` (any catalog of this shape loads; see catalog.hpp).
inline constexpr std::string_view kCatalogContract = "splendor-standard-catalog-v1";
inline constexpr std::string_view kCatalogPayloadSha256 = 
    "b68753ad5f030c65ed697360042c8bd6eadcfa9a3bedb6db6ae42d7259aee7c9";
// `kSeedContract`, the seed law's domain-separation tag, is a build constant (config.hpp).

inline constexpr std::size_t kPlayerCount = 2;
inline constexpr std::size_t kGemColorCount = 5;
inline constexpr std::size_t kTokenColorCount = 6;
inline constexpr std::size_t kTierCount = 3;
inline constexpr std::size_t kMarketSlotsPerTier = 4;
inline constexpr std::size_t kMarketSlotCount = kTierCount * kMarketSlotsPerTier;
inline constexpr std::size_t kReserveSlotsPerPlayer = 3;
inline constexpr std::size_t kVisibleNobleCount = kPlayerCount + 1;
// Tokens of each gem color in play (two seats: four, three: five, four: seven) and of gold.
inline constexpr std::uint8_t kGemSupply = kPlayerCount == 2 ? 4 : kPlayerCount == 3 ? 5 : 7;
inline constexpr std::uint8_t kGoldSupply = 5;
inline constexpr std::size_t kCardCount = 90;
inline constexpr std::size_t kNobleCount = 10;
inline constexpr std::uint16_t kTargetPrestige = 15;

}  // namespace splendor
