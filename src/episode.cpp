#include "splendor/detail/episode.hpp"
#include "splendor/seed.hpp"
#include "splendor/detail/transition.hpp"
#include "splendor/detail/turn.hpp"
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

std::optional<TerminalFacts> play_decision(
    State& state,
    std::uint32_t& decision_ordinal,
    const CandidateDescriptor& selected,
    const std::uint64_t draw_seed,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    if (decision_ordinal == std::numeric_limits<std::uint32_t>::max()) {
        throw std::logic_error("episode decision ordinal overflows uint32");
    }
    const TransitionResult transition = apply_generated_candidate(
        state, max_game_plies, catalog, selected);
    ++decision_ordinal;
    if (transition.terminal) return transition.terminal;
    state = select_episode_outcome(transition, max_game_plies, catalog, draw_seed);
    return std::nullopt;
}

std::optional<TerminalFacts> play_choice(
    Decision& decision,
    const TurnChoice& selected,
    const EpisodeSeeds& seeds,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
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

std::optional<TerminalFacts> play_leaf(
    Decision& decision,
    const CandidateDescriptor& leaf,
    const EpisodeSeeds& seeds,
    const std::uint32_t max_game_plies,
    const Catalog& catalog,
    std::vector<std::string>& exposed) {
    if (decision.phase != DecisionPhase::Main) {
        throw std::invalid_argument("a complete leaf is played from a turn start");
    }
    const LeafPath path = leaf_path(decision.state, leaf);
    const std::uint8_t actor = decision.state.current_player;
    const std::uint32_t ply = decision.state.game_ply;
    const std::vector<TurnChoice> mains = decision_choices(decision, max_game_plies, catalog);
    if (mains.size() > 1) exposed.push_back(path.main_key);
    if (std::optional<TerminalFacts> terminal = play_choice(
            decision, find_choice(mains, path.main_key), seeds, max_game_plies, catalog)) {
        return terminal;
    }
    // The precommitted cleanup is applied only where the turn still asks for one.
    if (decision.phase == DecisionPhase::Cleanup) {
        const std::vector<TurnChoice> cleanups =
            decision_choices(decision, max_game_plies, catalog);
        if (cleanups.size() > 1) exposed.push_back(path.cleanup_key);
        return play_choice(
            decision, find_choice(cleanups, path.cleanup_key), seeds, max_game_plies, catalog);
    }
    if (decision.state.current_player == actor && decision.state.game_ply == ply) {
        throw std::logic_error("a leaf left its turn incomplete");
    }
    return std::nullopt;
}

}  // namespace splendor::detail
