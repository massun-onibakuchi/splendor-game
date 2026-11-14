#include "splendor/episode.hpp"

#include "splendor/detail/turn.hpp"

#include <atomic>
#include <stdexcept>

namespace splendor {
namespace {
std::atomic<std::uint64_t> next_episode_identity{1};
}  // namespace

TurnEpisode::TurnEpisode(
    const Catalog& catalog,
    const std::uint8_t player_count,
    const std::uint32_t max_game_plies,
    const std::uint64_t setup_seed,
    const std::uint64_t episode_seed)
    : catalog_(catalog),
      max_game_plies_(max_game_plies),
      seeds_(episode_seed),
      decision_{initial_state(catalog_, player_count, max_game_plies, setup_seed), DecisionPhase::Main},
      identity_(next_episode_identity.fetch_add(1, std::memory_order_relaxed)) {
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
    const State& before = decision_.state;
    TurnTraceStep& step = trace_.emplace_back();
    step.game_ply = before.game_ply;
    step.actor = before.current_player;
    step.forced = forced;
    step.choice = choice;
    step.noble_ids = before.noble_ids;
    TurnStep next;
    if (decision_.phase == DecisionPhase::Main) {
        MainOutcome outcome = apply_main(decision_, choice, max_game_plies_, catalog_);
        // The environment draws whatever the turn owes, an ending turn's blind
        // reservation included: the final world names every card.
        std::optional<std::uint16_t> card;
        if (outcome.draw) {
            card = detail::select_episode_chance_card(outcome.outcome_card_ids, seeds_.draw(before));
            if (outcome.draw->kind == DrawKind::Refill) {
                step.refill_slot = static_cast<std::uint8_t>(detail::market_index(outcome.draw->tier, outcome.draw->slot));
                step.refill_card = card;
            } else {
                step.blind_draw_tier = outcome.draw->tier;
            }
        }
        if (outcome.cleanup_count == 1) {
            TurnTraceStep cleanup;
            cleanup.game_ply = step.game_ply;
            cleanup.actor = step.actor;
            cleanup.forced = true;
            cleanup.choice.phase = DecisionPhase::Cleanup;
            cleanup.choice.returned = outcome.forced_return;
            cleanup.choice.noble_slot = outcome.forced_noble;
            cleanup.noble_ids = step.noble_ids;
            next = resolve_main(std::move(outcome), card, max_game_plies_, catalog_);
            trace_.push_back(std::move(cleanup));
        } else {
            next = resolve_main(std::move(outcome), card, max_game_plies_, catalog_);
        }
    } else {
        next = apply_cleanup(decision_, choice, max_game_plies_, catalog_);
    }
    decision_ = std::move(next.decision);
    terminal_ = next.terminal;
    if (terminal_) choices_.clear();
    ++revision_;
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
