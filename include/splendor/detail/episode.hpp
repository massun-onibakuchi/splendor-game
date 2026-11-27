#pragma once

#include "splendor/turn.hpp"

#include <array>
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
    std::array<std::uint64_t, kMaxPlayers> policy_{};
};

// The environment's draw law: the card an owed draw places, uniform over `card_ids`
// (`MainOutcome::outcome_card_ids`, ascending) under the turn's draw seed.
std::uint16_t select_episode_chance_card(
    const CardSet& card_ids,
    std::uint64_t draw_seed);

// What the environment did after a MAIN choice: the draw it owed, the card drawn and the
// forced cleanup that completed the turn, if any.
struct PlayedChoice {
    std::optional<OwedDraw> draw;
    std::optional<std::uint16_t> drawn_card;
    std::optional<TurnChoice> forced_cleanup;
};

// Plays `selected`, one of `decision`'s legal choices, and advances `decision` to the
// next decision: after a MAIN choice the owed draw -- public refill or private blind
// draw -- is made under the environment's seed for the turn, then a forced cleanup
// completes the turn. Returns the terminal facts when the turn ended the game. `played`,
// when given, receives what the environment did.
std::optional<TerminalFacts> play_choice(
    Decision& decision,
    const TurnChoice& selected,
    const EpisodeSeeds& seeds,
    std::uint32_t max_game_plies,
    const Catalog& catalog,
    PlayedChoice* played = nullptr);

// Plays every decision with exactly one legal choice, which asks no policy, until
// `choices` holds a real choice of `decision` or the game ends.
std::optional<TerminalFacts> play_forced(
    Decision& decision,
    std::vector<TurnChoice>& choices,
    const EpisodeSeeds& seeds,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

}  // namespace splendor::detail
