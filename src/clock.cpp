#include "splendor/clock.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace splendor {

namespace {

std::int64_t whole_ms(const double value) { return std::llround(value); }

}  // namespace

ValidationResult validate_time_control(const TimeControl& control) {
    if (control.kind != ClockKind::Turn && control.kind != ClockKind::Bank) {
        return ValidationResult{false, "time control kind must be turn or bank"};
    }
    if (control.allowance_ms.size() < 2) {
        return ValidationResult{false, "time control needs an allowance per seat"};
    }
    if (std::find(control.allowance_ms.begin(), control.allowance_ms.end(), 0U)
        != control.allowance_ms.end()) {
        return ValidationResult{false, "time control allowances must be positive"};
    }
    if (!control.forfeit_on_timeout.empty()
        && control.forfeit_on_timeout.size() != control.allowance_ms.size()) {
        return ValidationResult{false, "timeout policy needs one entry per seat"};
    }
    return ValidationResult{true, {}};
}

GameClock::GameClock(TimeControl control) : control_(std::move(control)) {
    if (const ValidationResult result = validate_time_control(control_); !result) {
        throw std::invalid_argument(result.message);
    }
    limits_.assign(control_.allowance_ms.begin(), control_.allowance_ms.end());
}

void GameClock::run(const std::uint8_t seat, const std::uint32_t game_ply, const double now_ms) {
    if (actor_ && game_ply == game_ply_) {
        if (seat != *actor_) throw std::logic_error("a game ply has one acting seat");
        if (!since_) since_ = now_ms;
        return;
    }
    stop(now_ms);
    record(seat, game_ply, 0);
    since_ = now_ms;
}

void GameClock::stop(const double now_ms) {
    if (!since_) return;
    used_ += now_ms - *since_;
    since_.reset();
}

void GameClock::record(
    const std::uint8_t seat, const std::uint32_t game_ply, const std::uint64_t used_ms) {
    if (since_) throw std::logic_error("a running clock cannot adopt a logged decision");
    if (seat >= limits_.size()) throw std::invalid_argument("clock seat is out of range");
    if (actor_ && game_ply == game_ply_) {
        if (seat != *actor_) throw std::logic_error("a game ply has one acting seat");
    } else {
        if (actor_ && control_.kind == ClockKind::Bank) limits_[*actor_] -= used_;
        actor_ = seat;
        game_ply_ = game_ply;
    }
    used_ = static_cast<double>(used_ms);
}

std::uint8_t GameClock::require_actor() const {
    if (!actor_) throw std::logic_error("the clock has not started a turn");
    return *actor_;
}

double GameClock::used(const double now_ms) const {
    return used_ + (since_ ? now_ms - *since_ : 0.0);
}

std::int64_t GameClock::used_ms(const double now_ms) const { return whole_ms(used(now_ms)); }

std::int64_t GameClock::limit_ms() const { return whole_ms(limits_[require_actor()]); }

bool GameClock::expired(const double now_ms) const {
    return actor_ && used_ms(now_ms) >= limit_ms();
}

bool GameClock::loses_on_time(const std::uint8_t seat) const {
    if (seat >= limits_.size()) throw std::invalid_argument("clock seat is out of range");
    return control_.kind == ClockKind::Bank
        || (!control_.forfeit_on_timeout.empty() && control_.forfeit_on_timeout[seat]);
}

std::int64_t GameClock::remaining_ms(const std::uint8_t seat, const double now_ms) const {
    if (seat >= limits_.size()) throw std::invalid_argument("clock seat is out of range");
    return whole_ms(limits_[seat] - (actor_ == seat ? used(now_ms) : 0.0));
}

std::uint32_t GameClock::own_turns() const {
    require_actor();
    return game_ply_ / static_cast<std::uint32_t>(limits_.size());
}

}  // namespace splendor
