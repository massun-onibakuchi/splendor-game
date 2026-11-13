#pragma once

#include "splendor/turn.hpp"
#include "splendor/clock.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace splendor {

struct TurnTerminal {
    std::string reason;
    int winner_seat{-1};
    std::vector<int> final_prestige;
};

// One choice recorded before it took effect. A forced cleanup is recorded after MAIN.
// A blind draw's identity belongs only to its owner's observation, never to this trace.
struct TurnTraceStep {
    std::uint32_t game_ply{};
    std::uint8_t actor{};
    bool forced{};
    std::string action_key;
    std::string choice;
    std::optional<std::uint8_t> refill_slot;
    std::optional<std::uint16_t> refill_card;
    std::optional<std::uint8_t> blind_draw_tier;
};

// A recorded game, independent of policies and runtimes. Forced choices and chance are
// resolved natively. The catalog is borrowed and must outlive the episode. Agents receive
// view(actor), never world(): the latter is the host's omniscient state, also available to
// a training-time critic. Views and legal-choice references expire at the next selection.
// Batched producers can use the typed turn/episode primitives without recording a trace.
class TurnEpisode {
public:
    TurnEpisode(
        const Catalog& catalog,
        std::uint32_t max_game_plies,
        std::uint64_t setup_seed,
        std::uint64_t episode_seed);
    TurnEpisode(Catalog&&, std::uint32_t, std::uint64_t, std::uint64_t) = delete;
    TurnEpisode(const TurnEpisode&) = delete;
    TurnEpisode& operator=(const TurnEpisode&) = delete;

    const Catalog& catalog() const noexcept { return catalog_; }
    std::uint32_t max_game_plies() const noexcept { return max_game_plies_; }
    const Decision& world() const noexcept { return decision_; }
    PlayerView view(std::uint8_t observer) const { return splendor::observe(decision_, observer); }
    const std::vector<TurnChoice>& legal_choices() const noexcept { return choices_; }
    std::uint64_t decision_seed() const;
    // Process-local identity and mutation generation, not random seeds or replay metadata.
    std::uint64_t identity() const noexcept { return identity_; }
    std::uint64_t revision() const noexcept { return revision_; }

    std::string phase() const;
    std::uint32_t game_ply() const { return decision_.state.game_ply; }
    std::uint8_t current_player() const { return decision_.state.current_player; }
    std::vector<std::string> choices() const;
    TurnTerminal timeout_result(std::uint8_t loser) const;
    std::optional<TurnTerminal> adjudicate_timeout(const GameClock& clock, double now_ms) const;
    bool is_terminal() const { return terminal_.has_value(); }
    TurnTerminal terminal() const;
    void select(const std::string& action_key);
    void select(std::size_t choice_index);

    // Presentation only; native consumers use view() and legal_choices() directly.
    std::string observe(std::uint8_t observer) const;
    std::string choice_details() const;
    std::string token_takes() const;
    std::vector<TurnTraceStep> drain_trace();

private:
    void settle();
    void apply(const TurnChoice& choice, bool forced);

    const Catalog& catalog_;
    std::uint32_t max_game_plies_;
    std::uint64_t episode_seed_;
    Decision decision_;
    std::vector<TurnChoice> choices_;
    std::optional<TerminalFacts> terminal_;
    std::vector<TurnTraceStep> trace_;
    const std::uint64_t identity_;
    std::uint64_t revision_{};
};

}  // namespace splendor
