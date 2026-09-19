#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

/// Internal to sa-spectral. Not in include/ because nothing outside this
/// library should be hashing anything with these; they are here rather than
/// inside SpectrogramTileStore.cpp only so that the test suite can check them
/// against published vectors, which is the difference between a digest that is
/// SHA-256 and one that merely says so.
namespace sa::spectral::detail {

// --- SHA-256 ---------------------------------------------------------------
//
// Hand-written because this project takes no dependency it does not need, and
// because the alternative for a cache key is a short hash whose collisions
// would be wrong pixels rather than a rebuild. FIPS 180-4 with nothing clever
// in it: the round constants and the initial state are the published ones.

inline constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

[[nodiscard]] constexpr std::uint32_t rotateRight(std::uint32_t value, int bits) noexcept {
    return (value >> bits) | (value << (32 - bits));
}

class Sha256 {
public:
    void update(const void* data, std::size_t bytes) noexcept {
        const auto* at = static_cast<const std::uint8_t*>(data);
        total_ += bytes;
        while (bytes > 0) {
            const std::size_t room = std::min(bytes, std::size_t{64} - fill_);
            std::memcpy(buffer_.data() + fill_, at, room);
            fill_ += room;
            at += room;
            bytes -= room;
            if (fill_ == 64) {
                compress(buffer_.data());
                fill_ = 0;
            }
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept {
        const std::uint64_t bits = total_ * 8;
        constexpr std::uint8_t kPad = 0x80;
        update(&kPad, 1);
        constexpr std::uint8_t kZero = 0;
        while (fill_ != 56) {
            update(&kZero, 1);
        }
        // Big-endian, as is everything in SHA-256. Written a byte at a time
        // rather than memcpy-ed, which is what makes the digest the same on a
        // little-endian machine as on any other.
        std::array<std::uint8_t, 8> length{};
        for (std::size_t i = 0; i < 8; ++i) {
            length[i] = static_cast<std::uint8_t>((bits >> (56 - 8 * i)) & 0xFFu);
        }
        update(length.data(), length.size());

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t word = 0; word < 8; ++word) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
                digest[word * 4 + byte] =
                    static_cast<std::uint8_t>((state_[word] >> (24 - 8 * byte)) & 0xFFu);
            }
        }
        return digest;
    }

private:
    void compress(const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t i = 0; i < 16; ++i) {
            schedule[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                          (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                          (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                          static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotateRight(schedule[i - 15], 7) ^
                                     rotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
            const std::uint32_t s1 = rotateRight(schedule[i - 2], 17) ^
                                     rotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
            schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
        }

        std::array<std::uint32_t, 8> working = state_;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotateRight(working[4], 6) ^ rotateRight(working[4], 11) ^
                                     rotateRight(working[4], 25);
            const std::uint32_t choose = (working[4] & working[5]) ^ (~working[4] & working[6]);
            const std::uint32_t temp1 =
                working[7] + s1 + choose + kSha256RoundConstants[i] + schedule[i];
            const std::uint32_t s0 = rotateRight(working[0], 2) ^ rotateRight(working[0], 13) ^
                                     rotateRight(working[0], 22);
            const std::uint32_t majority =
                (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            const std::uint32_t temp2 = s0 + majority;

            working[7] = working[6];
            working[6] = working[5];
            working[5] = working[4];
            working[4] = working[3] + temp1;
            working[3] = working[2];
            working[2] = working[1];
            working[1] = working[0];
            working[0] = temp1 + temp2;
        }
        for (std::size_t i = 0; i < 8; ++i) {
            state_[i] += working[i];
        }
    }

    std::array<std::uint32_t, 8> state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                           0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t fill_ = 0;
    std::uint64_t total_ = 0;
};

// --- CRC-32 ----------------------------------------------------------------
//
// The IEEE 802.3 polynomial, which is what any other tool that has to look at
// one of these files will also have. It is a check against accidents -- a
// truncated write, a flipped bit, a disk that returned the wrong sector -- and
// against nothing deliberate: anyone who can write into the cache directory can
// recompute it.

[[nodiscard]] constexpr std::array<std::uint32_t, 256> makeCrcTable() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::size_t index = 0; index < 256; ++index) {
        std::uint32_t value = static_cast<std::uint32_t>(index);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
        }
        table[index] = value;
    }
    return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrcTable = makeCrcTable();

[[nodiscard]] inline std::uint32_t crc32(const std::uint8_t* data, std::size_t bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < bytes; ++i) {
        const auto slot =
            static_cast<std::size_t>((crc ^ static_cast<std::uint32_t>(data[i])) & 0xFFu);
        crc = kCrcTable[slot] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace sa::spectral::detail
