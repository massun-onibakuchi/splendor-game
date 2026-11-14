#include "splendor/primary.hpp"

#include "splendor/detail/turn.hpp"

#include <array>
#include <charconv>
#include <cstring>

namespace splendor {
namespace {

// The source zone a family's key names.
std::string_view zone_name(const PrimaryFamily family) {
    switch (family) {
        case PrimaryFamily::ReserveVisible:
        case PrimaryFamily::BuyVisible: return "market";
        case PrimaryFamily::ReserveBlind: return "deck";
        case PrimaryFamily::BuyReserved: return "self_reserve";
        case PrimaryFamily::Take:
        case PrimaryFamily::PassForced: return "none";
    }
    return {};
}

// Formats one key on the stack, so a key costs one append. The capacity holds the
// longest key any primary's field values can spell.
class KeyWriter {
public:
    void text(const std::string_view value) {
        std::memcpy(end_, value.data(), value.size());
        end_ += value.size();
    }

    void number(const int value) {
        if (value >= 0 && value < 10) {
            *end_++ = static_cast<char>('0' + value);
            return;
        }
        end_ = std::to_chars(end_, buffer_.data() + buffer_.size(), value).ptr;
    }

    template <typename T, std::size_t N>
    void numbers(const std::array<T, N>& values) {
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index != 0) *end_++ = ',';
            number(static_cast<int>(values[index]));
        }
    }

    template <typename T>
    void field(const std::string_view label, const std::optional<T>& value) {
        if (!value) return;
        text(label);
        number(static_cast<int>(*value));
    }

    std::string_view view() const {
        return {buffer_.data(), static_cast<std::size_t>(end_ - buffer_.data())};
    }

private:
    std::array<char, 160> buffer_;
    char* end_ = buffer_.data();
};

}  // namespace

std::string_view primary_family_name(const PrimaryFamily family) {
    switch (family) {
        case PrimaryFamily::Take: return "take";
        case PrimaryFamily::ReserveVisible: return "reserve_visible";
        case PrimaryFamily::ReserveBlind: return "reserve_blind";
        case PrimaryFamily::BuyVisible: return "buy_visible";
        case PrimaryFamily::BuyReserved: return "buy_reserved";
        case PrimaryFamily::PassForced: return "pass_forced";
    }
    return {};
}

namespace detail {

void append_primary_key(std::string& output, const Primary& primary) {
    KeyWriter key;
    key.text(primary_family_name(primary.family));
    key.text("|d=");
    key.numbers(primary.token_delta);
    key.text("|p=");
    key.numbers(primary.payment);
    key.text("|z=");
    key.text(zone_name(primary.family));
    key.field("|t=", primary.source_tier);
    key.field("|s=", primary.source_slot);
    key.field("|o=", primary.reserve_owner);
    key.field("|r=", primary.reserve_slot);
    key.field("|c=", primary.target_card_id);
    output += key.view();
}

}  // namespace detail

}  // namespace splendor
