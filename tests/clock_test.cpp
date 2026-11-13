#include "splendor/clock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>

namespace {

splendor::TimeControl bank(const std::uint64_t milliseconds) {
    return {splendor::ClockKind::Bank, {milliseconds, milliseconds}};
}

}  // namespace

// A turn's MAIN and CLEANUP share one game ply and one running use; a pause freezes it;
// the next ply charges the whole turn to its seat's bank, never the other seat's.
TEST(GameClockTest, BankChargesEachEndedTurnToItsSeat) {
    splendor::GameClock clock(bank(60'000));
    clock.run(0, 0, 1'000.0);
    clock.stop(4'000.0);  // MAIN decided after 3 s
    clock.run(0, 0, 9'000.0);
    EXPECT_EQ(clock.used_ms(10'000.0), 4'000);
    EXPECT_EQ(clock.limit_ms(), 60'000);
    clock.stop(11'000.0);  // pause
    EXPECT_EQ(clock.remaining_ms(0, 50'000.0), 55'000);
    EXPECT_EQ(clock.remaining_ms(1, 50'000.0), 60'000);

    clock.run(1, 1, 12'000.0);
    EXPECT_EQ(clock.remaining_ms(0, 13'000.0), 55'000);
    EXPECT_EQ(clock.remaining_ms(1, 13'000.0), 59'000);
    clock.run(0, 2, 22'000.0);  // seat 1's turn ran on until seat 0's began
    EXPECT_EQ(clock.remaining_ms(1, 22'000.0), 50'000);
    EXPECT_EQ(clock.limit_ms(), 55'000);
    EXPECT_FALSE(clock.expired(76'999.0));
    EXPECT_TRUE(clock.expired(77'000.0));
}

// A turn allowance is fresh every turn; expiry is the turn's own overtime.
TEST(GameClockTest, TurnAllowanceIsFreshEveryTurn) {
    splendor::GameClock clock({splendor::ClockKind::Turn, {120'000, 10'000}});
    clock.run(1, 0, 0.0);
    EXPECT_TRUE(clock.expired(10'000.0));
    clock.run(0, 1, 15'000.0);
    clock.run(1, 2, 20'000.0);
    EXPECT_EQ(clock.limit_ms(), 10'000);
    EXPECT_EQ(clock.remaining_ms(1, 21'000.0), 9'000);
    EXPECT_EQ(clock.remaining_ms(0, 21'000.0), 120'000);
    EXPECT_FALSE(clock.expired(29'999.0));
}

// Replaying logged turn-cumulative uses rebuilds the banks the live clock charged.
TEST(GameClockTest, RecordedDecisionsRebuildTheBanks) {
    splendor::GameClock live(bank(60'000));
    live.run(0, 0, 0.0);
    live.stop(2'500.0);  // MAIN logged at 2.5 s
    live.run(0, 0, 3'000.0);
    live.stop(4'000.0);  // CLEANUP logged at 3.5 s
    live.run(1, 1, 4'000.0);
    live.stop(9'000.0);  // logged at 5 s
    live.run(0, 2, 9'000.0);
    live.stop(9'000.0);

    splendor::GameClock replayed(bank(60'000));
    replayed.record(0, 0, 2'500);
    replayed.record(0, 0, 3'500);
    replayed.record(1, 1, 5'000);
    replayed.run(0, 2, 0.0);
    replayed.stop(0.0);
    for (std::uint8_t seat = 0; seat < 2; ++seat) {
        EXPECT_EQ(replayed.remaining_ms(seat, 0.0), live.remaining_ms(seat, 9'000.0));
    }
    EXPECT_THROW(replayed.record(1, 2, 0), std::logic_error);
}

TEST(GameClockTest, RejectsInvalidTimeControlsAndTurns) {
    EXPECT_THROW(splendor::GameClock(bank(0)), std::invalid_argument);
    EXPECT_THROW(
        splendor::GameClock({splendor::ClockKind::Bank, {60'000}}), std::invalid_argument);
    splendor::GameClock clock(bank(60'000));
    EXPECT_THROW(clock.own_turns(), std::logic_error);
    clock.run(0, 0, 0.0);
    EXPECT_THROW(clock.run(1, 0, 1.0), std::logic_error);
    EXPECT_THROW(clock.record(0, 1, 0), std::logic_error);
}
