#include "splendor/choice_columns.hpp"

#include "splendor/detail/turn.hpp"

#include <algorithm>

namespace splendor {
namespace {

template <typename T>
void reset(std::vector<T>& column, const std::size_t size, const T value) {
    column.assign(size, value);
}

std::uint8_t field(const std::optional<std::uint8_t>& value) {
    return value ? *value : kNoChoiceField;
}

}  // namespace

void write_choice_columns(const std::span<const TurnChoice> choices, ChoiceColumns& out) {
    const std::size_t rows = choices.size();
    out.rows = rows;
    thread_local std::vector<ActionNode> nodes;
    action_tree(choices, nodes);
    reset<std::uint8_t>(out.phase, rows, 0);
    reset<std::uint8_t>(out.family, rows, 0);
    reset<std::uint16_t>(out.intent, rows, 0);
    reset<std::uint8_t>(out.null_take, rows, 0);
    reset<std::uint8_t>(out.primary, rows, kNoChoiceField);
    reset<std::int8_t>(out.token_delta, rows * kTokenColorCount, 0);
    reset<std::uint8_t>(out.payment, rows * kTokenColorCount, 0);
    reset<std::uint8_t>(out.tier, rows, kNoChoiceField);
    reset<std::uint8_t>(out.market_slot, rows, kNoChoiceField);
    reset<std::uint8_t>(out.reserve_slot, rows, kNoChoiceField);
    reset<std::uint16_t>(out.card, rows, kNoChoiceCard);
    reset<std::uint8_t>(out.eligible_nobles, rows, 0);
    reset<std::uint8_t>(out.returned, rows * kTokenColorCount, 0);
    reset<std::uint8_t>(out.noble_slot, rows, kNoChoiceField);
    for (std::size_t row = 0; row < rows; ++row) {
        const TurnChoice& choice = choices[row];
        out.phase[row] = static_cast<std::uint8_t>(choice.phase);
        out.family[row] = static_cast<std::uint8_t>(nodes[row].family);
        out.intent[row] = nodes[row].intent;
        out.null_take[row] = nodes[row].null ? 1 : 0;
        const std::size_t wide = row * kTokenColorCount;
        if (choice.phase == DecisionPhase::Cleanup) {
            std::copy(choice.returned.begin(), choice.returned.end(), out.returned.begin() + wide);
            out.noble_slot[row] = field(choice.noble_slot);
            continue;
        }
        const Primary& primary = choice.primary;
        out.primary[row] = static_cast<std::uint8_t>(primary.family);
        std::copy(primary.token_delta.begin(), primary.token_delta.end(), out.token_delta.begin() + wide);
        std::copy(primary.payment.begin(), primary.payment.end(), out.payment.begin() + wide);
        out.tier[row] = field(primary.source_tier);
        if (primary.source_tier && primary.source_slot) {
            out.market_slot[row] =
                static_cast<std::uint8_t>(detail::market_index(*primary.source_tier, *primary.source_slot));
        }
        out.reserve_slot[row] = field(primary.reserve_slot);
        out.card[row] = primary.target_card_id ? *primary.target_card_id : kNoChoiceCard;
        out.eligible_nobles[row] = choice.eligible_nobles;
    }
}

}  // namespace splendor
