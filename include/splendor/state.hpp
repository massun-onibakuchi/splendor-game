#pragma once

#include "splendor/contracts.hpp"
#include "splendor/catalog.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace splendor {

// One reserved development card. `card_id` is absent exactly when the state's knowledge
// does not carry the identity: a blind reservation made by a seat other than the one
// whose knowledge the state holds. A concealed reserve was drawn blind and is still
// known to its owner alone; it becomes public only by being purchased.
struct Reserve {
    std::uint8_t tier{};
    std::optional<std::uint16_t> card_id;
    bool concealed{};
    bool operator==(const Reserve&) const = default;
};

struct PlayerState {
    std::array<std::uint8_t, kTokenColorCount> tokens{};
    std::array<std::uint8_t, kGemColorCount> bonuses{};
    std::uint16_t prestige{};
    std::uint8_t purchased_cards{};
    std::uint8_t noble_count{};
    std::array<std::optional<Reserve>, kReserveSlotsPerPlayer> reserves{};
    bool operator==(const PlayerState&) const = default;
};

// A game state together with the knowledge it carries. Knowledge lives only in the
// reserve identities: the unknown set holds every card whose location this state
// cannot name -- the physical deck plus the concealed reserves without an identity. A
// state carrying every identity is omniscient (the environment's ground truth or a
// hypothetical world a search samples); `observe` projects it to one seat's knowledge.
//
// `player_count` seats play, in cyclic order from `starting_player`; the remaining
// entries of `players` are empty, as are the visible-noble slots past
// `visible_noble_count(player_count)`.
struct State {
    std::uint8_t player_count{};
    std::uint8_t current_player{};
    std::uint8_t starting_player{};
    std::uint32_t game_ply{};
    bool endgame_triggered{};
    std::optional<std::uint8_t> endgame_trigger_player;
    std::array<PlayerState, kMaxPlayers> players{};
    std::array<std::uint8_t, kTokenColorCount> bank_tokens{};
    std::array<std::optional<std::uint16_t>, kMarketSlotCount> market_card_ids{};
    CardSet unknown_card_ids;
    std::array<std::optional<std::uint16_t>, kMaxVisibleNobles> noble_ids{};
    bool operator==(const State&) const = default;
};

// The seated players, in seat order.
inline std::span<const PlayerState> seated(const State& state) noexcept {
    return {state.players.data(), state.player_count};
}
inline std::span<PlayerState> seated(State& state) noexcept {
    return {state.players.data(), state.player_count};
}

struct ValidationResult {
    bool valid{};
    std::string message;
    explicit operator bool() const noexcept { return valid; }
};

// `Stable` is a turn-start state. `Intermediate` is a within-turn state of the staged
// surface: at most thirteen tokens held by the acting seat (every other seat ended its
// turn within ten), one draw still owed (a vacant market slot or a reserve awaiting its
// blind draw), and the endgame trigger not yet recorded. Every
// other invariant -- card, noble and token conservation, tier placement, reserve
// bounds, knowledge consistency -- holds in both.
enum class StateBoundary : std::uint8_t { Stable, Intermediate };

ValidationResult validate_state(
    const State& state,
    std::uint32_t max_game_plies,
    const Catalog& catalog,
    bool require_nonterminal = true,
    StateBoundary boundary = StateBoundary::Stable);

// The setup of a `player_count`-seat game: shuffled decks and nobles, the seat count's
// supply, seat 0 to start. Two seats' setup draws the same cards and nobles from a seed
// as any other count's, which only reveals more nobles.
State initial_state(
    const Catalog& catalog,
    std::uint8_t player_count,
    std::uint32_t max_game_plies,
    std::uint64_t setup_seed);

std::size_t token_count(const PlayerState& player) noexcept;
std::size_t reserve_count(const PlayerState& player) noexcept;

// The seat acting after `seat` in turn order.
constexpr std::uint8_t next_seat(const State& state, const std::uint8_t seat) noexcept {
    return static_cast<std::uint8_t>((seat + 1) % state.player_count);
}

// Whether the acting seat closes the round: the last seat before the starting player.
// Only its turn can end the game before the ply cap.
constexpr bool closes_round(const State& state) noexcept {
    return next_seat(state, state.current_player) == state.starting_player;
}

// Per tier, the cards the state cannot place (`unknown`) and, of those, the cards
// physically in the deck (`deck`): the tier's unknown cards that are not concealed
// reserves without an identity. Refill and blind-reserve legality read `deck`, never
// the unknown set. Three set intersections and one pass over the reserves.
struct TierCounts {
    std::array<std::size_t, kTierCount> unknown{};
    std::array<std::size_t, kTierCount> deck{};
};
TierCounts tier_counts(const State& state, const Catalog& catalog);

// Whether every reserve of `seat` carries its identity.
bool knows_own_reserves(const State& state, std::uint8_t seat) noexcept;

// Whether every reserve of every seat carries its identity.
bool omniscient(const State& state) noexcept;

// Whether `state` is exactly as `observer` knows it -- `observe(state, observer) == state`
// -- without building the projection: the observer knows its own reserves and no other
// seat's concealed reserve carries an identity.
bool observed_by(const State& state, std::uint8_t observer) noexcept;

// `state` as `observer` knows it: every concealed reserve of another seat loses its
// identity to the unknown set. Idempotent. Throws when `state` lacks the observer's own
// reserve identities, since knowledge cannot be manufactured from a narrower view.
State observe(const State& state, std::uint8_t observer);

}  // namespace splendor
