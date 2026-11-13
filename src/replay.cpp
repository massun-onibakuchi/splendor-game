#include "splendor/replay.hpp"

#include <stdexcept>
#include <utility>

namespace splendor {

TraceReplay::TraceReplay(std::vector<TurnTraceStep> expected) : expected_(std::move(expected)) {}

void TraceReplay::verify(const std::span<const TurnTraceStep> actual) {
    for (const TurnTraceStep& step : actual) {
        if (position_ >= expected_.size()) {
            throw std::invalid_argument("the log omits generated game steps");
        }
        const TurnTraceStep& expected = expected_[position_];
        if (step.game_ply != expected.game_ply || step.actor != expected.actor
            || step.forced != expected.forced || step.action_key != expected.action_key
            || step.refill_slot != expected.refill_slot || step.refill_card != expected.refill_card) {
            throw std::invalid_argument("the log does not replay at step " + std::to_string(position_ + 1));
        }
        ++position_;
    }
}

void TraceReplay::finish() const {
    if (position_ != expected_.size()) {
        throw std::invalid_argument("the log contains unplayed game steps");
    }
}

}  // namespace splendor
