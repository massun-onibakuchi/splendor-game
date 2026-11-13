#pragma once

#include "splendor/episode.hpp"
#include <span>
#include <vector>

namespace splendor {

// Verifies persisted game facts against generated native traces, incrementally. Display
// text, rendered choices, indexes, clocks and search reports are not game trace identity.
class TraceReplay {
public:
    explicit TraceReplay(std::vector<TurnTraceStep> expected);
    void verify(std::span<const TurnTraceStep> actual);
    void finish() const;

private:
    std::vector<TurnTraceStep> expected_;
    std::size_t position_{};
};

}  // namespace splendor
