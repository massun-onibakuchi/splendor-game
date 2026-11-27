#include "splendor/detail/turn.hpp"

#include "splendor/terminal.hpp"

#include <algorithm>
#include <stdexcept>

namespace splendor::detail {

void apply_token_delta(
    State& state,
    const std::uint8_t seat,
    const std::array<std::int8_t, kTokenColorCount>& delta) {
    PlayerState& player = state.players[seat];
    for (std::size_t color = 0; color < delta.size(); ++color) {
        const int value = delta[color];
        if (value > 0) {
            if (state.bank_tokens[color] < value) throw std::logic_error("token delta overdrew the bank");
            state.bank_tokens[color] -= static_cast<std::uint8_t>(value);
            player.tokens[color] += static_cast<std::uint8_t>(value);
        } else if (value < 0) {
            const auto returned = static_cast<std::uint8_t>(-value);
            if (player.tokens[color] < returned) throw std::logic_error("token delta returned more than held");
            player.tokens[color] -= returned;
            state.bank_tokens[color] += returned;
        }
    }
}

void apply_payment(
    State& state,
    const std::uint8_t seat,
    const std::array<std::uint8_t, kTokenColorCount>& payment) {
    PlayerState& player = state.players[seat];
    for (std::size_t color = 0; color < payment.size(); ++color) {
        if (player.tokens[color] < payment[color]) throw std::logic_error("payment exceeds the held tokens");
        player.tokens[color] -= payment[color];
        state.bank_tokens[color] += payment[color];
    }
}

void claim_noble(
    State& state,
    const Catalog& catalog,
    const std::uint8_t seat,
    const std::optional<std::uint8_t> chosen_slot) {
    if (!chosen_slot) return;
    const std::size_t slot = *chosen_slot;
    if (!state.noble_ids[slot]) throw std::logic_error("claimed noble is absent");
    const Noble& noble = catalog.noble(*state.noble_ids[slot]);
    const PlayerState& player = state.players[seat];
    for (std::size_t color = 0; color < kGemColorCount; ++color) {
        if (player.bonuses[color] < noble.requirements[color]) {
            throw std::logic_error("claimed noble is not eligible");
        }
    }
    state.players[seat].prestige += noble.prestige;
    ++state.players[seat].noble_count;
    state.noble_ids[slot].reset();
}

std::optional<std::uint8_t> first_empty_reserve(const PlayerState& player) {
    for (std::size_t slot = 0; slot < player.reserves.size(); ++slot) {
        if (!player.reserves[slot]) return static_cast<std::uint8_t>(slot);
    }
    return std::nullopt;
}

std::optional<OwedDraw> apply_primary_effect(
    State& state,
    const Catalog& catalog,
    const Primary& primary) {
    const std::uint8_t seat = state.current_player;
    apply_token_delta(state, seat, primary.token_delta);
    PlayerState& player = state.players[seat];
    std::optional<OwedDraw> owed;
    switch (primary.family) {
        case PrimaryFamily::Take:
        case PrimaryFamily::PassForced:
            break;
        case PrimaryFamily::ReserveVisible: {
            const std::size_t index = market_index(*primary.source_tier, *primary.source_slot);
            if (state.market_card_ids[index] != primary.target_card_id) {
                throw std::logic_error("reserve target changed");
            }
            const auto destination = first_empty_reserve(player);
            if (!destination) throw std::logic_error("reserve capacity exhausted");
            player.reserves[*destination] = Reserve{*primary.source_tier, primary.target_card_id, false};
            state.market_card_ids[index].reset();
            if (tier_counts(state, catalog).deck[*primary.source_tier - 1] != 0) {
                owed = OwedDraw{DrawKind::Refill, *primary.source_tier, *primary.source_slot, seat};
            }
            break;
        }
        case PrimaryFamily::ReserveBlind: {
            // The deck must still hold a card after accounting for every concealed
            // reservation the state cannot name; the new reservation is one of them
            // until its draw resolves.
            if (tier_counts(state, catalog).deck[*primary.source_tier - 1] == 0) {
                throw std::logic_error("blind reserve of an empty deck");
            }
            const auto destination = first_empty_reserve(player);
            if (!destination) throw std::logic_error("reserve capacity exhausted");
            player.reserves[*destination] = Reserve{*primary.source_tier, std::nullopt, true};
            owed = OwedDraw{DrawKind::BlindReserve, *primary.source_tier, *destination, seat};
            break;
        }
        case PrimaryFamily::BuyVisible:
        case PrimaryFamily::BuyReserved: {
            apply_payment(state, seat, primary.payment);
            if (primary.family == PrimaryFamily::BuyVisible) {
                const std::size_t index =
                    market_index(*primary.source_tier, *primary.source_slot);
                if (state.market_card_ids[index] != primary.target_card_id) {
                    throw std::logic_error("visible buy target changed");
                }
                state.market_card_ids[index].reset();
                if (tier_counts(state, catalog).deck[*primary.source_tier - 1] != 0) {
                    owed = OwedDraw{DrawKind::Refill, *primary.source_tier, *primary.source_slot, seat};
                }
            } else {
                if (primary.reserve_owner != seat
                    || !player.reserves[*primary.reserve_slot]
                    || player.reserves[*primary.reserve_slot]->card_id
                        != primary.target_card_id) {
                    throw std::logic_error("reserved buy target changed");
                }
                player.reserves[*primary.reserve_slot].reset();
            }
            const Card& card = catalog.card(*primary.target_card_id);
            ++player.bonuses[static_cast<std::size_t>(card.bonus)];
            player.prestige += card.prestige;
            ++player.purchased_cards;
            break;
        }
    }
    return owed;
}

void place_draw(
    State& state,
    const Catalog& catalog,
    const OwedDraw& owed,
    const std::uint16_t card_id) {
    if (!state.unknown_card_ids.contains(card_id)
        || catalog.card(card_id).tier != owed.tier) {
        throw std::invalid_argument("card is not a draw outcome of the owed tier");
    }
    switch (owed.kind) {
        case DrawKind::Refill: {
            std::optional<std::uint16_t>& slot =
                state.market_card_ids[market_index(owed.tier, owed.slot)];
            if (slot) throw std::logic_error("owed market slot is already filled");
            slot = card_id;
            break;
        }
        case DrawKind::BlindReserve: {
            std::optional<Reserve>& reserve = state.players[owed.owner].reserves[owed.slot];
            if (!reserve || reserve->card_id || !reserve->concealed || reserve->tier != owed.tier) {
                throw std::logic_error("owed blind reservation is not awaiting its draw");
            }
            reserve->card_id = card_id;
            break;
        }
    }
    state.unknown_card_ids.erase(card_id);
}

std::optional<TerminalFacts> finish_turn_state(
    State& state,
    const std::uint32_t max_game_plies) {
    const std::uint8_t seat = state.current_player;
    if (!state.endgame_triggered && state.players[seat].prestige >= kTargetPrestige) {
        state.endgame_triggered = true;
        state.endgame_trigger_player = seat;
    }
    ++state.game_ply;
    state.current_player =
        static_cast<std::uint8_t>((state.starting_player + state.game_ply) % state.player_count);
    return terminal_facts(state, max_game_plies);
}

}  // namespace splendor::detail
