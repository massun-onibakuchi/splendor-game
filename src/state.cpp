#include "splendor/state.hpp"

#include "splendor/seed.hpp"
#include "splendor/terminal.hpp"

#include <algorithm>
#include <array>
#include <numeric>
#include <stdexcept>

namespace splendor {
namespace {

ValidationResult invalid(std::string message) {
    return ValidationResult{false, std::move(message)};
}

template <typename T, std::size_t N>
std::size_t sum_array(const std::array<T, N>& values) {
    return std::accumulate(values.begin(), values.end(), std::size_t{0});
}

}  // namespace

std::size_t token_count(const PlayerState& player) noexcept {
    return sum_array(player.tokens);
}

std::size_t reserve_count(const PlayerState& player) noexcept {
    return static_cast<std::size_t>(std::count_if(
        player.reserves.begin(),
        player.reserves.end(),
        [](const auto& value) { return value.has_value(); }));
}

TierCounts tier_counts(const State& state, const Catalog& catalog) {
    TierCounts counts;
    for (const std::uint16_t id : state.unknown_card_ids) ++counts.unknown[catalog.card(id).tier - 1];
    counts.deck = counts.unknown;
    for (const PlayerState& player : seated(state)) {
        for (const auto& reserve : player.reserves) {
            if (!reserve || reserve->card_id) continue;
            std::size_t& deck = counts.deck[reserve->tier - 1];
            if (deck == 0) throw std::logic_error("unknown set is smaller than its concealed reserves");
            --deck;
        }
    }
    return counts;
}

bool knows_own_reserves(const State& state, const std::uint8_t seat) noexcept {
    return std::all_of(
        state.players[seat].reserves.begin(), state.players[seat].reserves.end(),
        [](const auto& reserve) { return !reserve || reserve->card_id.has_value(); });
}

bool omniscient(const State& state) noexcept {
    for (std::uint8_t seat = 0; seat < state.player_count; ++seat) {
        if (!knows_own_reserves(state, seat)) return false;
    }
    return true;
}

bool observed_by(const State& state, const std::uint8_t observer) noexcept {
    if (observer >= state.player_count || !knows_own_reserves(state, observer)) return false;
    for (std::uint8_t seat = 0; seat < state.player_count; ++seat) {
        if (seat == observer) continue;
        for (const auto& reserve : state.players[seat].reserves) {
            if (reserve && reserve->concealed && reserve->card_id) return false;
        }
    }
    return true;
}

State observe(const State& state, const std::uint8_t observer) {
    if (observer >= state.player_count) throw std::invalid_argument("observer seat is out of range");
    if (!knows_own_reserves(state, observer)) {
        throw std::invalid_argument("state does not carry the observer's own reserve identities");
    }
    State view = state;
    bool widened = false;
    for (std::uint8_t seat = 0; seat < state.player_count; ++seat) {
        if (seat == observer) continue;
        for (auto& reserve : view.players[seat].reserves) {
            if (!reserve || !reserve->concealed || !reserve->card_id) continue;
            view.unknown_card_ids.push_back(*reserve->card_id);
            reserve->card_id.reset();
            widened = true;
        }
    }
    if (widened) std::sort(view.unknown_card_ids.begin(), view.unknown_card_ids.end());
    return view;
}

ValidationResult validate_state(
    const State& state,
    const std::uint32_t max_game_plies,
    const Catalog& catalog,
    const bool require_nonterminal,
    const StateBoundary boundary) {
    const bool intermediate = boundary == StateBoundary::Intermediate;
    if (max_game_plies == 0) return invalid("max_game_plies must be positive");
    if (!supported_player_count(state.player_count)) return invalid("player count is unsupported");
    const std::uint8_t seats = state.player_count;
    if (state.current_player >= seats || state.starting_player >= seats) {
        return invalid("player index is out of range");
    }
    if (state.current_player != (state.starting_player + state.game_ply) % seats) {
        return invalid("current player does not match starting player and game ply");
    }
    if (state.endgame_triggered != state.endgame_trigger_player.has_value()) {
        return invalid("endgame trigger presence is inconsistent");
    }
    if (state.endgame_trigger_player && *state.endgame_trigger_player >= seats) {
        return invalid("endgame trigger player is out of range");
    }
    if (state.endgame_trigger_player
        && state.players[*state.endgame_trigger_player].prestige < kTargetPrestige) {
        return invalid("endgame trigger player has insufficient prestige");
    }
    if (std::any_of(state.players.begin() + seats, state.players.end(), [](const PlayerState& player) {
            return player != PlayerState{};
        })) {
        return invalid("an unseated player holds game material");
    }
    if (!intermediate && !state.endgame_triggered
        && std::any_of(state.players.begin(), state.players.end(), [](const PlayerState& player) {
               return player.prestige >= kTargetPrestige;
           })) {
        return invalid("target prestige reached before endgame trigger");
    }
    if (require_nonterminal && terminal_facts(state, max_game_plies)) {
        return invalid("state is terminal");
    }

    std::array<bool, kCardCount> seen_cards{};
    // Why a card cannot join the state, or null when it is known and not yet seen.
    auto add_card = [&](const std::uint16_t id) -> const char* {
        if (id >= kCardCount) return " card id is unknown";
        if (seen_cards[id]) return " card id is duplicated";
        seen_cards[id] = true;
        return nullptr;
    };
    const auto card_failure = [](const char* zone, const char* reason) {
        return invalid(std::string(zone) + reason);
    };

    std::array<std::size_t, kTierCount> unknown_counts{};
    if (!std::is_sorted(state.unknown_card_ids.begin(), state.unknown_card_ids.end())
        || std::adjacent_find(state.unknown_card_ids.begin(), state.unknown_card_ids.end())
            != state.unknown_card_ids.end()) {
        return invalid("unknown card ids must be strictly ascending");
    }
    for (const std::uint16_t id : state.unknown_card_ids) {
        if (const char* reason = add_card(id)) return card_failure("unknown set", reason);
        ++unknown_counts[catalog.card(id).tier - 1];
    }
    for (std::size_t index = 0; index < state.market_card_ids.size(); ++index) {
        if (!state.market_card_ids[index]) continue;
        const std::uint16_t id = *state.market_card_ids[index];
        if (const char* reason = add_card(id)) return card_failure("market", reason);
        if (catalog.card(id).tier != index / kMarketSlotsPerTier + 1) {
            return invalid("market card is in the wrong tier");
        }
    }

    std::size_t purchased_count = 0;
    std::size_t claimed_nobles = 0;
    // Concealed reserves without an identity, per tier: their cards are in the unknown set.
    std::array<std::size_t, kTierCount> concealed_counts{};
    for (std::size_t seat = 0; seat < seats; ++seat) {
        const PlayerState& player = state.players[seat];
        if (token_count(player) > (intermediate && seat == state.current_player ? 13U : 10U)) {
            return invalid("player exceeds the token limit");
        }
        if (sum_array(player.bonuses) != player.purchased_cards) {
            return invalid("player bonus count does not match purchased cards");
        }
        purchased_count += player.purchased_cards;
        claimed_nobles += player.noble_count;
        for (const auto& reserve : player.reserves) {
            if (!reserve) continue;
            if (reserve->tier < 1 || reserve->tier > kTierCount) {
                return invalid("reserve tier is out of range");
            }
            if (reserve->card_id) {
                if (const char* reason = add_card(*reserve->card_id)) {
                    return card_failure("reserve", reason);
                }
                if (catalog.card(*reserve->card_id).tier != reserve->tier) {
                    return invalid("reserve tier does not match its card");
                }
            } else {
                if (!reserve->concealed) {
                    return invalid("a reserve without an identity must be concealed");
                }
                ++concealed_counts[reserve->tier - 1];
            }
        }
    }
    if (static_cast<std::size_t>(std::count(seen_cards.begin(), seen_cards.end(), true))
            + purchased_count
        != kCardCount) {
        return invalid("card conservation failed");
    }
    for (std::size_t tier = 0; tier < kTierCount; ++tier) {
        if (concealed_counts[tier] > unknown_counts[tier]) {
            return invalid("concealed reserves exceed the unknown cards of their tier");
        }
    }

    const std::size_t nobles = visible_noble_count(seats);
    if (std::any_of(state.noble_ids.begin() + static_cast<std::ptrdiff_t>(nobles), state.noble_ids.end(),
            [](const auto& id) { return id.has_value(); })) {
        return invalid("a noble lies past the seat count's visible slots");
    }
    std::array<bool, kNobleCount> seen_nobles{};
    std::size_t visible_nobles = 0;
    for (const auto id : state.noble_ids) {
        if (!id) continue;
        if (*id >= kNobleCount || seen_nobles[*id]) return invalid("invalid visible noble id");
        static_cast<void>(catalog.noble(*id));
        seen_nobles[*id] = true;
        ++visible_nobles;
    }
    if (visible_nobles + claimed_nobles != nobles) {
        return invalid("noble conservation failed");
    }

    for (std::size_t color = 0; color < kTokenColorCount; ++color) {
        const std::size_t supply =
            color == static_cast<std::size_t>(Color::Gold) ? kGoldSupply : gem_supply(seats);
        std::size_t total = state.bank_tokens[color];
        for (const PlayerState& player : seated(state)) total += player.tokens[color];
        if (total != supply) return invalid("token conservation failed");
    }
    for (std::size_t tier = 0; tier < kTierCount; ++tier) {
        const bool market_gap = std::any_of(
            state.market_card_ids.begin() + tier * kMarketSlotsPerTier,
            state.market_card_ids.begin() + (tier + 1) * kMarketSlotsPerTier,
            [](const auto& value) { return !value.has_value(); });
        const std::size_t deck = unknown_counts[tier] - concealed_counts[tier];
        if (market_gap && deck != 0 && !intermediate) {
            return invalid("market has an empty slot while its deck is nonempty");
        }
    }
    return ValidationResult{true, {}};
}

State initial_state(
    const Catalog& catalog,
    const std::uint8_t player_count,
    const std::uint32_t max_game_plies,
    const std::uint64_t setup_seed) {
    if (!supported_player_count(player_count)) {
        throw std::invalid_argument("player count is unsupported");
    }
    if (max_game_plies == 0) {
        throw std::invalid_argument("max_game_plies must be positive");
    }
    State state;
    state.player_count = player_count;
    state.current_player = 0;
    state.starting_player = 0;
    const std::uint8_t gems = gem_supply(player_count);
    state.bank_tokens = {gems, gems, gems, gems, gems, kGoldSupply};

    std::uint64_t counter = 0;
    for (std::uint8_t tier = 1; tier <= kTierCount; ++tier) {
        std::vector<std::uint16_t> ids = catalog.card_ids_for_tier(tier);
        for (std::size_t index = ids.size() - 1; index > 0; --index) {
            const std::size_t other = random_counter(setup_seed, counter++) % (index + 1);
            std::swap(ids[index], ids[other]);
        }
        for (std::size_t slot = 0; slot < kMarketSlotsPerTier; ++slot) {
            state.market_card_ids[(tier - 1) * kMarketSlotsPerTier + slot] = ids[slot];
        }
        state.unknown_card_ids.insert(
            state.unknown_card_ids.end(),
            ids.begin() + kMarketSlotsPerTier,
            ids.end());
    }
    std::sort(state.unknown_card_ids.begin(), state.unknown_card_ids.end());

    std::array<std::uint16_t, kNobleCount> noble_ids{};
    std::iota(noble_ids.begin(), noble_ids.end(), std::uint16_t{0});
    for (std::size_t index = noble_ids.size() - 1; index > 0; --index) {
        const std::size_t other = random_counter(setup_seed, counter++) % (index + 1);
        std::swap(noble_ids[index], noble_ids[other]);
    }
    for (std::size_t slot = 0; slot < visible_noble_count(player_count); ++slot) {
        state.noble_ids[slot] = noble_ids[slot];
    }
    if (const ValidationResult result = validate_state(
            state, max_game_plies, catalog); !result) {
        throw std::logic_error("generated invalid initial state: " + result.message);
    }
    return state;
}

}  // namespace splendor
