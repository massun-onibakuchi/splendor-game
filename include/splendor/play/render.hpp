#pragma once

#include "splendor/match.hpp"
#include "splendor/turn.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// Presentation: JSON renderings of boards, choices and results for hosts and boards. Game
// identity never depends on them; the core game library has no JSON.
namespace splendor::play {

// The board `view`'s observer sees: the state's public fields, per-tier physical deck
// counts, every seat's reserves as that observer knows them, and the terminal facts. Own
// reserves carry their slot; another seat's reserves are listed in canonical order so slot
// identity never distinguishes otherwise indistinguishable hands.
std::string board_json(
    const PlayerView& view,
    const Catalog& catalog,
    const std::optional<TerminalFacts>& terminal);

// One choice with its public referents resolved: market slots are tier-major `0..11` and
// nobles carry the catalog ID `noble_ids`, the visible nobles at the choice's decision,
// name for its slots.
std::string choice_json(
    const TurnChoice& choice,
    std::span<const std::optional<std::uint16_t>> noble_ids);

// A decision's legal choices, in choice order.
std::string choices_json(std::span<const TurnChoice> choices, const State& state);

// A MAIN decision's official gem takes before any return; empty at CLEANUP.
std::string token_takes_json(const Decision& decision);

// `{reason, winner_seat, final_prestige}`.
std::string result_json(const MatchResult& result);

}  // namespace splendor::play
