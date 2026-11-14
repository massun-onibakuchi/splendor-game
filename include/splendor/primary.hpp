#pragma once

#include "splendor/contracts.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace splendor {

// The effect a MAIN choice commits (`splendor-staged-action-v1`,
// docs/specs/staged-actions.md#primary-effects). A take carries its gain/give normal
// form's net delta, a reservation exactly its mandatory gold, a purchase its complete
// payment; any later return and the turn's noble are CLEANUP's.
enum class PrimaryFamily : std::uint8_t {
    Take,
    ReserveVisible,
    ReserveBlind,
    BuyVisible,
    BuyReserved,
    PassForced,
};

// The fields a family carries; every other field stays empty. `source_tier` is the
// card's or deck's tier (1-3), `source_slot` its market slot within the tier, and
// `reserve_owner`/`reserve_slot` name the acting seat's reserve a reserved buy pays
// for. A blind reservation names its tier and no card.
struct Primary {
    PrimaryFamily family{};
    std::array<std::int8_t, kTokenColorCount> token_delta{};
    std::array<std::uint8_t, kTokenColorCount> payment{};
    std::optional<std::uint8_t> source_tier;
    std::optional<std::uint8_t> source_slot;
    std::optional<std::uint8_t> reserve_owner;
    std::optional<std::uint8_t> reserve_slot;
    std::optional<std::uint16_t> target_card_id;
    bool operator==(const Primary&) const = default;
};

// The null move: the take of the empty gain, legal exactly at ten held tokens while a
// take exists. Gain and return lie in different colours, so its net delta is zero.
inline bool is_null_take(const Primary& primary) noexcept {
    if (primary.family != PrimaryFamily::Take) return false;
    for (const std::int8_t delta : primary.token_delta) {
        if (delta != 0) return false;
    }
    return true;
}

// `take`, `reserve_visible`, `reserve_blind`, `buy_visible`, `buy_reserved` or
// `pass_forced`: the family's name in keys and renderings.
std::string_view primary_family_name(PrimaryFamily family);

}  // namespace splendor
