#include "splendor/detail/episode.hpp"
#include "splendor/seed.hpp"
#include "splendor/detail/turn.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace splendor::detail {

EpisodeSeeds::EpisodeSeeds(const std::uint64_t episode_seed) : episode_(episode_seed) {
    for (std::uint8_t seat = 0; seat < kMaxPlayers; ++seat) {
        const std::array<std::uint64_t, 1> coordinate{seat};
        policy_[seat] = derive64(episode_, "policy", coordinate);
    }
}

std::uint64_t EpisodeSeeds::decision(const State& state) const {
    const std::array<std::uint64_t, 1> coordinate{state.game_ply};
    return derive64(policy_[state.current_player], "decision", coordinate);
}

std::uint64_t EpisodeSeeds::decision(const Decision& decision) const {
    const std::uint64_t turn = this->decision(decision.state);
    return decision.phase == DecisionPhase::Main ? turn : derive64(turn, "cleanup");
}

std::uint64_t EpisodeSeeds::draw(const State& state) const {
    const std::array<std::uint64_t, 1> coordinate{state.game_ply};
    return derive64(episode_, "draw", coordinate);
}

std::uint16_t select_episode_chance_card(
    const std::vector<std::uint16_t>& card_ids,
    const std::uint64_t draw_seed) {
    if (card_ids.empty()) throw std::invalid_argument("an owed draw has no outcome");
    const std::uint64_t event_seed = derive64(draw_seed, "episode-chance");
    const std::size_t index = std::min<std::size_t>(
        card_ids.size() - 1,
        static_cast<std::size_t>(std::floor(card_ids.size() * u01(event_seed))));
    return card_ids[index];
}

std::optional<TerminalFacts> play_choice(
    Decision& decision,
    const TurnChoice& selected,
    const EpisodeSeeds& seeds,
    const std::uint32_t max_game_plies,
    const Catalog& catalog,
    PlayedChoice* const played) {
    if (played) *played = {};
    TurnStep step;
    if (decision.phase == DecisionPhase::Main) {
        MainOutcome outcome = apply_main(decision, selected, max_game_plies, catalog);
        // The environment draws whatever the turn owes, an ending turn's blind
        // reservation included: the final world names every card.
        std::optional<std::uint16_t> card;
        if (outcome.awaiting_draw()) {
            card = select_episode_chance_card(
                outcome.outcome_card_ids, seeds.draw(decision.state));
        }
        if (played) {
            played->draw = outcome.draw;
            played->drawn_card = card;
            if (outcome.cleanup_count == 1) {
                TurnChoice& cleanup = played->forced_cleanup.emplace();
                cleanup.phase = DecisionPhase::Cleanup;
                cleanup.returned = outcome.forced_return;
                cleanup.noble_slot = outcome.forced_noble;
            }
        }
        step = resolve_main(std::move(outcome), card, max_game_plies, catalog);
    } else {
        step = apply_cleanup(decision, selected, max_game_plies, catalog);
    }
    decision = std::move(step.decision);
    return step.terminal;
}

std::optional<TerminalFacts> play_forced(
    Decision& decision,
    std::vector<TurnChoice>& choices,
    const EpisodeSeeds& seeds,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    for (;;) {
        legal_choices(decision, catalog, choices);
        if (choices.size() != 1) return std::nullopt;
        const TurnChoice only = choices.front();
        if (std::optional<TerminalFacts> terminal =
                play_choice(decision, only, seeds, max_game_plies, catalog)) {
            choices.clear();
            return terminal;
        }
    }
}

}  // namespace splendor::detail
