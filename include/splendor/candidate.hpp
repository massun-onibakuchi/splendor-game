#pragma once

#include "splendor/contracts.hpp"
#include "splendor/catalog.hpp"
#include "splendor/state.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace splendor {

// `ReserveBlind` names a tier whose top card the actor reserves unseen: a MAIN primary
// of the staged turn only, never an effective leaf, since its return is chosen after
// the private draw. A `Take` carries its gain/give normal form's net delta; the null
// move is the take of the empty gain (`is_null_take`).
enum class CandidateFamily : std::uint8_t {
    Take,
    ReserveVisible,
    ReserveBlind,
    BuyVisible,
    BuyReserved,
    PassForced,
};

enum class SourceZone : std::uint8_t { Market, Deck, SelfReserve, OpponentReserve, None };

struct CandidateDescriptor {
    std::string action_key;
    CandidateFamily family{};
    std::array<std::int8_t, kTokenColorCount> token_delta{};
    std::array<std::uint8_t, kTokenColorCount> payment{};
    SourceZone source_zone{SourceZone::None};
    std::optional<std::uint8_t> source_tier;
    std::optional<std::uint8_t> source_slot;
    std::optional<std::uint8_t> reserve_owner;
    std::optional<std::uint8_t> reserve_slot;
    std::optional<std::uint16_t> target_card_id;
    std::optional<std::uint8_t> chosen_noble_slot;
    bool operator==(const CandidateDescriptor&) const = default;
};

// The null move: the take of the empty gain, legal exactly at ten held tokens while a
// take exists. Gain and return lie in different colours, so its net delta is zero.
inline bool is_null_take(const CandidateDescriptor& candidate) noexcept {
    if (candidate.family != CandidateFamily::Take) return false;
    for (const std::int8_t delta : candidate.token_delta) {
        if (delta != 0) return false;
    }
    return true;
}

struct CandidateBatch {
    std::vector<CandidateDescriptor> rows;
};

std::string_view candidate_family_name(CandidateFamily family);
std::string encode_action_key(const CandidateDescriptor& descriptor);

CandidateBatch generate_candidates(
    const State& state,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

}  // namespace splendor
