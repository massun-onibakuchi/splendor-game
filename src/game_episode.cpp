#include "splendor/episode.hpp"
#include "splendor/match.hpp"

#include "splendor/detail/episode.hpp"
#include "splendor/detail/turn.hpp"
#include "splendor/detail/transition.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <type_traits>

namespace splendor {
namespace {
std::atomic<std::uint64_t> next_episode_identity{1};

template <typename T> std::string json(const T& value);
template <typename T> std::string json(const std::optional<T>& value) {
    return value ? json(*value) : "null";
}
template <typename T> std::string json(const T& value) {
    if constexpr (std::is_same_v<T, bool>) return value ? "true" : "false";
    else if constexpr (std::is_integral_v<T>) return std::to_string(value);
    else {
        std::string text = "[";
        bool first = true;
        for (const auto& item : value) {
            if (!first) text += ',';
            first = false;
            text += json(item);
        }
        return text + ']';
    }
}

std::string phase_name(const DecisionPhase phase) {
    return std::string(decision_phase_name(phase));
}

// The board `view`'s observer sees: the state's public fields, per-tier physical deck
// counts, every seat's reserves as that observer knows them, and the terminal facts.
// Own reserves carry their slot; another seat's reserves are listed in canonical order
// so slot identity never distinguishes otherwise indistinguishable hands.
std::string board_json(
    const PlayerView& view,
    const Catalog& catalog,
    const std::string_view phase,
    const std::optional<TerminalFacts>& terminal) {
    const State& state = view.state();
    std::string out = "{\"observer\":" + json(view.observer())
        + ",\"current_player\":" + json(state.current_player)
        + ",\"starting_player\":" + json(state.starting_player)
        + ",\"game_ply\":" + json(state.game_ply)
        + ",\"phase\":\"" + std::string(phase) + "\""
        + ",\"endgame_triggered\":" + json(state.endgame_triggered)
        + ",\"endgame_trigger_player\":" + json(state.endgame_trigger_player)
        + ",\"bank_tokens\":" + json(state.bank_tokens)
        + ",\"market_card_ids\":" + json(state.market_card_ids)
        + ",\"noble_ids\":" + json(state.noble_ids);
    const TierCounts tiers = tier_counts(state, catalog);
    std::array<std::uint8_t, kTierCount> counts{};
    for (std::size_t tier = 0; tier < kTierCount; ++tier) {
        counts[tier] = static_cast<std::uint8_t>(tiers.deck[tier]);
    }
    out += ",\"deck_counts\":" + json(counts) + ",\"players\":[";
    for (std::size_t seat = 0; seat < kPlayerCount; ++seat) {
        if (seat) out += ',';
        const PlayerState& player = state.players[seat];
        const bool own = seat == view.observer();
        std::vector<std::string> reserves;
        for (std::size_t slot = 0; slot < kReserveSlotsPerPlayer; ++slot) {
            const std::optional<Reserve>& reserve = player.reserves[slot];
            if (!reserve) continue;
            std::string card = "{\"tier\":" + json(reserve->tier)
                + ",\"card_id\":" + json(reserve->card_id)
                + ",\"concealed\":" + json(reserve->concealed);
            if (own) card += ",\"slot\":" + json(slot);
            reserves.push_back(card + '}');
        }
        if (!own) std::sort(reserves.begin(), reserves.end());
        std::string rendered = "[";
        for (std::size_t index = 0; index < reserves.size(); ++index) {
            if (index) rendered += ',';
            rendered += reserves[index];
        }
        out += "{\"tokens\":" + json(player.tokens) + ",\"bonuses\":" + json(player.bonuses)
            + ",\"prestige\":" + json(player.prestige)
            + ",\"purchased_cards\":" + json(player.purchased_cards)
            + ",\"noble_count\":" + json(player.noble_count)
            + ",\"reserves\":" + rendered + "]}";
    }
    out += "],\"terminal\":";
    if (!terminal) out += "null";
    else out += "{\"reason\":\"" + std::string(terminal->reason == TerminalReason::MaxGamePlies
            ? "max_game_plies" : "target_score_equal_turns")
        + "\",\"winner_seat\":" + json(terminal->winner_seat)
        + ",\"final_prestige\":" + json(terminal->final_prestige)
        + ",\"purchased_cards\":" + json(terminal->purchased_cards) + '}';
    return out + '}';
}

std::optional<std::uint16_t> noble_at(
    const State& state, const std::optional<std::uint8_t> slot) {
    return slot ? state.noble_ids[*slot] : std::nullopt;
}

// One legal choice with its public referents resolved at `state`, the decision it
// belongs to: market slots are tier-major `0..11`, nobles carry their catalog ID.
std::string choice_json(const TurnChoice& choice, const State& state) {
    std::string out = "{\"key\":\"" + choice_key(choice) + "\",\"phase\":\""
        + phase_name(choice.phase) + "\"";
    if (choice.phase == DecisionPhase::Cleanup) {
        return out + ",\"returned\":" + json(choice.returned)
            + ",\"noble_slot\":" + json(choice.noble_slot)
            + ",\"noble\":" + json(noble_at(state, choice.noble_slot)) + '}';
    }
    const CandidateDescriptor& primary = choice.primary;
    std::optional<std::size_t> market_slot;
    if (primary.source_slot) market_slot = market_index(*primary.source_tier, *primary.source_slot);
    std::vector<std::uint16_t> nobles;
    for (const std::uint8_t slot : choice.eligible_noble_slots) {
        nobles.push_back(*state.noble_ids[slot]);
    }
    return out + ",\"family\":\"" + std::string(candidate_family_name(primary.family)) + "\""
        + ",\"delta\":" + json(primary.token_delta)
        + ",\"payment\":" + json(primary.payment)
        + ",\"tier\":" + json(primary.source_tier)
        + ",\"market_slot\":" + json(market_slot)
        + ",\"reserve_slot\":" + json(primary.reserve_slot)
        + ",\"card\":" + json(primary.target_card_id)
        + ",\"eligible_nobles\":" + json(nobles) + '}';
}

TurnTerminal terminal_record(const MatchResult& result) {
    TurnTerminal record;
    record.reason = match_termination_name(result.reason);
    record.winner_seat = result.winner_seat ? *result.winner_seat : -1;
    record.final_prestige.assign(result.final_prestige.begin(), result.final_prestige.end());
    return record;
}

}  // namespace
TurnEpisode::TurnEpisode(
    const Catalog& catalog,
    const std::uint32_t max_game_plies,
    const std::uint64_t setup_seed,
    const std::uint64_t episode_seed)
    : catalog_(catalog),
      max_game_plies_(max_game_plies),
      episode_seed_(episode_seed),
      decision_{initial_state(catalog_, max_game_plies, setup_seed), DecisionPhase::Main},
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

void TurnEpisode::apply(const TurnChoice& choice, const bool forced) {
    const State& before = decision_.state;
    TurnTraceStep step{
        before.game_ply, before.current_player, forced, choice_key(choice),
        choice_json(choice, before), std::nullopt, std::nullopt, std::nullopt};
    // `play_choice` consumes a MAIN step's draw and forced cleanup unseen; the primary
    // effect alone names both, and the cleanup never touches the drawn card.
    std::optional<OwedDraw> owed;
    std::optional<TurnTraceStep> cleanup;
    if (decision_.phase == DecisionPhase::Main) {
        State primary = before;
        owed = detail::apply_primary_effect(primary, catalog_, choice.primary);
        const std::vector<TurnChoice> cleanups = detail::cleanup_choices(primary, catalog_);
        if (cleanups.size() == 1) {
            cleanup = TurnTraceStep{
                step.game_ply, step.actor, true, choice_key(cleanups.front()),
                choice_json(cleanups.front(), primary), std::nullopt, std::nullopt,
                std::nullopt};
        }
    }
    terminal_ = detail::play_choice(
        decision_, choice, detail::EpisodeSeeds(episode_seed_), max_game_plies_, catalog_);
    if (owed && owed->kind == DrawKind::Refill) {
        const std::size_t slot = market_index(owed->tier, owed->slot);
        // An ending turn leaves its refill undrawn.
        if (const std::optional<std::uint16_t> card = decision_.state.market_card_ids[slot]) {
            step.refill_slot = static_cast<std::uint8_t>(slot);
            step.refill_card = card;
        }
    } else if (owed) {
        // A blind reservation's draw is always resolved, an ending turn's included.
        step.blind_draw_tier = owed->tier;
    }
    trace_.push_back(std::move(step));
    if (cleanup) trace_.push_back(std::move(*cleanup));
    if (terminal_) choices_.clear();
    ++revision_;
}

std::string TurnEpisode::phase() const {
    if (terminal_) return "terminal";
    return phase_name(decision_.phase);
}

std::vector<std::string> TurnEpisode::choices() const {
    std::vector<std::string> keys;
    keys.reserve(choices_.size());
    for (const TurnChoice& choice : choices_) keys.push_back(choice_key(choice));
    return keys;
}

TurnTerminal TurnEpisode::terminal() const {
    if (!terminal_) throw std::logic_error("episode is not terminal");
    return terminal_record(match_result(*terminal_));
}

TurnTerminal TurnEpisode::timeout_result(const std::uint8_t loser) const {
    return terminal_record(splendor::timeout_result(decision_.state, loser));
}

std::optional<TurnTerminal> TurnEpisode::adjudicate_timeout(
    const GameClock& clock, const double now_ms) const {
    if (terminal_) return std::nullopt;
    const auto result = splendor::adjudicate_timeout(clock, decision_.state, now_ms);
    return result ? std::optional(terminal_record(*result)) : std::nullopt;
}

void TurnEpisode::select(const std::string& action_key) {
    if (terminal_) throw std::logic_error("episode is already terminal");
    apply(find_choice(choices_, action_key), false);
    if (!terminal_) settle();
}

std::string TurnEpisode::observe(const std::uint8_t observer) const {
    return board_json(splendor::observe(decision_, observer), catalog_, phase(), terminal_);
}

std::string TurnEpisode::choice_details() const {
    std::string out = "[";
    for (const TurnChoice& choice : choices_) {
        if (out.size() > 1) out += ',';
        out += choice_json(choice, decision_.state);
    }
    return out + ']';
}

std::string TurnEpisode::token_takes() const {
    if (terminal_ || decision_.phase != DecisionPhase::Main) return "[]";
    std::vector<detail::TokenCounts> takes;
    detail::token_takes(decision_.state, takes);
    return json(takes);
}

std::vector<TurnTraceStep> TurnEpisode::drain_trace() {
    std::vector<TurnTraceStep> steps;
    steps.swap(trace_);
    return steps;
}

std::uint64_t TurnEpisode::decision_seed() const {
    if (terminal_) throw std::logic_error("a finished episode has no decision");
    return detail::EpisodeSeeds(episode_seed_).decision(decision_);
}

void TurnEpisode::select(const std::size_t choice_index) {
    if (terminal_) throw std::logic_error("episode is already terminal");
    if (choice_index >= choices_.size()) throw std::out_of_range("choice index is out of range");
    apply(choices_[choice_index], false);
    if (!terminal_) settle();
}

}  // namespace splendor
