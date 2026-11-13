#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace splendor::detail {

// FIPS 180-4 SHA-256 on 32-bit words. Bulk callers (training row order, environment
// draws, search draw keys) derive many seeds, where a byte-oriented implementation cost
// more than the rest of the derivation.
class Sha256 {
private:
    static constexpr std::array<std::uint32_t, 64> kRound{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
        0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
        0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
        0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

public:
    using Digest = std::array<std::uint32_t, 8>;
    static constexpr std::size_t kBlockSize = 64;

    static constexpr Digest kInitial{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

    // The digest of `message` given `state`, the state after its first `offset` bytes
    // (a multiple of the block size).
    static Digest finish(Digest state, const std::string_view message, std::size_t offset) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(message.data());
        for (; offset + kBlockSize <= message.size(); offset += kBlockSize) {
            compress(state, bytes + offset);
        }
        // The remaining bytes, the 0x80 terminator and the big-endian bit length fill one
        // final block, or two when fewer than nine bytes of the first are free.
        std::array<unsigned char, 2 * kBlockSize> tail{};
        const std::size_t remaining = message.size() - offset;
        if (remaining != 0) std::memcpy(tail.data(), bytes + offset, remaining);
        tail[remaining] = 0x80;
        const std::size_t length = remaining + 9 <= kBlockSize ? kBlockSize : 2 * kBlockSize;
        const std::uint64_t bits = static_cast<std::uint64_t>(message.size()) * 8U;
        for (std::size_t index = 0; index < 8; ++index) {
            tail[length - 1 - index] = static_cast<unsigned char>(bits >> (8U * index));
        }
        for (std::size_t block = 0; block < length; block += kBlockSize) {
            compress(state, tail.data() + block);
        }
        return state;
    }

    static void compress(Digest& state, const unsigned char* block) {
        std::array<std::uint32_t, 64> w;
        for (std::size_t index = 0; index < 16; ++index) {
            const unsigned char* word = block + 4 * index;
            w[index] = (std::uint32_t{word[0]} << 24U) | (std::uint32_t{word[1]} << 16U)
                | (std::uint32_t{word[2]} << 8U) | std::uint32_t{word[3]};
        }
        for (std::size_t index = 16; index < 64; ++index) {
            const std::uint32_t s0 = std::rotr(w[index - 15], 7) ^ std::rotr(w[index - 15], 18)
                ^ (w[index - 15] >> 3U);
            const std::uint32_t s1 = std::rotr(w[index - 2], 17) ^ std::rotr(w[index - 2], 19)
                ^ (w[index - 2] >> 10U);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (std::size_t index = 0; index < 64; ++index) {
            const std::uint32_t t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25))
                + ((e & f) ^ (~e & g)) + kRound[index] + w[index];
            const std::uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22))
                + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
};

}  // namespace splendor::detail
