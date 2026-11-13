#include "splendor/action_tree.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace splendor {
namespace {

constexpr std::size_t kGoldIndex = static_cast<std::size_t>(Color::Gold);

// The family of a MAIN choice's primary.
ActionFamily main_family(const CandidateFamily family) {
    switch (family) {
        case CandidateFamily::Take: return ActionFamily::Take;
        case CandidateFamily::ReserveVisible:
        case CandidateFamily::ReserveBlind: return ActionFamily::Reserve;
        case CandidateFamily::BuyVisible:
        case CandidateFamily::BuyReserved: return ActionFamily::Buy;
        case CandidateFamily::PassForced: return ActionFamily::Pass;
    }
    throw std::logic_error("unknown candidate family");
}

// A take's gain: the positive part of its net delta, which the normal form makes the gain
// itself (its return lies in other colours). Gold is never gained, so each gem colour's
// count of 0, 1 or 2 is one base-3 digit.
std::uint64_t gain_code(const CandidateDescriptor& primary) {
    std::uint64_t code = 0;
    for (std::size_t color = kGoldIndex; color-- > 0;) {
        code = 3 * code + static_cast<std::uint64_t>(std::max<int>(0, primary.token_delta[color]));
    }
    return code;
}

// The identity of a choice's intent among those that can hold several parameters, or
// none when the choice is its own intent: a take by its gain, a purchase by its source
// family and card (a card sits in one market slot or one reserve slot).
std::optional<std::uint64_t> shared_intent(const TurnChoice& choice) {
    if (choice.phase != DecisionPhase::Main) return std::nullopt;
    const CandidateDescriptor& primary = choice.primary;
    switch (primary.family) {
        case CandidateFamily::Take: return gain_code(primary);
        case CandidateFamily::BuyVisible: return (std::uint64_t{1} << 32) | *primary.target_card_id;
        case CandidateFamily::BuyReserved: return (std::uint64_t{2} << 32) | *primary.target_card_id;
        case CandidateFamily::ReserveVisible:
        case CandidateFamily::ReserveBlind:
        case CandidateFamily::PassForced: return std::nullopt;
    }
    throw std::logic_error("unknown candidate family");
}

bool all_zero(const std::array<std::int8_t, kTokenColorCount>& delta) {
    return std::all_of(delta.begin(), delta.end(), [](const auto value) { return value == 0; });
}

}  // namespace

std::string_view action_family_name(const ActionFamily family) {
    switch (family) {
        case ActionFamily::Take: return "take";
        case ActionFamily::Reserve: return "reserve";
        case ActionFamily::Buy: return "buy";
        case ActionFamily::Pass: return "pass";
        case ActionFamily::Cleanup: return "cleanup";
    }
    return {};
}

void action_tree(const std::span<const TurnChoice> choices, std::vector<ActionNode>& nodes) {
    if (choices.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::logic_error("a decision has more choices than intent indices");
    }
    nodes.resize(choices.size());
    // At most 29 gains and 15 purchases share intents, so a linear scan is the lookup.
    thread_local std::vector<std::pair<std::uint64_t, std::uint16_t>> shared;
    shared.clear();
    std::uint16_t next = 0;
    for (std::size_t index = 0; index < choices.size(); ++index) {
        const TurnChoice& choice = choices[index];
        ActionNode& node = nodes[index];
        if (choice.phase == DecisionPhase::Cleanup) {
            node = ActionNode{ActionFamily::Cleanup, next++, false};
            continue;
        }
        node.family = main_family(choice.primary.family);
        node.null = node.family == ActionFamily::Take && all_zero(choice.primary.token_delta);
        const std::optional<std::uint64_t> key = shared_intent(choice);
        if (!key) {
            node.intent = next++;
            continue;
        }
        const auto known = std::find_if(shared.begin(), shared.end(), [&](const auto& entry) {
            return entry.first == *key;
        });
        if (known != shared.end()) {
            node.intent = known->second;
        } else {
            shared.emplace_back(*key, next);
            node.intent = next++;
        }
    }
}

std::vector<ActionNode> action_tree(const std::span<const TurnChoice> choices) {
    std::vector<ActionNode> nodes;
    action_tree(choices, nodes);
    return nodes;
}

}  // namespace splendor
