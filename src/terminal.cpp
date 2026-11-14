#include "splendor/terminal.hpp"

#include <stdexcept>

namespace splendor {

std::optional<TerminalFacts> terminal_facts(
    const State& state,
    const std::uint32_t max_game_plies) {
    const bool ordinary = state.endgame_triggered && state.game_ply > 0
        && state.current_player == state.starting_player;
    const bool ply_limit = state.game_ply >= max_game_plies;
    if (!ordinary && !ply_limit) return std::nullopt;

    TerminalFacts facts;
    facts.reason = ordinary
        ? TerminalReason::TargetScoreEqualTurns
        : TerminalReason::MaxGamePlies;
    facts.player_count = state.player_count;
    for (std::size_t seat = 0; seat < state.player_count; ++seat) {
        facts.final_prestige[seat] = state.players[seat].prestige;
        facts.purchased_cards[seat] = state.players[seat].purchased_cards;
    }
    if (facts.reason == TerminalReason::MaxGamePlies) return facts;
    // Greatest prestige, then fewest purchased cards; whoever remains shares the win.
    std::size_t best = 0;
    for (std::size_t seat = 1; seat < state.player_count; ++seat) {
        if (facts.final_prestige[seat] > facts.final_prestige[best]
            || (facts.final_prestige[seat] == facts.final_prestige[best]
                && facts.purchased_cards[seat] < facts.purchased_cards[best])) {
            best = seat;
        }
    }
    for (std::size_t seat = 0; seat < state.player_count; ++seat) {
        if (facts.final_prestige[seat] == facts.final_prestige[best]
            && facts.purchased_cards[seat] == facts.purchased_cards[best]) {
            facts.winners = static_cast<SeatMask>(facts.winners | (1U << seat));
        }
    }
    return facts;
}

std::optional<std::uint8_t> sole_winner(const SeatMask winners) noexcept {
    if (winners == 0 || (winners & (winners - 1)) != 0) return std::nullopt;
    std::uint8_t seat = 0;
    while (!contains(winners, seat)) ++seat;
    return seat;
}

std::array<double, kMaxPlayers> seat_utilities(const std::uint8_t player_count, const SeatMask winners) {
    if (!supported_player_count(player_count)) throw std::invalid_argument("player count is unsupported");
    const double seats = static_cast<double>(player_count);
    // No winner is a tie of every seat.
    const SeatMask sharing = winners == 0 ? static_cast<SeatMask>((1U << player_count) - 1) : winners;
    std::size_t count = 0;
    for (std::size_t seat = 0; seat < player_count; ++seat) count += contains(sharing, seat) ? 1 : 0;
    std::array<double, kMaxPlayers> utility{};
    for (std::size_t seat = 0; seat < player_count; ++seat) {
        const double share = contains(sharing, seat) ? 1.0 / static_cast<double>(count) : 0.0;
        utility[seat] = (seats * share - 1.0) / (seats - 1.0);
    }
    return utility;
}

}  // namespace splendor
