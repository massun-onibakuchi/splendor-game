#pragma once

#include "splendor/candidate.hpp"
#include "splendor/transition.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace splendor {

// Canonical staged-action turn (`splendor-staged-action-v1`, docs/specs/staged-actions.md):
//
//   MAIN -> primary effect -> draw, when owed -> joint CLEANUP -> turn end
//
// MAIN commits one family-consistent primary effect; the draw is the public refill of
// a vacated market slot or the private draw of a blind reservation, revealed to its
// owner alone; CLEANUP is the complete return vector together with the turn's noble,
// chosen after the actual draw. A decision with exactly one legal choice is applied
// without a policy call. `action_tree` (splendor/action_tree.hpp) groups a decision's
// choices into families, intents and parameters.
enum class DecisionPhase : std::uint8_t { Main, Cleanup };

// `main` or `cleanup`.
std::string_view decision_phase_name(DecisionPhase phase);

// The draw a MAIN primary owes before its cleanup: the public refill of market slot
// `slot` of `tier`, or the blind draw of `tier`'s top card into the actor's reserve
// slot `slot`.
enum class DrawKind : std::uint8_t { Refill, BlindReserve };

struct OwedDraw {
    DrawKind kind{};
    std::uint8_t tier{};
    // A refill's market slot within the tier; a blind reservation's reserve slot.
    std::uint8_t slot{};
    // The seat whose reservation the draw fills; unused by a refill. Named rather than
    // read from the acting seat because the draw may resolve after the turn completed.
    std::uint8_t owner{};
    bool operator==(const OwedDraw&) const = default;
};

// A state at which the acting player chooses. A MAIN state is a stable turn
// start. A CLEANUP state is the actor's post-primary, post-refill holding: it may
// hold up to thirteen tokens and a target prestige whose endgame trigger is not
// yet recorded, owes no draw, and offers more than one joint cleanup.
struct Decision {
    State state;
    DecisionPhase phase{DecisionPhase::Main};
    bool operator==(const Decision&) const = default;
};

// A seat's view of a decision: `observe(decision.state, observer)` with its phase.
// Only `observe` and `reveal` construct one, so a boundary that takes a view (features,
// evaluators, search roots, leaf policies, records, boards) cannot be handed knowledge its
// observer lacks unless its caller chose `reveal`. The decision's actor need not be the
// observer; an authoritative decision for the actor is one whose observer is the actor.
//
// A view of a decision its observer already sees whole (`observed_by`) refers to that
// decision instead of copying it, so it must not outlive it. A view owns its decision
// only when it hides another seat's concealed identities (the projection) or was
// observed from a temporary (moved in). A holder that must outlive an lvalue source
// keeps `decision()` by value.
class PlayerView {
public:
    const Decision& decision() const noexcept { return owned_ ? *owned_ : *source_; }
    const State& state() const noexcept { return decision().state; }
    DecisionPhase phase() const noexcept { return decision().phase; }
    std::uint8_t observer() const noexcept { return observer_; }
    std::uint8_t actor() const noexcept { return state().current_player; }
    bool acting() const noexcept { return observer_ == actor(); }
    bool operator==(const PlayerView& other) const {
        return observer_ == other.observer_ && decision() == other.decision();
    }

private:
    friend PlayerView observe(const Decision& decision, std::uint8_t observer);
    friend PlayerView observe(Decision&& decision, std::uint8_t observer);
    friend PlayerView reveal(const Decision& decision);
    friend PlayerView reveal(Decision&& decision);
    PlayerView(const Decision* source, std::optional<Decision> owned, const std::uint8_t observer)
        : source_(source), owned_(std::move(owned)), observer_(observer) {}

    const Decision* source_;
    std::optional<Decision> owned_;
    std::uint8_t observer_{};
};

PlayerView observe(const Decision& decision, std::uint8_t observer);
PlayerView observe(Decision&& decision, std::uint8_t observer);

// `observe` of the decision's own actor: the view every policy, evaluator and record
// of that decision uses.
inline PlayerView observe_actor(const Decision& decision) {
    return observe(decision, decision.state.current_player);
}
inline PlayerView observe_actor(Decision&& decision) {
    const std::uint8_t actor = decision.state.current_player;
    return observe(std::move(decision), actor);
}

// The actor's view of an omniscient decision with every seat's concealed identity kept:
// the environment's ground truth about the present, never the deck order or a future
// draw. It exists for a training-time critic that may condition on hidden state (a
// centralized value baseline); no policy, evaluator, search root, record or board takes
// one, so a caller of `reveal` must not inform any seat's play. Throws unless
// `omniscient(decision.state)`.
PlayerView reveal(const Decision& decision);
PlayerView reveal(Decision&& decision);

