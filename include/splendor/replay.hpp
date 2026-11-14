#pragma once

#include "splendor/episode.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace splendor {

// The game facts a persisted trace step binds: who chose what at which ply, whether it was
// forced, and the public refill it caused. Display text, rendered choices, indexes, clocks
// and agent reports are not game identity.
struct RecordedStep {
    std::uint32_t game_ply{};
    std::uint8_t actor{};
    bool forced{};
    std::string action_key;
    std::optional<std::uint8_t> refill_slot;
    std::optional<std::uint16_t> refill_card;
};

// Verifies persisted game facts against generated native traces, incrementally.
class TraceReplay {
public:
    explicit TraceReplay(std::vector<RecordedStep> expected);
    void verify(std::span<const TurnTraceStep> actual);
    void finish() const;

private:
    std::vector<RecordedStep> expected_;
    std::size_t position_{};
};

}  // namespace splendor
