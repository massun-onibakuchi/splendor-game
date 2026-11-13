#pragma once

#include "splendor/candidate.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace splendor {

enum class TerminalReason : std::uint8_t { TargetScoreEqualTurns = 0, MaxGamePlies = 1 };

struct TerminalFacts {
    TerminalReason reason{};
    std::optional<std::uint8_t> winner_seat;
    std::array<std::uint16_t, kPlayerCount> final_prestige{};
    std::array<std::uint8_t, kPlayerCount> purchased_cards{};
    bool operator==(const TerminalFacts&) const = default;
};

struct TransitionResult {
    State state;
    std::optional<std::uint8_t> refill_tier;
    std::optional<std::uint8_t> refill_market_slot;
    std::optional<TerminalFacts> terminal;

    bool awaiting_refill() const noexcept {
        return refill_tier.has_value();
    }
};

TransitionResult apply_candidate(
    const State& state,
    std::uint32_t max_game_plies,
    const Catalog& catalog,
    const CandidateDescriptor& candidate);

std::vector<State> chance_outcomes(
    const TransitionResult& deferred,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

State select_episode_outcome(
    const TransitionResult& transition,
    std::uint32_t max_game_plies,
    const Catalog& catalog,
    std::uint64_t decision_seed);

std::optional<TerminalFacts> terminal_facts(
    const State& state,
    std::uint32_t max_game_plies);

// Each seat's terminal utility, physical-seat indexed, from the win share
// `r_i = 1/m` of the `m` tied winners: `u_i = (N r_i - 1) / (N - 1)`. A seat that
// wins alone scores 1 and every other seat -1; a cap-only termination, which names no
// winner, is an N-way tie worth 0 to every seat.
std::array<double, kPlayerCount> terminal_utilities(const TerminalFacts& terminal);

}  // namespace splendor
