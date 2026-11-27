#pragma once

#include "splendor/contracts.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>

namespace splendor {

// Membership in the catalog's dense ID space. Enumeration is always ascending,
// including across the word boundary: chance draws index that order.
class CardSet {
    static_assert(kCardCount > 64 && kCardCount <= 128);
    using Words = std::array<std::uint64_t, 2>;
    Words bits_{};

public:
    bool operator==(const CardSet&) const = default;
    const auto& words() const noexcept { return bits_; }
    std::size_t size() const noexcept { return std::popcount(bits_[0]) + std::popcount(bits_[1]); }
    bool empty() const noexcept { return (bits_[0] | bits_[1]) == 0; }
    // The card at an ascending zero-based draw rank (rank < size()).
    std::uint16_t operator[](std::size_t rank) const noexcept {
        const auto low = static_cast<std::size_t>(std::popcount(bits_[0]));
        const bool high = rank >= low;
        auto word = bits_[high];
        if (high) rank -= low;
        while (rank--) word &= word - 1;
        return static_cast<std::uint16_t>((high ? 64 : 0) + std::countr_zero(word));
    }
    std::uint16_t front() const noexcept { return *begin(); }
    bool contains(std::uint16_t id) const noexcept {
        return id < kCardCount && ((bits_[id / 64] >> (id % 64)) & 1U);
    }
    void insert(std::uint16_t id) {
        if (id >= kCardCount) throw std::out_of_range("unknown card id");
        bits_[id / 64] |= std::uint64_t{1} << (id % 64);
    }
    void erase(std::uint16_t id) {
        if (id >= kCardCount) throw std::out_of_range("unknown card id");
        bits_[id / 64] &= ~(std::uint64_t{1} << (id % 64));
    }
    CardSet operator&(const CardSet& other) const noexcept {
        CardSet result;
        result.bits_ = {bits_[0] & other.bits_[0], bits_[1] & other.bits_[1]};
        return result;
    }

    class iterator {
        Words remaining_{};
    public:
        using value_type = std::uint16_t;
        using difference_type = std::ptrdiff_t;
        using reference = value_type;
        using pointer = void;
        using iterator_category = std::forward_iterator_tag;

        iterator() = default;
        explicit iterator(Words remaining) : remaining_(remaining) {}
        bool operator==(const iterator&) const = default;
        value_type operator*() const noexcept {
            return remaining_[0] ? std::countr_zero(remaining_[0]) : 64 + std::countr_zero(remaining_[1]);
        }
        iterator& operator++() noexcept {
            auto& word = remaining_[remaining_[0] == 0];
            word &= word - 1;
            return *this;
        }
        iterator operator++(int) noexcept { auto old = *this; ++*this; return old; }
    };
    iterator begin() const noexcept { return iterator(bits_); }
    iterator end() const noexcept { return {}; }
};

}  // namespace splendor
