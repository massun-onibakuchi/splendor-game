#include "splendor/episode.hpp"

#include "splendor/detail/turn.hpp"

#include <stdexcept>

namespace splendor {

TurnEpisode::TurnEpisode(
    const Catalog& catalog,
    const std::uint8_t player_count,
    const std::uint32_t max_game_plies,
    const std::uint64_t setup_seed,
    const std::uint64_t episode_seed)
    : catalog_(catalog),
      max_game_plies_(max_game_plies),
      seeds_(episode_seed),
      decision_{initial_state(catalog_, player_count, max_game_plies, setup_seed), DecisionPhase::Main} {
    settle();
}

// The forced-decision law of `detail::play_forced`, recording each applied choice. Every
// decision here is engine-produced, so its choices are generated without revalidation.
void TurnEpisode::settle() {
    for (;;) {
        detail::legal_choices(decision_, catalog_, choices_);
        if (choices_.size() != 1) return;
        apply(choices_.front(), true);
        if (terminal_) return;
    }
}

// `detail::play_choice`, recording the choice, the draw it owed and, after a MAIN, the
// forced cleanup that completed its turn.
void TurnEpisode::apply(const TurnChoice& choice, const bool forced) {
    TurnTraceStep step;
    step.game_ply = decision_.state.game_ply;
    step.actor = decision_.state.current_player;
    step.forced = forced;
    step.choice = choice;
    step.noble_ids = decision_.state.noble_ids;
    detail::PlayedChoice played;
    terminal_ = detail::play_choice(decision_, choice, seeds_, max_game_plies_, catalog_, &played);
    if (played.draw && played.draw->kind == DrawKind::Refill) {
        step.refill_slot = static_cast<std::uint8_t>(detail::market_index(played.draw->tier, played.draw->slot));
        step.refill_card = played.drawn_card;
    } else if (played.draw) {
        step.blind_draw_tier = played.draw->tier;
    }
    if (played.forced_cleanup) {
        TurnTraceStep cleanup;
        cleanup.game_ply = step.game_ply;
        cleanup.actor = step.actor;
        cleanup.forced = true;
        cleanup.choice = *played.forced_cleanup;
        cleanup.noble_ids = step.noble_ids;
        trace_.push_back(std::move(step));
        trace_.push_back(std::move(cleanup));
    } else {
        trace_.push_back(std::move(step));
    }
    if (terminal_) choices_.clear();
}

std::optional<MatchResult> TurnEpisode::adjudicate_timeout(
    const GameClock& clock, const double now_ms) const {
    if (terminal_) return std::nullopt;
    return splendor::adjudicate_timeout(clock, decision_.state, now_ms);
}

void TurnEpisode::select(const std::size_t choice_index) {
    if (terminal_) throw std::logic_error("episode is already terminal");
    if (choice_index >= choices_.size()) throw std::out_of_range("choice index is out of range");
    apply(choices_[choice_index], false);
    if (!terminal_) settle();
}

void TurnEpisode::select(const std::string_view action_key) {
    if (terminal_) throw std::logic_error("episode is already terminal");
    apply(find_choice(choices_, action_key), false);
    if (!terminal_) settle();
}

std::vector<TurnTraceStep> TurnEpisode::drain_trace() {
    std::vector<TurnTraceStep> steps;
    steps.swap(trace_);
    return steps;
}

std::uint64_t TurnEpisode::decision_seed() const {
    if (terminal_) throw std::logic_error("a finished episode has no decision");
    return seeds_.decision(decision_);
}

}  // namespace splendor