// A legal choice of a decision. Its identity is its fields; its key (`choice_key`) is
// formatted only where a key is published, so generating a choice set formats none.
struct TurnChoice {
    DecisionPhase phase{};
    // MAIN: the primary effect. A take carries its gain/give normal form's net delta, a
    // visible reserve exactly its mandatory gold, a purchase its complete
    // payment. Never a later return or noble. Its own `action_key` stays empty:
    // `choice_key` names the choice.
    CandidateDescriptor primary;
    // MAIN: the visible noble slots eligible once the primary's permanent bonus applies,
    // carried-over eligibility included. A set of possible or forced consequences, never
    // a selection.
    SlotMask eligible_nobles{};
    // CLEANUP: the complete six-colour return and the claimed noble.
    std::array<std::uint8_t, kTokenColorCount> returned{};
    std::optional<std::uint8_t> noble_slot;
    bool operator==(const TurnChoice&) const = default;
};

// A MAIN choice that reserves a tier's top card unseen.
inline bool is_blind_reservation(const TurnChoice& choice) noexcept {
    return choice.phase == DecisionPhase::Main
        && choice.primary.family == CandidateFamily::ReserveBlind;
}

// `main|<primary effect key>` or `cleanup|d=<six returned counts>|n=<slot or ->`.
std::string choice_key(const TurnChoice& choice);

ValidationResult validate_decision(
    const Decision& decision,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

// The decision's legal choices in ascending `choice_key` order; never empty.
std::vector<TurnChoice> decision_choices(
    const Decision& decision,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

// A MAIN choice applied up to its draw.
struct MainOutcome {
    // After the primary effect; an owed market slot is still vacant, an owed blind
    // reservation holds no identity yet.
    State state;
    // The draw still owed. An ending turn leaves a public refill undrawn (no terminal
    // fact or seat's knowledge reads it) but still owes a blind reservation's draw: the
    // owner's world must name the card, so `resolve_main` places it into the final state.
    std::optional<OwedDraw> draw;
    // The owed tier's unknown cards as `state` knows them, in ascending order, each
    // equiprobable: the physical deck of a complete world, or the marginal law of a
    // seat's view.
    std::vector<std::uint16_t> outcome_card_ids;
    // How many joint cleanups follow; the set does not depend on the drawn card.
    std::size_t cleanup_count{};
    // The only cleanup when `cleanup_count` is one, which no decision exposes.
    std::array<std::uint8_t, kTokenColorCount> forced_return{};
    std::optional<std::uint8_t> forced_noble;
    // Set when the forced cleanup ends the game: the turn is adjudicated and `state` is
    // final up to the owed draw.
    std::optional<TerminalFacts> terminal;

    bool awaiting_draw() const noexcept { return draw.has_value(); }
};

// The next decision after a completed step: the same actor's CLEANUP, or the next
// actor's MAIN. With terminal facts, `decision.state` is the final, complete state.
struct TurnStep {
    Decision decision;
    std::optional<TerminalFacts> terminal;
};

// `choice` must be one of `decision_choices(decision)` for this MAIN decision.
MainOutcome apply_main(
    const Decision& decision,
    const TurnChoice& choice,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

// Places `drawn_card`, required exactly when a draw is owed -- in the vacated market
// slot, or as the identity of the blind reservation -- then exposes the CLEANUP
// decision when several cleanups are legal, or applies the only one and completes
// the turn. A consumed outcome lends its state to the step instead of copying it.
TurnStep resolve_main(
    const MainOutcome& outcome,
    std::optional<std::uint16_t> drawn_card,
    std::uint32_t max_game_plies,
    const Catalog& catalog);
TurnStep resolve_main(
    MainOutcome&& outcome,
    std::optional<std::uint16_t> drawn_card,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

// `choice` must be one of `decision_choices(decision)` for this CLEANUP decision:
// return, then the noble, then end-of-turn adjudication and one actor transition.
TurnStep apply_cleanup(
    const Decision& decision,
    const TurnChoice& choice,
    std::uint32_t max_game_plies,
    const Catalog& catalog);

// The choice of `choices` whose `choice_key` is `action_key`; rejects any other key.
const TurnChoice& find_choice(
    const std::vector<TurnChoice>& choices,
    std::string_view action_key);

// Atomic adapter: the MAIN and CLEANUP keys a complete effective leaf of the turn
// start `root` precommits. The leaf's cleanup is fixed before the draw, so a policy
// over leaves is a restricted, information-poorer staged policy; no leaf blind
// reserves, since its return would be fixed before a draw its owner is entitled to see.
struct LeafPath {
    std::string main_key;
    std::string cleanup_key;
    bool operator==(const LeafPath&) const = default;
};

LeafPath leaf_path(const State& root, const CandidateDescriptor& leaf);

}  // namespace splendor
