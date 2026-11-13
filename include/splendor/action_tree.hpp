#pragma once

#include "splendor/turn.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace splendor {

// The action tree of one decision (`splendor-staged-action-v1`, docs/specs/staged-actions.md#action-tree).
// A MAIN decision is one engine decision whose legal choices are the leaves of
//
//   family (take, reserve, buy) -> intent (what is gained) -> parameter (what is given)
//
// A take's intent is its gain and its parameter the return; a reservation's intent is
// its card or deck tier, with no parameter; a purchase's intent is its card, from the
// market or the actor's reserve, and its parameter the payment. A forced pass is its own
// family. A CLEANUP decision is one family whose every choice is its own intent. The tree
// groups choices and never adds one: a staged policy of any granularity over it is a law
// over the same leaves.
enum class ActionFamily : std::uint8_t { Take, Reserve, Buy, Pass, Cleanup };
inline constexpr std::size_t kActionFamilyCount = 5;

std::string_view action_family_name(ActionFamily family);

// A choice's place in its decision's tree. `intent` is dense within the decision,
// numbered in order of first appearance in choice order; every member of an intent
// shares its family. `null` marks the take of the empty gain, legal exactly at ten held
// tokens while a take exists.
struct ActionNode {
    ActionFamily family{};
    std::uint16_t intent{};
    bool null{};
    bool operator==(const ActionNode&) const = default;
};

// The tree of `choices` (one decision's legal choices, in choice order), written over
// `nodes`. Depends only on the choices.
void action_tree(std::span<const TurnChoice> choices, std::vector<ActionNode>& nodes);
std::vector<ActionNode> action_tree(std::span<const TurnChoice> choices);

}  // namespace splendor
