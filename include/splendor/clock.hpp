#pragma once

#include "splendor/state.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace splendor {

// How a match's clock allots time (docs/specs/game-engine.md#match-clock-and-result). `Turn` gives every turn
// of a seat the same fresh allowance; `Bank` gives each seat one bank for the whole game
// with no increment, as Board Game Arena tournaments do. `allowance_ms` holds one positive
// entry per seat: its turn allowance, or its initial bank.
enum class ClockKind : std::uint8_t { Turn, Bank };

struct TimeControl {
    ClockKind kind{};
    std::vector<std::uint64_t> allowance_ms;
    // Per-turn overtime forfeits by seat; empty means flag only. A bank always forfeits.
    std::vector<bool> forfeit_on_timeout{};
    bool operator==(const TimeControl&) const = default;
};

ValidationResult validate_time_control(const TimeControl& control);

// One match's clock, shared by every surface that plays on time: the human-versus-model
// boards and time-controlled arenas. A turn is one seat's decisions at one game ply, its
// MAIN and CLEANUP alike; it runs in segments between `run` and `stop`, so a pause
// freezes it. Starting another ply ends the current turn and, under `Bank`, charges its
// use to its seat's bank. Times are the caller's monotonic milliseconds; durations are
// rounded to whole milliseconds.
class GameClock {
public:
    explicit GameClock(TimeControl control);

    // Runs `seat`'s turn at `game_ply` from `now_ms`: the current turn resumes, another ply
    // ends it (its running segment included) and starts a fresh one. A running segment of
    // the same turn continues.
    void run(std::uint8_t seat, std::uint32_t game_ply, double now_ms);
    // Ends the running segment, if any.
    void stop(double now_ms);
    // Adopts a logged decision while stopped: as `run` would, then the turn has used
    // `used_ms`, the turn-cumulative time the log recorded at that decision.
    void record(std::uint8_t seat, std::uint32_t game_ply, std::uint64_t used_ms);

    const TimeControl& control() const noexcept { return control_; }
    bool running() const noexcept { return since_.has_value(); }
    // The current turn's seat; none before the first turn.
    std::optional<std::uint8_t> actor() const noexcept { return actor_; }
    // The current turn's use.
    std::int64_t used_ms(double now_ms) const;
    // The current turn's limit: the seat's allowance, or its bank when the turn began.
    std::int64_t limit_ms() const;
    // Whether the current turn has used its whole limit.
    bool expired(double now_ms) const;
    bool loses_on_time(std::uint8_t seat) const;
    // What `seat` has left now: the rest of the current turn's limit for the actor, else
    // its next turn's.
    std::int64_t remaining_ms(std::uint8_t seat, double now_ms) const;
    // The turns the current turn's seat completed before it, counted from the game's
    // setup: what its time allocation paces the game by.
    std::uint32_t own_turns() const;

private:
    std::uint8_t require_actor() const;
    double used(double now_ms) const;

    TimeControl control_;
    // Each seat's limit at the start of its next turn: its allowance under `Turn`; its
    // bank, net of every ended turn, under `Bank`.
    std::vector<double> limits_;
    std::optional<std::uint8_t> actor_;
    std::uint32_t game_ply_{};
    // The current turn's closed segments, and the running segment's start.
    double used_{};
    std::optional<double> since_;
};

}  // namespace splendor
