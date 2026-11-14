#pragma once

#include "splendor/turn.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Within-turn primitives of the staged-action turn. Each applies or enumerates exactly
// one consequence of a turn, so every caller of a consequence shares its one
// implementation.
namespace splendor::detail {

using TokenCounts = std::array<int, kTokenColorCount>;

// One take's net token delta in gain/give normal form.
using TokenDelta = std::array<std::int8_t, kTokenColorCount>;

// The tier-major index (0-11) of market slot `slot` of `tier` (1-3).
constexpr std::size_t market_index(const std::uint8_t tier, const std::uint8_t slot) noexcept {
    return (static_cast<std::size_t>(tier) - 1) * kMarketSlotsPerTier + slot;
}

// The visible noble slots the supplied bonuses qualify for.
inline SlotMask eligible_noble_mask(
    const State& state,
    const Catalog& catalog,
    const std::array<std::uint8_t, kGemColorCount>& bonuses) {
    SlotMask mask = 0;
    for (std::size_t slot = 0; slot < state.noble_ids.size(); ++slot) {
        if (!state.noble_ids[slot]) continue;
        const Noble& noble = catalog.noble(*state.noble_ids[slot]);
        bool eligible = true;
        for (std::size_t color = 0; color < kGemColorCount; ++color) {
            eligible = eligible && bonuses[color] >= noble.requirements[color];
        }
        if (eligible) mask = static_cast<SlotMask>(mask | (1U << slot));
    }
    return mask;
}

// Appends the visible noble slots the supplied bonuses qualify for, in ascending order.
template <typename Slot>
void append_eligible_nobles(
    const State& state,
    const Catalog& catalog,
    const std::array<std::uint8_t, kGemColorCount>& bonuses,
    std::vector<Slot>& out) {
    const SlotMask mask = eligible_noble_mask(state, catalog, bonuses);
    for (std::size_t slot = 0; slot < kMaxVisibleNobles; ++slot) {
        if (contains(mask, slot)) out.push_back(static_cast<std::uint8_t>(slot));
    }
}

// Eligible noble slots for the supplied bonuses, or one absent choice when no
// noble is eligible.
inline void noble_choices(
    const State& state,
    const Catalog& catalog,
    const std::array<std::uint8_t, kGemColorCount>& bonuses,
    std::vector<std::optional<std::uint8_t>>& out) {
    out.clear();
    append_eligible_nobles(state, catalog, bonuses, out);
    if (out.empty()) out.push_back(std::nullopt);
}

// Appends every return of `left` tokens drawn from `after[color..]` to `out`, in
// ascending count order per colour. `room[color]` is the sum of `after[color..]`; a
// branch that cannot return `left` from what remains is skipped, and a branch with
// nothing left to return is complete, so only emitted returns are visited.
inline void append_discards(
    const TokenCounts& after,
    const std::array<int, kTokenColorCount + 1>& room,
    const std::size_t color,
    const int left,
    TokenCounts& current,
    std::vector<TokenCounts>& out) {
    if (left == 0) {
        out.push_back(current);
        return;
    }
    if (room[color] < left) return;
    for (int count = 0; count <= std::min(after[color], left); ++count) {
        current[color] = count;
        append_discards(after, room, color + 1, left - count, current, out);
    }
    current[color] = 0;
}

// Complete return vectors that bring the holding back to ten tokens, or one
// empty vector when the take leaves no excess.
inline void discard_variants(
    const PlayerState& player,
    const TokenCounts& take,
    std::vector<TokenCounts>& out) {
    out.clear();
    TokenCounts after{};
    for (std::size_t color = 0; color < after.size(); ++color) {
        after[color] = player.tokens[color] + take[color];
    }
    std::array<int, kTokenColorCount + 1> room{};
    for (std::size_t color = after.size(); color-- > 0;) room[color] = room[color + 1] + after[color];
    TokenCounts current{};
    append_discards(after, room, 0, std::max(0, room[0] - 10), current, out);
}

// Every official token take of the state's current actor before any return, written over
// `takes`: three distinct available colours (all of them, never fewer, when fewer than
// three are available), or two of one colour with at least four in the bank.
void token_takes(const State& state, std::vector<TokenCounts>& takes);

// Every take of the state's current actor in gain/give normal form, as net deltas
// written over `deltas`, grouped by gain: a gain `G`, a sub-multiset of an official take
// (`token_takes`), less a return `R` of colours outside `G`, gold allowed, with
// `|R| = max(0, held + |G| - 10)`. A gain that is not itself an official take is legal
// only when the hand ends at exactly ten. Each net delta of an official take followed by
// any complete return appears exactly once, as `(G, R) = (delta+, delta-)`; the empty gain
// at ten held tokens is the null move.
void normal_form_takes(const State& state, std::vector<TokenDelta>& deltas);

// Moves `delta` between the bank and `seat`'s holding; token movement is always paired
// with the bank, so conservation has one authority.
void apply_token_delta(
    State& state,
    std::uint8_t seat,
    const std::array<std::int8_t, kTokenColorCount>& delta);

// Moves `payment` from `seat`'s holding to the bank.
void apply_payment(
    State& state,
    std::uint8_t seat,
    const std::array<std::uint8_t, kTokenColorCount>& payment);

// Grants `seat` the noble of visible slot `chosen_slot`, when one is chosen.
void claim_noble(
    State& state,
    const Catalog& catalog,
    std::uint8_t seat,
    std::optional<std::uint8_t> chosen_slot);

std::optional<std::uint8_t> first_empty_reserve(const PlayerState& player);

// Applies a primary effect for the state's current actor: its token movement, its
// reservation or purchase, and the permanent bonus and prestige a purchase grants.
// Returns the draw the primary owes: the vacated market slot of a tier that still has
// cards, or the blind reservation awaiting its identity.
std::optional<OwedDraw> apply_primary_effect(
    State& state,
    const Catalog& catalog,
    const Primary& primary);

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

// Appends a MAIN choice's primary key, `<family>|d=..|p=..|z=<zone>[|t=][|s=][|o=][|r=][|c=]`,
// to `output`.
void append_primary_key(std::string& output, const Primary& primary);

// Appends `choice_key(choice)` to `output`, so a writer of many keys reuses one buffer.
void append_choice_key(std::string& output, const TurnChoice& choice);

}  // namespace splendor::detail
