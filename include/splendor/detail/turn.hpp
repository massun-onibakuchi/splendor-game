#pragma once

#include "splendor/detail/candidate.hpp"
#include "splendor/turn.hpp"

#include <cstdint>
#include <optional>

// Within-turn effect primitives of the staged-action turn. Each applies exactly one
// consequence of a turn, so a staged path and the complete effective leaf it
// realizes stay one implementation.
namespace splendor::detail {

// Every official token take of the state's current actor before any return, written over
// `takes`: three distinct available colours (all of them, never fewer, when fewer than
// three are available), or two of one colour with at least four in the bank.
void token_takes(const State& state, std::vector<TokenCounts>& takes);

// One take's net token delta in gain/give normal form.
using TokenDelta = std::array<std::int8_t, kTokenColorCount>;

// Every take of the state's current actor in gain/give normal form, as net deltas
// written over `deltas`, grouped by gain: a gain `G`, a sub-multiset of an official take
// (`token_takes`), less a return `R` of colours outside `G`, gold allowed, with
// `|R| = max(0, held + |G| - 10)`. A gain that is not itself an official take is legal
// only when the hand ends at exactly ten. Each net delta of an official take followed by
// any complete return appears exactly once, as `(G, R) = (delta+, delta-)`; the empty gain
// at ten held tokens is the null move.
void normal_form_takes(const State& state, std::vector<TokenDelta>& deltas);

// Applies a primary effect for the state's current actor: its token movement (a staged
// primary's take, or a complete leaf's net take less its return), its reservation or
// purchase, and the permanent bonus and prestige a purchase grants. Returns the draw the
// primary owes: the vacated market slot of a tier that still has cards, or the blind
// reservation awaiting its identity. A descriptor's noble is not part of the effect.
std::optional<OwedDraw> apply_primary_effect(
    State& state,
    const Catalog& catalog,
    const CandidateDescriptor& primary);

// Places the drawn card where the draw is owed -- the market slot, or the identity of
// the actor's blind reservation -- and removes it from the unknown set in the same
// step, so no observation sees one without the other.
void place_draw(
    State& state,
    const Catalog& catalog,
    const OwedDraw& owed,
    std::uint16_t card_id);

// End-of-turn adjudication: the endgame trigger, the ply, the one actor
// transition, and the terminal facts they imply.
std::optional<TerminalFacts> finish_turn_state(
    State& state,
    std::uint32_t max_game_plies);

// A CLEANUP state owes no draw: no slot of a tier that still has cards is vacant, and
// every reserve of the actor carries its identity.
ValidationResult validate_resolved_draws(const State& state, const Catalog& catalog);

// Every joint cleanup of the actor's current holding and bonuses, in ascending
// key order: each complete return crossed with each eligible noble, or the one
// empty cleanup when neither obligation exists.
std::vector<TurnChoice> cleanup_choices(const State& state, const Catalog& catalog);

// How many joint cleanups `cleanup_choices(state, catalog)` holds, without building
// them, and, when exactly one is legal, that forced cleanup's return and noble.
struct CleanupSupport {
    std::size_t count{};
    std::array<std::uint8_t, kTokenColorCount> forced_return{};
    std::optional<std::uint8_t> forced_noble;
};
CleanupSupport cleanup_support(const State& state, const Catalog& catalog);

// Fills `choices` with `decision_choices(decision)` for a decision already known valid,
// such as one a transition produced: no validation is repeated. Choices already in
// `choices` are overwritten in place, so a recycled set refills without allocating.
void legal_choices(
    const Decision& decision,
    const Catalog& catalog,
    std::vector<TurnChoice>& choices);

// Appends `choice_key(choice)` to `output`, so a writer of many keys reuses one buffer.
void append_choice_key(std::string& output, const TurnChoice& choice);

}  // namespace splendor::detail
