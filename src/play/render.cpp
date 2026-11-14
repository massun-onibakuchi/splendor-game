#include "splendor/play/render.hpp"

#include "splendor/detail/transition.hpp"
#include "splendor/detail/turn.hpp"

#include <algorithm>
#include <type_traits>
#include <vector>

namespace splendor::play {
namespace {

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

// The first `count` entries of a seat- or slot-indexed array.
template <typename T, std::size_t N>
std::span<const T> head(const std::array<T, N>& values, const std::size_t count) {
    return {values.data(), count};
}

constexpr std::array<std::string_view, kTokenColorCount> kColorNames{
    "white", "blue", "green", "red", "black", "gold"};

std::vector<std::uint8_t> seats_of(const SeatMask mask, const std::uint8_t player_count) {
    std::vector<std::uint8_t> seats;
    for (std::uint8_t seat = 0; seat < player_count; ++seat) {
        if (contains(mask, seat)) seats.push_back(seat);
    }
    return seats;
}

// `count color` for each colour whose signed count is positive, or "nothing".
template <typename T>
std::string token_list(const std::array<T, kTokenColorCount>& counts, const int sign = 1) {
    std::string out;
    for (std::size_t color = 0; color < kTokenColorCount; ++color) {
        const int count = sign * static_cast<int>(counts[color]);
        if (count <= 0) continue;
        if (!out.empty()) out += ", ";
        out += std::to_string(count) + ' ' + std::string(kColorNames[color]);
    }
    return out.empty() ? "nothing" : out;
}

}  // namespace

std::string board_json(
    const PlayerView& view,
    const Catalog& catalog,
    const std::optional<TerminalFacts>& terminal) {
    const State& state = view.state();
    const std::string_view phase = terminal ? "terminal" : decision_phase_name(view.phase());
    std::string out = "{\"observer\":" + json(view.observer())
        + ",\"current_player\":" + json(state.current_player)
        + ",\"starting_player\":" + json(state.starting_player)
        + ",\"game_ply\":" + json(state.game_ply)
        + ",\"phase\":\"" + std::string(phase) + "\""
        + ",\"endgame_triggered\":" + json(state.endgame_triggered)
        + ",\"endgame_trigger_player\":" + json(state.endgame_trigger_player)
        + ",\"bank_tokens\":" + json(state.bank_tokens)
        + ",\"market_card_ids\":" + json(state.market_card_ids)
        + ",\"noble_ids\":" + json(head(state.noble_ids, visible_noble_count(state.player_count)));
    const TierCounts tiers = tier_counts(state, catalog);
    std::array<std::uint8_t, kTierCount> counts{};
    for (std::size_t tier = 0; tier < kTierCount; ++tier) {
        counts[tier] = static_cast<std::uint8_t>(tiers.deck[tier]);
    }
    out += ",\"deck_counts\":" + json(counts) + ",\"players\":[";
    for (std::size_t seat = 0; seat < state.player_count; ++seat) {
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
    out += "],\"terminal\":" + (terminal ? result_json(match_result(*terminal)) : std::string("null"));
    return out + '}';
}

std::string choice_json(
    const TurnChoice& choice,
    const std::span<const std::optional<std::uint16_t>> noble_ids) {
    const auto noble_at = [&](const std::uint8_t slot) -> std::optional<std::uint16_t> {
        return slot < noble_ids.size() ? noble_ids[slot] : std::nullopt;
    };
    std::string out = "{\"key\":\"" + choice_key(choice) + "\",\"phase\":\""
        + std::string(decision_phase_name(choice.phase)) + "\"";
    if (choice.phase == DecisionPhase::Cleanup) {
        return out + ",\"returned\":" + json(choice.returned)
            + ",\"noble_slot\":" + json(choice.noble_slot)
            + ",\"noble\":" + (choice.noble_slot ? json(noble_at(*choice.noble_slot)) : "null") + '}';
    }
    const CandidateDescriptor& primary = choice.primary;
    std::optional<std::size_t> market_slot;
    if (primary.source_slot) market_slot = market_index(*primary.source_tier, *primary.source_slot);
    std::vector<std::uint16_t> nobles;
    for (std::uint8_t slot = 0; slot < kMaxVisibleNobles; ++slot) {
        if (!contains(choice.eligible_nobles, slot)) continue;
        if (const std::optional<std::uint16_t> id = noble_at(slot)) nobles.push_back(*id);
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

std::string choices_json(const std::span<const TurnChoice> choices, const State& state) {
    std::string out = "[";
    for (const TurnChoice& choice : choices) {
        if (out.size() > 1) out += ',';
        out += choice_json(choice, state.noble_ids);
    }
    return out + ']';
}

std::string token_takes_json(const Decision& decision) {
    if (decision.phase != DecisionPhase::Main) return "[]";
    std::vector<detail::TokenCounts> takes;
    detail::token_takes(decision.state, takes);
    return json(takes);
}

std::string result_json(const MatchResult& result) {
    return "{\"reason\":\"" + std::string(match_termination_name(result.reason))
        + "\",\"winners\":" + json(seats_of(result.winners, result.player_count))
        + ",\"final_prestige\":" + json(head(result.final_prestige, result.player_count))
        + ",\"purchased_cards\":" + json(head(result.purchased_cards, result.player_count))
        + ",\"timed_out_seat\":" + json(result.timed_out_seat) + '}';
}

std::string catalog_json(const Catalog& catalog) {
    std::string out = "{\"cards\":[";
    for (const Card& card : catalog.cards()) {
        if (card.id) out += ',';
        out += "{\"id\":" + json(card.id) + ",\"tier\":" + json(card.tier) + ",\"bonus\":\""
            + std::string(kColorNames[static_cast<std::size_t>(card.bonus)]) + "\",\"prestige\":"
            + json(card.prestige) + ",\"cost\":" + json(card.cost) + '}';
    }
    out += "],\"nobles\":[";
    for (const Noble& noble : catalog.nobles()) {
        if (noble.id) out += ',';
        out += "{\"id\":" + json(noble.id) + ",\"prestige\":" + json(noble.prestige)
            + ",\"requirements\":" + json(noble.requirements) + '}';
    }
    return out + "]}";
}

std::string choice_text(
    const TurnChoice& choice,
    const Catalog& catalog,
    const std::span<const std::optional<std::uint16_t>> noble_ids) {
    if (choice.phase == DecisionPhase::Cleanup) {
        std::string out;
        if (std::any_of(choice.returned.begin(), choice.returned.end(), [](auto count) { return count; })) {
            out = "returned " + token_list(choice.returned);
        }
        if (choice.noble_slot && *choice.noble_slot < noble_ids.size() && noble_ids[*choice.noble_slot]) {
            if (!out.empty()) out += "; ";
            out += "claimed noble #" + std::to_string(*noble_ids[*choice.noble_slot]);
        }
        return out.empty() ? "ended the turn" : out;
    }
    const CandidateDescriptor& primary = choice.primary;
    constexpr std::size_t kGold = static_cast<std::size_t>(Color::Gold);
    const std::string gold = primary.token_delta[kGold] > 0 ? " +1 gold" : "";
    switch (primary.family) {
        case CandidateFamily::Take: {
            if (is_null_take(primary)) return "took no tokens (the null move at ten tokens)";
            const std::string returned = token_list(primary.token_delta, -1);
            return "took " + token_list(primary.token_delta)
                + (returned == "nothing" ? "" : " and returned " + returned);
        }
        case CandidateFamily::PassForced:
            return "passed (no legal action)";
        case CandidateFamily::ReserveBlind:
            return "reserved a tier-" + std::to_string(*primary.source_tier) + " card from the deck" + gold;
        case CandidateFamily::ReserveVisible:
        case CandidateFamily::BuyVisible:
        case CandidateFamily::BuyReserved:
            break;
    }
    const Card& card = catalog.card(*primary.target_card_id);
    const std::string label = "tier-" + std::to_string(card.tier) + ' '
        + std::string(kColorNames[static_cast<std::size_t>(card.bonus)]) + " card #"
        + std::to_string(card.id) + " (" + std::to_string(card.prestige) + " pt)";
    if (primary.family == CandidateFamily::ReserveVisible) return "reserved " + label + gold;
    return "bought " + label
        + (primary.family == CandidateFamily::BuyReserved ? " from reserve" : " from market")
        + " paying " + token_list(primary.payment);
}

}  // namespace splendor::play
