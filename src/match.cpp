#include "splendor/match.hpp"

#include <stdexcept>

namespace splendor {

std::string_view match_termination_name(const MatchTermination reason) {
    switch (reason) {
        case MatchTermination::TargetScoreEqualTurns: return "target_score_equal_turns";
        case MatchTermination::MaxGamePlies: return "max_game_plies";
        case MatchTermination::Timeout: return "timeout";
    }
    throw std::invalid_argument("unknown match termination");
}

MatchResult match_result(const TerminalFacts& terminal) {
    return {
        terminal.reason == TerminalReason::TargetScoreEqualTurns
            ? MatchTermination::TargetScoreEqualTurns : MatchTermination::MaxGamePlies,
        terminal.player_count, terminal.winners, terminal.final_prestige,
        terminal.purchased_cards, std::nullopt};
}

MatchResult timeout_result(const State& state, const std::uint8_t loser) {
    if (loser >= state.player_count) throw std::invalid_argument("timeout seat is out of range");
    const auto seated = static_cast<SeatMask>((1U << state.player_count) - 1);
    MatchResult result{
        MatchTermination::Timeout, state.player_count,
        static_cast<SeatMask>(seated & ~(1U << loser)), {}, {}, loser};
    for (std::size_t seat = 0; seat < state.player_count; ++seat) {
        result.final_prestige[seat] = state.players[seat].prestige;
        result.purchased_cards[seat] = state.players[seat].purchased_cards;
    }
    return result;
}

std::optional<MatchResult> adjudicate_timeout(
    const GameClock& clock, const State& state, const double now_ms) {
    const auto actor = clock.actor();
    if (!actor || !clock.loses_on_time(*actor) || !clock.expired(now_ms)) return std::nullopt;
    if (*actor != state.current_player) {
        throw std::logic_error("match clock does not belong to the pending actor");
    }
    return timeout_result(state, *actor);
}

}  // namespace splendor
