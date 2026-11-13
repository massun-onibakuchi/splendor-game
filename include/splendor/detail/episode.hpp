#pragma once

#include "splendor/catalog.hpp"
#include "splendor/state.hpp"
#include "splendor/transition.hpp"
#include "splendor/turn.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace splendor::detail {

// The native episode seed law every episode driver shares: one episode seed, one policy
// stream per seat, and one decision seed per turn-start ply of the acting seat. Search,
// policy sampling, and episode chance all consume the decision seed.
class EpisodeSeeds {
public:
    EpisodeSeeds() = default;
    explicit EpisodeSeeds(std::uint64_t episode_seed);

    std::uint64_t episode() const noexcept { return episode_; }
    // One seed per turn of the acting seat: its MAIN and CLEANUP policy and search draw
    // from it. It is handed to the acting agent.
    std::uint64_t decision(const State& state) const;
    // A decision's own policy and search seed: a CLEANUP decision addresses its
    // randomness apart from its turn's MAIN.
    std::uint64_t decision(const Decision& decision) const;
    // The environment's draw seed for the turn at `state`'s ply. It belongs to the
    // episode, never to a seat: no agent receives it, so no seat can anticipate a
    // card the rules keep from it.
    std::uint64_t draw(const State& state) const;

private:
    std::uint64_t episode_{};
    std::array<std::uint64_t, kPlayerCount> policy_{};
};

// Atomic adapter: plays the acting seat's `selected` complete effective leaf, generated
// for exactly `state`, as decision `decision_ordinal` and counts it. Returns the terminal
// facts, or else advances `state` through the turn's chance outcome drawn from
// `draw_seed`, the environment's seed for the turn.
std::optional<TerminalFacts> play_decision(
    State& state,
    std::uint32_t& decision_ordinal,
    const CandidateDescriptor& selected,
    std::uint64_t draw_seed,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

// Plays `selected`, one of `decision`'s legal choices, and advances `decision` to the
// next decision: after a MAIN choice the owed draw -- public refill or private blind
// draw -- is made under the environment's seed for the turn, then a forced cleanup
// completes the turn. Returns the terminal facts when the turn ended the game.
std::optional<TerminalFacts> play_choice(
    Decision& decision,
    const TurnChoice& selected,
    const EpisodeSeeds& seeds,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

// Plays every decision with exactly one legal choice, which asks no policy, until
// `choices` holds a real choice of `decision` or the game ends.
std::optional<TerminalFacts> play_forced(
    Decision& decision,
    std::vector<TurnChoice>& choices,
    const EpisodeSeeds& seeds,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

// Atomic adapter: plays a complete effective leaf of the turn start `decision` as its
// precommitted MAIN and CLEANUP keys. The cleanup is fixed before the refill. Appends to
// `exposed` the key of each of those decisions that had more than one legal choice: the
// keys that replay the turn in a `TurnEpisode`, which plays forced choices itself.
std::optional<TerminalFacts> play_leaf(
    Decision& decision,
    const CandidateDescriptor& leaf,
    const EpisodeSeeds& seeds,
    std::uint32_t max_game_plies,
    const Catalog& catalog,
    std::vector<std::string>& exposed);

}  // namespace splendor::detail
