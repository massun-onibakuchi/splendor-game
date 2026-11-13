#include "splendor/seed.hpp"

#include "splendor/contracts.hpp"
#include "sha256_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace splendor {
namespace {

using detail::Sha256;

void append_json_string(std::string& out, const std::string_view value) {
    out.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    throw std::invalid_argument("seed domain contains a control character");
                }
                out.push_back(static_cast<char>(ch));
        }
    }
    out.push_back('"');
}

// A decimal string needs no escaping.
void append_quoted_decimal(std::string& out, const std::uint64_t value) {
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 1> digits;
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    out.push_back('"');
    out.append(digits.data(), result.ptr);
    out.push_back('"');
}

// The fixed opening of every preimage: the contract (the configured domain-separation
// tag, which needs no escaping), then the coordinate list.
constexpr std::string_view kPreimageOpen = "{\"contract\":\"";
constexpr std::string_view kPreimageClose = "\",\"coordinates\":[";
constexpr auto kPreimagePrefixBytes = [] {
    std::array<char, kPreimageOpen.size() + kSeedContract.size() + kPreimageClose.size()> bytes{};
    auto out = std::copy(kPreimageOpen.begin(), kPreimageOpen.end(), bytes.begin());
    out = std::copy(kSeedContract.begin(), kSeedContract.end(), out);
    std::copy(kPreimageClose.begin(), kPreimageClose.end(), out);
    return bytes;
}();
constexpr std::string_view kPreimagePrefix(kPreimagePrefixBytes.data(), kPreimagePrefixBytes.size());

// The RFC 8785 bytes of the seed-derivation object, written over `out`.
void write_seed_preimage(
    std::string& out,
    const std::uint64_t root,
    const std::string_view domain,
    const std::span<const std::uint64_t> coordinates) {
    out.assign(kPreimagePrefix);
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        if (index != 0) out.push_back(',');
        append_quoted_decimal(out, coordinates[index]);
    }
    out += "],\"domain\":";
    append_json_string(out, domain);
    out += ",\"root\":";
    append_quoted_decimal(out, root);
    out.push_back('}');
}

// The shortest preimage -- no coordinates, a one-byte domain, root 0 -- still fills the
// first block, and the bytes completing it fit one word.
static_assert(
    kPreimagePrefix.size() + std::string_view("],\"domain\":\"x\",\"root\":\"0\"}").size()
    >= Sha256::kBlockSize);
static_assert(kPreimagePrefix.size() < Sha256::kBlockSize
              && Sha256::kBlockSize - kPreimagePrefix.size() <= sizeof(std::uint64_t));

// The SHA-256 state after a preimage's first block, keyed by the bytes that complete the
// block. Bulk derivations repeat their leading coordinate (an epoch, a ply) or carry none,
// so most compress one block fewer. One cache per thread; an entry holds its whole key, so
// a collision only recomputes.
class FirstBlockStates {
public:
    const Sha256::Digest& of(const std::string_view preimage) {
        std::uint64_t key = 0;
        std::memcpy(&key, preimage.data() + kPreimagePrefix.size(), kTailSize);
        Entry& entry = entries_[(key * 0x9e3779b97f4a7c15ULL) >> (64 - kIndexBits)];
        if (!entry.filled || entry.key != key) {
            entry.key = key;
            entry.filled = true;
            entry.state = Sha256::kInitial;
            Sha256::compress(entry.state, reinterpret_cast<const unsigned char*>(preimage.data()));
        }
        return entry.state;
    }

private:
    static constexpr std::size_t kTailSize = Sha256::kBlockSize - kPreimagePrefix.size();
    static constexpr unsigned kIndexBits = 8;
    struct Entry {
        std::uint64_t key{};
        bool filled{};
        Sha256::Digest state{};
    };
    std::array<Entry, std::size_t{1} << kIndexBits> entries_{};
};

}  // namespace

std::uint64_t derive64(
    const std::uint64_t root,
    const std::string_view domain,
    const std::span<const std::uint64_t> coordinates) {
    if (domain.empty()) throw std::invalid_argument("seed domain must be nonempty");
    // Reused per-thread storage keeps a derivation allocation-free and skips the first
    // block whose state is already known.
    thread_local std::string preimage;
    thread_local FirstBlockStates first_blocks;
    write_seed_preimage(preimage, root, domain, coordinates);
    const Sha256::Digest digest =
        Sha256::finish(first_blocks.of(preimage), preimage, Sha256::kBlockSize);
    return (std::uint64_t{digest[0]} << 32U) | digest[1];
}

double u01(const std::uint64_t value) noexcept {
    const std::uint64_t r = value >> 12U;
    return std::ldexp(static_cast<double>(r) + 0.5, -52);
}

std::uint64_t random_counter(
    const std::uint64_t event_seed,
    const std::uint64_t draw_ordinal) {
    const std::array coordinates{draw_ordinal};
    return derive64(event_seed, "random", coordinates);
}

}  // namespace splendor
