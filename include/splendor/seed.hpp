#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace splendor {

// The seed law: SHA-256 of the canonical `(kSeedContract, root, domain, coordinates)`
// preimage. Every seed the engine consumes derives from it.
std::uint64_t derive64(
    std::uint64_t root,
    std::string_view domain,
    std::span<const std::uint64_t> coordinates = {});

// A uniform draw in (0, 1) from a derived seed.
double u01(std::uint64_t value) noexcept;

std::uint64_t random_counter(std::uint64_t event_seed, std::uint64_t draw_ordinal);

}  // namespace splendor
