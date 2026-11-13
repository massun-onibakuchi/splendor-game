#include "splendor/candidate.hpp"

#include "splendor/detail/candidate.hpp"
#include <array>
#include <charconv>
#include <cstring>

namespace splendor {
namespace {

std::string_view zone_name(const SourceZone zone) {
    switch (zone) {
        case SourceZone::Market: return "market";
        case SourceZone::Deck: return "deck";
        case SourceZone::SelfReserve: return "self_reserve";
        case SourceZone::OpponentReserve: return "opponent_reserve";
        case SourceZone::None: return "none";
    }
    return {};
}

// Formats one key on the stack, so a key costs one append. The capacity holds the
// longest key any descriptor field values can spell.
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
    std::array<char, 192> buffer_;
    char* end_ = buffer_.data();
};

}  // namespace

std::string_view candidate_family_name(const CandidateFamily family) {
    switch (family) {
        case CandidateFamily::Take: return "take";
        case CandidateFamily::ReserveVisible: return "reserve_visible";
        case CandidateFamily::ReserveBlind: return "reserve_blind";
        case CandidateFamily::BuyVisible: return "buy_visible";
        case CandidateFamily::BuyReserved: return "buy_reserved";
        case CandidateFamily::PassForced: return "pass_forced";
    }
    return {};
}

std::string encode_action_key(const CandidateDescriptor& descriptor) {
    std::string output;
    detail::append_action_key(output, descriptor);
    return output;
}

namespace detail {

void append_action_key(std::string& output, const CandidateDescriptor& descriptor) {
    KeyWriter key;
    key.text(candidate_family_name(descriptor.family));
    key.text("|d=");
    key.numbers(descriptor.token_delta);
    key.text("|p=");
    key.numbers(descriptor.payment);
    key.text("|z=");
    key.text(zone_name(descriptor.source_zone));
    key.field("|t=", descriptor.source_tier);
    key.field("|s=", descriptor.source_slot);
    key.field("|o=", descriptor.reserve_owner);
    key.field("|r=", descriptor.reserve_slot);
    key.field("|c=", descriptor.target_card_id);
    key.field("|n=", descriptor.chosen_noble_slot);
    output += key.view();
}

}  // namespace detail

}  // namespace splendor
