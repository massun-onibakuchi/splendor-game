#pragma once

#include "splendor/state.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace splendor {

enum class TerminalReason : std::uint8_t { TargetScoreEqualTurns = 0, MaxGamePlies = 1 };

// How a game ended. `winners` are the seats of greatest prestige and, among them, fewest
// purchased cards; several share the win, and a cap-only termination names none.
// Seat-indexed arrays hold `player_count` entries.
struct TerminalFacts {
    TerminalReason reason{};
    std::uint8_t player_count{};
    SeatMask winners{};
    std::array<std::uint16_t, kMaxPlayers> final_prestige{};
    std::array<std::uint8_t, kMaxPlayers> purchased_cards{};
    bool operator==(const TerminalFacts&) const = default;
};

// The one seat that won outright; absent for a shared win or a game without a winner.
std::optional<std::uint8_t> sole_winner(SeatMask winners) noexcept;

// The terminal facts of a stable turn start, if the game is over there: equal turns after
// the target was reached, which takes precedence over a simultaneous ply cap.
std::optional<TerminalFacts> terminal_facts(
    const State& state,
    std::uint32_t max_game_plies);

// Each seated seat's utility from its win share `r_i = 1/m` of the `m` winners among
// `player_count` seats: `u_i = (N r_i - 1) / (N - 1)`. A seat that wins alone scores 1
// and every other seat `-1/(N-1)`; no winner is an N-way tie worth 0 to every seat. The
// utilities of the seated seats sum to zero; unseated entries are zero.
std::array<double, kMaxPlayers> seat_utilities(std::uint8_t player_count, SeatMask winners);

inline std::array<double, kMaxPlayers> terminal_utilities(const TerminalFacts& terminal) {
    return seat_utilities(terminal.player_count, terminal.winners);
}

}  // namespace splendor
