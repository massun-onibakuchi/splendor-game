#pragma once

#include "splendor/clock.hpp"
#include "splendor/transition.hpp"

#include <string_view>

namespace splendor {

// Stable result codes used by native games, hosted matches and arena columns.
enum class MatchTermination : std::uint8_t {
    TargetScoreEqualTurns = 0,
    MaxGamePlies = 1,
    Timeout = 2,
};

struct MatchResult {
    MatchTermination reason{};
    std::optional<std::uint8_t> winner_seat;
    std::array<std::uint16_t, kPlayerCount> final_prestige{};
    std::array<std::uint8_t, kPlayerCount> purchased_cards{};
    std::optional<std::uint8_t> timed_out_seat;
    bool operator==(const MatchResult&) const = default;
};

std::string_view match_termination_name(MatchTermination reason);
MatchResult match_result(const TerminalFacts& terminal);
// Records a timeout at the host's pending board, also used to reconstruct archived results.
MatchResult timeout_result(const State& state, std::uint8_t loser);
// The clock must belong to the pending decision. No mutation, scheduling or time sampling:
// the host supplies monotonic time and stops play before accepting an expired selection.
std::optional<MatchResult> adjudicate_timeout(
    const GameClock& clock, const State& state, double now_ms);

}  // namespace splendor
