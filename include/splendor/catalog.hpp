#pragma once

#include "splendor/card_set.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace splendor {

enum class Color : std::uint8_t { White, Blue, Green, Red, Black, Gold };

struct Card {
    std::uint16_t id{};
    std::uint8_t tier{};
    Color bonus{};
    std::uint8_t prestige{};
    std::array<std::uint8_t, kGemColorCount> cost{};
    bool operator==(const Card&) const = default;
};

struct Noble {
    std::uint16_t id{};
    std::uint8_t prestige{};
    std::array<std::uint8_t, kGemColorCount> requirements{};
    bool operator==(const Noble&) const = default;
};

// A card and noble catalog. Its identity is the contract name it declares and the SHA-256
// of its RFC 8785 canonical payload (that name included), so a consumer pins the exact
// catalog it plays by its digest. The engine's own standard catalog is `kCatalogContract`.
class Catalog {
public:
    // Parses and validates a catalog of the standard shape: 90 cards and 10 nobles with
    // dense IDs, tiers 1-3 and the five gem colours.
    static Catalog load(const std::filesystem::path& path);

    // Inline: every rule, feature and search step looks cards up by id.
    const Card& card(const std::uint16_t id) const {
        if (id >= cards_.size()) [[unlikely]] throw_unknown_card();
        return cards_[id];
    }
    const Noble& noble(const std::uint16_t id) const {
        if (id >= nobles_.size()) [[unlikely]] throw_unknown_noble();
        return nobles_[id];
    }
    const std::array<Card, kCardCount>& cards() const noexcept { return cards_; }
    const std::array<Noble, kNobleCount>& nobles() const noexcept { return nobles_; }
    const CardSet& card_ids_for_tier(std::uint8_t tier) const;
    const std::string& contract() const noexcept { return contract_; }
    const std::string& payload_sha256() const noexcept { return payload_sha256_; }

private:
    [[noreturn]] static void throw_unknown_card();
    [[noreturn]] static void throw_unknown_noble();

    std::array<CardSet, kTierCount> tiers_{};
    std::array<Card, kCardCount> cards_{};
    std::array<Noble, kNobleCount> nobles_{};
    std::string contract_;
    std::string payload_sha256_;
};

}  // namespace splendor
