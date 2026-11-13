#include "splendor/candidate.hpp"

#include "splendor/transition.hpp"
#include "splendor/detail/candidate.hpp"
#include "splendor/detail/payment.hpp"
#include "splendor/detail/turn.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace splendor {
namespace {

using detail::TokenCounts;
using detail::discard_variants;
using detail::noble_choices;

class CandidateBuilder {
public:
    CandidateBuilder(const State& state, const Catalog& catalog)
        : state_(state), catalog_(catalog), player_(state.players[state.current_player]) {}

    CandidateBatch build() {
        add_takes();
        add_reserves();
        add_buys();
        if (rows_.empty()) {
            CandidateDescriptor pass;
            pass.family = CandidateFamily::PassForced;
            emit_with_nobles(pass, player_.bonuses);
        }
        std::stable_sort(
            rows_.begin(), rows_.end(),
            [](const CandidateDescriptor& lhs, const CandidateDescriptor& rhs) {
                return lhs.action_key < rhs.action_key;
            });
        CandidateBatch result;
        result.rows.reserve(rows_.size());
        std::size_t index = 0;
        while (index < rows_.size()) {
            std::size_t next = index + 1;
            while (next < rows_.size() && rows_[next].action_key == rows_[index].action_key) ++next;
            result.rows.push_back(std::move(rows_[index]));
            index = next;
        }
        return result;
    }

private:
    void emit_with_nobles(
        CandidateDescriptor base,
        const std::array<std::uint8_t, kGemColorCount>& bonuses) {
        noble_choices(state_, catalog_, bonuses, noble_scratch_);
        for (const auto noble : noble_scratch_) {
            CandidateDescriptor descriptor = base;
            descriptor.chosen_noble_slot = noble;
            descriptor.action_key = encode_action_key(descriptor);
            rows_.push_back(std::move(descriptor));
        }
    }

    // One leaf per normal-form take, the staged take's net delta: a take is complete at
    // MAIN, so the leaf and its staged path name the same delta.
    void add_takes() {
        detail::normal_form_takes(state_, delta_scratch_);
        for (const detail::TokenDelta& delta : delta_scratch_) {
            CandidateDescriptor descriptor;
            descriptor.family = CandidateFamily::Take;
            descriptor.token_delta = delta;
            emit_with_nobles(descriptor, player_.bonuses);
        }
    }

    void add_reserve_variant(
        const std::size_t market_index,
        const std::uint16_t card_id,
        const bool with_gold) {
        TokenCounts take{};
        if (with_gold) take[static_cast<std::size_t>(Color::Gold)] = 1;
        discard_variants(player_, take, discard_scratch_);
        for (const TokenCounts& discard : discard_scratch_) {
            CandidateDescriptor descriptor;
            descriptor.family = CandidateFamily::ReserveVisible;
            descriptor.source_zone = SourceZone::Market;
            descriptor.source_tier = static_cast<std::uint8_t>(market_index / kMarketSlotsPerTier + 1);
            descriptor.source_slot = static_cast<std::uint8_t>(market_index % kMarketSlotsPerTier);
            descriptor.target_card_id = card_id;
            for (std::size_t color = 0; color < kTokenColorCount; ++color) {
                descriptor.token_delta[color] = static_cast<std::int8_t>(take[color] - discard[color]);
            }
            emit_with_nobles(descriptor, player_.bonuses);
        }
    }

    void add_reserves() {
        if (reserve_count(player_) >= kReserveSlotsPerPlayer) return;
        const bool gold_available = state_.bank_tokens[static_cast<std::size_t>(Color::Gold)] != 0;
        for (std::size_t index = 0; index < state_.market_card_ids.size(); ++index) {
            if (!state_.market_card_ids[index]) continue;
            add_reserve_variant(index, *state_.market_card_ids[index], gold_available);
        }
    }

    void add_buy_candidate(
        const CandidateFamily family,
        const SourceZone zone,
        const std::uint16_t card_id,
        const std::optional<std::uint8_t> tier,
        const std::optional<std::uint8_t> market_slot,
        const std::optional<std::uint8_t> reserve_slot) {
        const Card& card = catalog_.card(card_id);
        auto bonuses = player_.bonuses;
        ++bonuses[static_cast<std::size_t>(card.bonus)];
        detail::payment_variants(player_, card, payment_scratch_);
        for (const auto& payment : payment_scratch_) {
            CandidateDescriptor descriptor;
            descriptor.family = family;
            descriptor.source_zone = zone;
            descriptor.source_tier = tier;
            descriptor.source_slot = market_slot;
            descriptor.target_card_id = card_id;
            descriptor.payment = payment;
            if (reserve_slot) {
                descriptor.reserve_owner = state_.current_player;
                descriptor.reserve_slot = reserve_slot;
            }
            emit_with_nobles(descriptor, bonuses);
        }
    }

    void add_buys() {
        for (std::size_t index = 0; index < state_.market_card_ids.size(); ++index) {
            if (!state_.market_card_ids[index]) continue;
            add_buy_candidate(
                CandidateFamily::BuyVisible,
                SourceZone::Market,
                *state_.market_card_ids[index],
                static_cast<std::uint8_t>(index / kMarketSlotsPerTier + 1),
                static_cast<std::uint8_t>(index % kMarketSlotsPerTier),
                std::nullopt);
        }
        // A leaf policy that cannot name a reserve's card cannot price its purchase: a
        // concealed reserve of another seat, seen from a narrower view, offers no leaf.
        for (std::size_t slot = 0; slot < player_.reserves.size(); ++slot) {
            const std::optional<Reserve>& reserve = player_.reserves[slot];
            if (!reserve || !reserve->card_id) continue;
            add_buy_candidate(
                CandidateFamily::BuyReserved,
                SourceZone::SelfReserve,
                *reserve->card_id,
                reserve->tier,
                std::nullopt,
                static_cast<std::uint8_t>(slot));
        }
    }

    const State& state_;
    const Catalog& catalog_;
    const PlayerState& player_;
    std::vector<CandidateDescriptor> rows_;
    std::vector<std::optional<std::uint8_t>> noble_scratch_;
    std::vector<TokenCounts> discard_scratch_;
    std::vector<detail::TokenDelta> delta_scratch_;
    std::vector<std::array<std::uint8_t, kTokenColorCount>> payment_scratch_;
};

}  // namespace

CandidateBatch generate_candidates(
    const State& state,
    const std::uint32_t max_game_plies,
    const Catalog& catalog) {
    if (terminal_facts(state, max_game_plies)) return {};
    return CandidateBuilder(state, catalog).build();
}

}  // namespace splendor
