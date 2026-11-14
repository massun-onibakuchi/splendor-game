#pragma once

#include "splendor/match.hpp"
#include "splendor/turn.hpp"
#include "splendor/detail/episode.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace splendor {

// One choice as it took effect, recorded before it applied. A forced cleanup is recorded
// after its MAIN. `noble_ids` are the visible nobles at the choice's decision, which its
// noble slots name. A blind draw's identity belongs only to its owner's observation,
// never to this trace. Keys and renderings derive from the typed choice on demand.
struct TurnTraceStep {
    std::uint32_t game_ply{};
    std::uint8_t actor{};
    bool forced{};
    TurnChoice choice;
    std::array<std::optional<std::uint16_t>, kMaxVisibleNobles> noble_ids{};
    // A public refill: its tier-major market slot and the card placed there. An ending
    // turn leaves its refill undrawn.
    std::optional<std::uint8_t> refill_slot;
    std::optional<std::uint16_t> refill_card;
    std::optional<std::uint8_t> blind_draw_tier;
    bool operator==(const TurnTraceStep&) const = default;
};

// A recorded game, independent of policies, runtimes and player kinds. Forced choices and
// chance are resolved natively. The catalog is borrowed and must outlive the episode.
// Agents receive view(seat), never world(): the latter is the host's omniscient state,
// also available to a training-time critic. Views and legal-choice references expire at
// the next selection. Batched producers use the typed turn/episode primitives without
// recording a trace.
class TurnEpisode {
public:
    TurnEpisode(
        const Catalog& catalog,
        std::uint8_t player_count,
        std::uint32_t max_game_plies,
        std::uint64_t setup_seed,
        std::uint64_t episode_seed);
    TurnEpisode(Catalog&&, std::uint8_t, std::uint32_t, std::uint64_t, std::uint64_t) = delete;
    TurnEpisode(const TurnEpisode&) = delete;
    TurnEpisode& operator=(const TurnEpisode&) = delete;

    const Catalog& catalog() const noexcept { return catalog_; }
    std::uint32_t max_game_plies() const noexcept { return max_game_plies_; }
    std::uint8_t player_count() const noexcept { return decision_.state.player_count; }
    const Decision& world() const noexcept { return decision_; }
    PlayerView view(std::uint8_t observer) const { return splendor::observe(decision_, observer); }
    // The pending decision's legal choices in choice order; empty once the game is over.
    const std::vector<TurnChoice>& legal_choices() const noexcept { return choices_; }
    std::uint64_t decision_seed() const;

    std::uint32_t game_ply() const noexcept { return decision_.state.game_ply; }
    std::uint8_t current_player() const noexcept { return decision_.state.current_player; }
    bool is_terminal() const noexcept { return terminal_.has_value(); }
    const std::optional<TerminalFacts>& terminal() const noexcept { return terminal_; }
    // A timeout of the pending decision's actor under `clock`; never once the game is over.
    std::optional<MatchResult> adjudicate_timeout(const GameClock& clock, double now_ms) const;

    // Applies a legal choice of the pending decision, then every forced decision after it.
    void select(std::size_t choice_index);
    // The legal choice whose `choice_key` is `action_key`; any other key is rejected
    // before any state change.
    void select(std::string_view action_key);

    // Every choice applied since the previous drain, forced ones included.
    std::vector<TurnTraceStep> drain_trace();

private:
    void settle();
    void apply(const TurnChoice& choice, bool forced);

    const Catalog& catalog_;
    std::uint32_t max_game_plies_;
    detail::EpisodeSeeds seeds_;
    Decision decision_;
    std::vector<TurnChoice> choices_;
    std::optional<TerminalFacts> terminal_;
    std::vector<TurnTraceStep> trace_;
};

}  // namespace splendor
