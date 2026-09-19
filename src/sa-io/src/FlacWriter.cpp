#include <sa/io/FlacWriter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// FLAC encoding, written against the format rather than against a library.
///
/// The bitstream this produces is ordinary native FLAC: a `fLaC` signature, a
/// STREAMINFO block, an optional VORBIS_COMMENT, then frames of subframes.
/// Every frame is a fixed-size block of samples; each channel becomes one
/// subframe holding a predictor and Rice-coded residuals; the predictors used
/// here are the constant, verbatim and four fixed polynomial ones, which are
/// exact integer operations and therefore lossless by construction.
///
/// The header comment on FlacWriter says why this is ours. What follows is the
/// format detail, which is laid out in the order the bytes come out.
namespace sa::io {

namespace {

/// FLAC's channel field is four bits wide, and the eight independent-channel
/// codes are the whole of it.
constexpr int kMaxFlacChannels = 8;

/// The subset caps the Rice partition order at eight, and nothing is gained
/// past it: a partition of sixteen samples already has its own parameter.
constexpr int kMaxPartitionOrder = 8;

// ---------------------------------------------------------------------------
// MD5, for the STREAMINFO signature
// ---------------------------------------------------------------------------

/// MD5 over the unencoded audio, which is what STREAMINFO's signature covers.
///
/// Not used as a security primitive -- MD5 has not been one for twenty years --
/// but because the format specifies it. It is what lets `flac -t` say that a
/// file decodes to the samples that were encoded, which is exactly the property
/// this writer claims, so leaving the field zero would be declining to be
/// checked.
class Md5 {
public:
    void update(const std::byte* data, std::size_t length) noexcept {
        length_ += length;
        for (std::size_t i = 0; i < length; ++i) {
            buffer_[held_++] = data[i];
            if (held_ == 64) {
                transform(buffer_.data());
                held_ = 0;
            }
        }
    }

    [[nodiscard]] std::array<std::byte, 16> finish() noexcept {
        const std::uint64_t bits = length_ * 8u;

        constexpr std::byte kOne{0x80};
        update(&kOne, 1);
        constexpr std::byte kZero{0};
        while (held_ != 56) {
            update(&kZero, 1);
        }
        std::array<std::byte, 8> tail{};
        for (std::size_t i = 0; i < 8; ++i) {
            tail[i] = static_cast<std::byte>((bits >> (8u * i)) & 0xFFu);
        }
        update(tail.data(), tail.size());

        std::array<std::byte, 16> digest{};
        for (std::size_t word = 0; word < 4; ++word) {
            for (std::size_t i = 0; i < 4; ++i) {
                digest[word * 4 + i] = static_cast<std::byte>((state_[word] >> (8u * i)) & 0xFFu);
            }
        }
        return digest;
    }

private:
    static std::uint32_t rotateLeft(std::uint32_t value, int count) noexcept {
        return (value << count) | (value >> (32 - count));
    }

    void transform(const std::byte* block) noexcept {
        static constexpr std::array<std::uint32_t, 64> kSine{
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
            0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
            0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
            0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
            0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
            0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
            0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
            0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
            0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
            0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
            0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
        static constexpr std::array<int, 64> kShift{
            7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
            14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
            4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};

        std::array<std::uint32_t, 16> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] = 0;
            for (std::size_t b = 0; b < 4; ++b) {
                words[i] |=
                    static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[i * 4 + b]))
                    << (8u * b);
            }
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];

        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t mixed = 0;
            std::size_t index = 0;
            if (i < 16) {
                mixed = (b & c) | (~b & d);
                index = i;
            } else if (i < 32) {
                mixed = (d & b) | (~d & c);
                index = (5 * i + 1) % 16;
            } else if (i < 48) {
                mixed = b ^ c ^ d;
                index = (3 * i + 5) % 16;
            } else {
                mixed = c ^ (b | ~d);
                index = (7 * i) % 16;
            }
            const std::uint32_t temporary = d;
            d = c;
            c = b;
            b = b + rotateLeft(a + mixed + kSine[i] + words[index], kShift[i]);
            a = temporary;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
    }

    std::array<std::uint32_t, 4> state_{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::array<std::byte, 64> buffer_{};
    std::size_t held_ = 0;
    std::uint64_t length_ = 0;
};

// ---------------------------------------------------------------------------
// The two CRCs a FLAC frame carries
// ---------------------------------------------------------------------------

/// CRC-8 over the frame header, CRC-16 over the whole frame. Both are the
/// plain MSB-first forms with no reflection and no final xor, which is what the
/// format specifies and what every decoder checks against.
constexpr std::array<std::uint8_t, 256> makeCrc8Table() noexcept {
    std::array<std::uint8_t, 256> table{};
    for (std::size_t i = 0; i < 256; ++i) {
        auto value = static_cast<std::uint8_t>(i);
        for (int bit = 0; bit < 8; ++bit) {
            const bool high = (value & 0x80u) != 0;
            value = static_cast<std::uint8_t>(value << 1);
            if (high) {
                value = static_cast<std::uint8_t>(value ^ 0x07u);
            }
        }
        table[i] = value;
    }
    return table;
}

constexpr std::array<std::uint16_t, 256> makeCrc16Table() noexcept {
    std::array<std::uint16_t, 256> table{};
    for (std::size_t i = 0; i < 256; ++i) {
        auto value = static_cast<std::uint16_t>(i << 8);
        for (int bit = 0; bit < 8; ++bit) {
            const bool high = (value & 0x8000u) != 0;
            value = static_cast<std::uint16_t>(value << 1);
            if (high) {
                value = static_cast<std::uint16_t>(value ^ 0x8005u);
            }
        }
        table[i] = value;
    }
    return table;
}

constexpr auto kCrc8Table = makeCrc8Table();
constexpr auto kCrc16Table = makeCrc16Table();

std::uint8_t crc8(const std::byte* data, std::size_t length) noexcept {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < length; ++i) {
        crc = kCrc8Table[static_cast<std::size_t>(crc ^ std::to_integer<std::uint8_t>(data[i]))];
    }
    return crc;
}

std::uint16_t crc16(const std::byte* data, std::size_t length) noexcept {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const auto index =
            static_cast<std::size_t>(((crc >> 8) ^ std::to_integer<std::uint8_t>(data[i])) & 0xFFu);
        crc = static_cast<std::uint16_t>(static_cast<std::uint16_t>(crc << 8) ^ kCrc16Table[index]);
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Bit-level output
// ---------------------------------------------------------------------------

constexpr std::uint64_t maskOf(int bits) noexcept {
    return bits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1u);
}

/// Most-significant-bit-first bit writer over a byte vector.
///
/// FLAC is a bit-packed format: a subframe header is eight bits that do not
/// start on a byte, and a Rice code is a run of zeroes of whatever length the
/// sample needs. Bytes only reappear at a frame boundary, where the format
/// requires padding back to one.
class BitWriter {
public:
    explicit BitWriter(std::vector<std::byte>& out) noexcept : out_(&out) {}

    void writeBits(std::uint32_t value, int bits) {
        if (bits <= 0) {
            return;
        }
        accumulator_ = (accumulator_ << bits) | (static_cast<std::uint64_t>(value) & maskOf(bits));
        held_ += bits;
        while (held_ >= 8) {
            held_ -= 8;
            out_->push_back(static_cast<std::byte>((accumulator_ >> held_) & 0xFFu));
        }
    }

    /// Two's complement in `bits` bits. The mask in writeBits drops everything
    /// above them, which is exactly the truncation the format wants.
    void writeSigned(std::int32_t value, int bits) {
        writeBits(static_cast<std::uint32_t>(value), bits);
    }

    /// `zeroes` zero bits then a one, which is how FLAC spells the quotient of
    /// a Rice code.
    void writeUnary(std::uint32_t zeroes) {
        while (zeroes >= 32) {
            writeBits(0, 32);
            zeroes -= 32;
        }
        writeBits(1u, static_cast<int>(zeroes) + 1);
    }

    void alignToByte() {
        if (held_ != 0) {
            writeBits(0u, 8 - held_);
        }
    }

private:
    std::vector<std::byte>* out_;
    std::uint64_t accumulator_ = 0;
    int held_ = 0;
};

void appendUtf8Number(BitWriter& writer, std::uint64_t value) {
    // The frame number goes out in the extension of UTF-8 that FLAC uses, which
    // reaches 36 bits in seven bytes. A lead byte of n set bits and a zero
    // carries 7-n bits of payload and promises n-1 continuation bytes of six
    // more, so a length of L spells 5L+1 bits -- except the seven-byte form,
    // whose lead byte is all payload-free and carries the full 36 in its
    // continuations.
    if (value < 0x80ull) {
        writer.writeBits(static_cast<std::uint32_t>(value), 8);
        return;
    }

    int length = 2;
    while (length < 7 && value >= (std::uint64_t{1} << (5 * length + 1))) {
        ++length;
    }

    const int payloadBits = 6 * (length - 1);
    const auto lead = static_cast<std::uint32_t>(((0xFFu << (8 - length)) & 0xFFu) |
                                                 static_cast<std::uint32_t>(value >> payloadBits));
    writer.writeBits(lead, 8);
    for (int i = length - 1; i > 0; --i) {
        writer.writeBits(0x80u | (static_cast<std::uint32_t>(value >> (6 * (i - 1))) & 0x3Fu), 8);
    }
}

// ---------------------------------------------------------------------------
// Choosing how a subframe is coded
// ---------------------------------------------------------------------------

std::uint32_t zigzag(std::int32_t value) noexcept {
    // Interleave positive and negative so that small magnitudes of either sign
    // become small unsigned numbers, which is what Rice coding rewards.
    return (static_cast<std::uint32_t>(value) << 1) ^ static_cast<std::uint32_t>(value >> 31);
}

/// The Rice parameter for a partition, and what it would cost.
///
/// The parameter wanted is the one where 2^k is about the mean of the mapped
/// residuals, so that most quotients are zero or one. The cost is then one stop
/// bit and k remainder bits per sample plus the quotients, and sum >> k bounds
/// the quotients from above -- the sum of the shifts is never more than the
/// shift of the sum. Choosing k this way also bounds the longest unary run by
/// the size of the partition, which is what keeps a single loud sample in a
/// quiet partition from costing a megabyte.
struct RiceChoice {
    int parameter = 0;
    std::uint64_t bits = 0;
};

RiceChoice riceFor(std::uint64_t sum, std::uint64_t count) noexcept {
    if (count == 0) {
        return {};
    }
    int parameter = 0;
    while (parameter < 30 && (count << parameter) < sum) {
        ++parameter;
    }
    return {parameter, count * static_cast<std::uint64_t>(parameter + 1) + (sum >> parameter)};
}

struct ResidualPlan {
    int partitionOrder = 0;
    /// Five-bit Rice parameters rather than four. Needed above parameter 14,
    /// which loud 24-bit material reaches routinely.
    bool wideParameters = false;
    std::uint64_t bits = 0;
};

/// Samples in partition `index` of `partitions`, given a predictor that ate the
/// first `order` of the block.
struct PartitionSpan {
    SampleCount start = 0;
    SampleCount end = 0;
};

PartitionSpan partitionSpan(SampleCount count, int partitionOrder, int index, int order) noexcept {
    const SampleCount span = count >> partitionOrder;
    const SampleCount start = static_cast<SampleCount>(index) * span + (index == 0 ? order : 0);
    return {start, static_cast<SampleCount>(index + 1) * span};
}

int largestPartitionOrder(SampleCount count, int order) noexcept {
    for (int candidate = kMaxPartitionOrder; candidate > 0; --candidate) {
        const SampleCount partitions = SampleCount{1} << candidate;
        if (count % partitions == 0 && (count >> candidate) > order) {
            return candidate;
        }
    }
    return 0;
}

ResidualPlan planResidual(const std::int32_t* residual, SampleCount count, int order,
                          std::vector<std::uint64_t>& sums) {
    const int finestOrder = largestPartitionOrder(count, order);

    sums.assign(static_cast<std::size_t>(SampleCount{1} << finestOrder), std::uint64_t{0});
    for (int index = 0; index < (1 << finestOrder); ++index) {
        const PartitionSpan span = partitionSpan(count, finestOrder, index, order);
        std::uint64_t sum = 0;
        for (SampleCount i = span.start; i < span.end; ++i) {
            sum += zigzag(residual[i]);
        }
        sums[static_cast<std::size_t>(index)] = sum;
    }

    ResidualPlan best;
    best.bits = ~std::uint64_t{0};

    for (int candidate = finestOrder; candidate >= 0; --candidate) {
        const int partitions = 1 << candidate;
        std::uint64_t bits = 2 + 4;
        bool wide = false;
        for (int index = 0; index < partitions; ++index) {
            const PartitionSpan span = partitionSpan(count, candidate, index, order);
            const auto samples = static_cast<std::uint64_t>(span.end - span.start);
            const RiceChoice choice = riceFor(sums[static_cast<std::size_t>(index)], samples);
            wide = wide || choice.parameter > 14;
            bits += choice.bits;
        }
        bits += static_cast<std::uint64_t>(partitions) * (wide ? 5u : 4u);

        if (bits < best.bits) {
            best = {candidate, wide, bits};
        }

        // Fold to the next coarser level: two neighbouring partitions become
        // one, so their sums simply add. That is what keeps this linear in the
        // block rather than linear in block times partition orders.
        for (int index = 0; index + 1 < partitions; index += 2) {
            sums[static_cast<std::size_t>(index / 2)] =
                sums[static_cast<std::size_t>(index)] + sums[static_cast<std::size_t>(index + 1)];
        }
    }

    return best;
}

/// Residual of the fixed polynomial predictor of `order` at block index `i`.
///
/// These are the binomial differences: order 1 is the first difference, order 2
/// the second, and so on. They are exact integer operations, which is the whole
/// reason a FLAC is lossless -- the decoder adds back what was subtracted, with
/// no rounding anywhere to disagree about.
std::int64_t fixedResidual(const std::int32_t* samples, SampleCount i, int order) noexcept {
    const auto at = [samples](SampleCount index) {
        return static_cast<std::int64_t>(samples[index]);
    };
    switch (order) {
    case 0:
        return at(i);
    case 1:
        return at(i) - at(i - 1);
    case 2:
        return at(i) - 2 * at(i - 1) + at(i - 2);
    case 3:
        return at(i) - 3 * at(i - 1) + 3 * at(i - 2) - at(i - 3);
    default:
        return at(i) - 4 * at(i - 1) + 6 * at(i - 2) - 4 * at(i - 3) + at(i - 4);
    }
}

struct SubframePlan {
    enum class Kind : std::uint8_t { Constant, Verbatim, Fixed };

    Kind kind = Kind::Verbatim;
    int order = 0;
    int wastedBits = 0;
    ResidualPlan residual;
    std::uint64_t bits = 0;
};

/// Trailing zero bits every sample in the block shares.
///
/// Worth finding because it is free: 16-bit audio written as a 24-bit FLAC has
/// eight of them in every sample, and coding them would be coding a constant.
/// Capped one short of the depth so that at least one bit of signal is left to
/// code.
int commonWastedBits(const std::int32_t* samples, SampleCount count, int bitsPerSample) noexcept {
    std::uint32_t merged = 0;
    for (SampleCount i = 0; i < count; ++i) {
        merged |= static_cast<std::uint32_t>(samples[i]);
    }
    if (merged == 0) {
        return 0;
    }
    int wasted = 0;
    while ((merged & 1u) == 0) {
        merged >>= 1;
        ++wasted;
    }
    return std::min(wasted, bitsPerSample - 1);
}

/// Pick the cheapest coding for one channel of one block.
///
/// Every candidate is costed in bits and the smallest wins. Verbatim is always
/// available and always correct, so the search cannot fail to find something --
/// which matters more than finding the best one, because a suboptimal choice
/// costs bytes and a wrong one costs the audio.
SubframePlan planSubframe(const std::int32_t* samples, SampleCount count, int bitsPerSample,
                          std::vector<std::int32_t>& residual, std::vector<std::uint64_t>& sums) {
    SubframePlan plan;

    const bool constant = count > 0 && std::all_of(samples, samples + count,
                                                   [first = samples[0]](std::int32_t value) {
                                                       return value == first;
                                                   });
    if (constant) {
        plan.kind = SubframePlan::Kind::Constant;
        plan.bits = 8 + static_cast<std::uint64_t>(bitsPerSample);
        return plan;
    }

    plan.wastedBits = commonWastedBits(samples, count, bitsPerSample);
    const int effective = bitsPerSample - plan.wastedBits;
    const std::uint64_t headerBits =
        8 + (plan.wastedBits > 0 ? static_cast<std::uint64_t>(plan.wastedBits) : 0u);

    plan.kind = SubframePlan::Kind::Verbatim;
    plan.bits =
        headerBits + static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(effective);

    residual.assign(static_cast<std::size_t>(count), 0);
    const int maxOrder = static_cast<int>(std::min<SampleCount>(4, count - 1));
    for (int order = 0; order <= maxOrder; ++order) {
        bool fits = true;
        for (SampleCount i = order; i < count; ++i) {
            const std::int64_t value = fixedResidual(samples, i, order) >> plan.wastedBits;
            if (value > 2147483647LL || value < -2147483648LL) {
                fits = false; // unreachable at the depths this writer accepts
                break;
            }
            residual[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(value);
        }
        if (!fits) {
            continue;
        }

        const ResidualPlan coded = planResidual(residual.data(), count, order, sums);
        const std::uint64_t bits =
            headerBits + static_cast<std::uint64_t>(order) * static_cast<std::uint64_t>(effective) +
            coded.bits;
        if (bits < plan.bits) {
            plan.kind = SubframePlan::Kind::Fixed;
            plan.order = order;
            plan.residual = coded;
            plan.bits = bits;
        }
    }

    return plan;
}

void writeResidual(BitWriter& writer, const std::int32_t* residual, SampleCount count, int order,
                   const ResidualPlan& plan) {
    writer.writeBits(plan.wideParameters ? 1u : 0u, 2);
    writer.writeBits(static_cast<std::uint32_t>(plan.partitionOrder), 4);

    const int partitions = 1 << plan.partitionOrder;
    for (int index = 0; index < partitions; ++index) {
        const PartitionSpan span = partitionSpan(count, plan.partitionOrder, index, order);
        std::uint64_t sum = 0;
        for (SampleCount i = span.start; i < span.end; ++i) {
            sum += zigzag(residual[i]);
        }

        RiceChoice choice = riceFor(sum, static_cast<std::uint64_t>(span.end - span.start));
        // The largest parameter each method can spell; one past it is the
        // escape code, which this encoder does not emit. Clamping here rather
        // than trusting the plan means a disagreement between the two costs
        // bytes instead of writing an escape marker nothing follows.
        choice.parameter = std::min(choice.parameter, plan.wideParameters ? 30 : 14);

        writer.writeBits(static_cast<std::uint32_t>(choice.parameter), plan.wideParameters ? 5 : 4);
        for (SampleCount i = span.start; i < span.end; ++i) {
            const std::uint32_t mapped = zigzag(residual[i]);
            writer.writeUnary(mapped >> choice.parameter);
            writer.writeBits(mapped, choice.parameter);
        }
    }
}

void writeSubframe(BitWriter& writer, const SubframePlan& plan, const std::int32_t* samples,
                   SampleCount count, int bitsPerSample, std::vector<std::int32_t>& residual) {
    writer.writeBits(0u, 1); // mandatory zero
    switch (plan.kind) {
    case SubframePlan::Kind::Constant:
        writer.writeBits(0b000000u, 6);
        break;
    case SubframePlan::Kind::Verbatim:
        writer.writeBits(0b000001u, 6);
        break;
    case SubframePlan::Kind::Fixed:
        writer.writeBits(0b001000u | static_cast<std::uint32_t>(plan.order), 6);
        break;
    }

    if (plan.wastedBits > 0) {
        writer.writeBits(1u, 1);
        // k wasted bits are spelled as k-1 zeroes and a one.
        writer.writeUnary(static_cast<std::uint32_t>(plan.wastedBits - 1));
    } else {
        writer.writeBits(0u, 1);
    }

    const int effective = bitsPerSample - plan.wastedBits;
    const int shift = plan.wastedBits;

    switch (plan.kind) {
    case SubframePlan::Kind::Constant:
        writer.writeSigned(samples[0], bitsPerSample);
        return;
    case SubframePlan::Kind::Verbatim:
        for (SampleCount i = 0; i < count; ++i) {
            writer.writeSigned(samples[i] >> shift, effective);
        }
        return;
    case SubframePlan::Kind::Fixed:
        break;
    }

    for (SampleCount i = 0; i < plan.order; ++i) {
        writer.writeSigned(samples[i] >> shift, effective);
    }
    residual.assign(static_cast<std::size_t>(count), 0);
    for (SampleCount i = plan.order; i < count; ++i) {
        residual[static_cast<std::size_t>(i)] =
            static_cast<std::int32_t>(fixedResidual(samples, i, plan.order) >> shift);
    }
    writeResidual(writer, residual.data(), count, plan.order, plan.residual);
}

// ---------------------------------------------------------------------------
// Header fields
// ---------------------------------------------------------------------------

/// The four-bit block size field, and any extra bits it defers to the end of
/// the header.
struct BlockSizeCode {
    std::uint32_t code = 0;
    int extraBits = 0;
};

BlockSizeCode blockSizeCode(SampleCount frames) noexcept {
    switch (frames) {
    case 192:
        return {0b0001u, 0};
    case 576:
        return {0b0010u, 0};
    case 1152:
        return {0b0011u, 0};
    case 2304:
        return {0b0100u, 0};
    case 4608:
        return {0b0101u, 0};
    case 256:
        return {0b1000u, 0};
    case 512:
        return {0b1001u, 0};
    case 1024:
        return {0b1010u, 0};
    case 2048:
        return {0b1011u, 0};
    case 4096:
        return {0b1100u, 0};
    case 8192:
        return {0b1101u, 0};
    case 16384:
        return {0b1110u, 0};
    default:
        break;
    }
    // Anything else -- which in practice means the short final frame -- states
    // its length explicitly after the frame number.
    return frames <= 256 ? BlockSizeCode{0b0110u, 8} : BlockSizeCode{0b0111u, 16};
}

struct SampleRateCode {
    std::uint32_t code = 0;
    int extraBits = 0;
    std::uint32_t extraValue = 0;
};

/// Prefer a code that states the rate in the frame header.
///
/// Code zero -- "look it up in STREAMINFO" -- is legal but puts the stream
/// outside the FLAC subset, and the subset is what hardware players implement.
/// Every rate this program can open is expressible, so the fallback is
/// unreachable in practice and is there only so that the function is total.
SampleRateCode sampleRateCode(std::uint32_t hz) noexcept {
    switch (hz) {
    case 88200:
        return {0b0001u, 0, 0};
    case 176400:
        return {0b0010u, 0, 0};
    case 192000:
        return {0b0011u, 0, 0};
    case 8000:
        return {0b0100u, 0, 0};
    case 16000:
        return {0b0101u, 0, 0};
    case 22050:
        return {0b0110u, 0, 0};
    case 24000:
        return {0b0111u, 0, 0};
    case 32000:
        return {0b1000u, 0, 0};
    case 44100:
        return {0b1001u, 0, 0};
    case 48000:
        return {0b1010u, 0, 0};
    case 96000:
        return {0b1011u, 0, 0};
    default:
        break;
    }
    if (hz % 1000u == 0 && hz / 1000u <= 255u) {
        return {0b1100u, 8, hz / 1000u};
    }
    if (hz <= 65535u) {
        return {0b1101u, 16, hz};
    }
    if (hz % 10u == 0 && hz / 10u <= 65535u) {
        return {0b1110u, 16, hz / 10u};
    }
    return {0b0000u, 0, 0};
}

std::uint32_t bitsPerSampleCode(int bits) noexcept {
    switch (bits) {
    case 8:
        return 0b001u;
    case 12:
        return 0b010u;
    case 16:
        return 0b100u;
    case 20:
        return 0b101u;
    case 24:
        return 0b110u;
    default:
        return 0b000u;
    }
}

void appendLittleU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void appendComment(std::vector<std::byte>& out, const std::string& field,
                   const std::string& value) {
    const std::string entry = field + '=' + value;
    appendLittleU32(out, static_cast<std::uint32_t>(entry.size()));
    for (const char character : entry) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

/// The VORBIS_COMMENT body, or nothing when there is no metadata to carry.
///
/// Lengths here are little-endian, unlike every other length in a FLAC stream.
/// That is not a mistake in this code: the block is Vorbis's, embedded whole,
/// and it keeps Vorbis's byte order.
std::vector<std::byte> vorbisComment(const AudioFileMetadata& metadata) {
    struct Entry {
        const char* field;
        const std::string* value;
    };

    const std::array<Entry, 5> entries{
        Entry{"TITLE", &metadata.title}, Entry{"ARTIST", &metadata.artist},
        Entry{"COMMENT", &metadata.comment}, Entry{"DATE", &metadata.date},
        Entry{"ENCODER", &metadata.software}};

    std::uint32_t present = 0;
    for (const Entry& entry : entries) {
        if (!entry.value->empty()) {
            ++present;
        }
    }
    if (present == 0) {
        return {};
    }

    static constexpr std::string_view kVendor = "Auscultate";
    std::vector<std::byte> block;
    appendLittleU32(block, static_cast<std::uint32_t>(kVendor.size()));
    for (const char character : kVendor) {
        block.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    appendLittleU32(block, present);
    for (const Entry& entry : entries) {
        if (!entry.value->empty()) {
            appendComment(block, entry.field, *entry.value);
        }
    }
    return block;
}

std::int32_t quantise(float value, int bits) noexcept {
    // Identical scaling and clamping to WavWriter::encodeSample, so that a
    // buffer written as 24-bit FLAC and the same buffer written as a 24-bit WAV
    // hold the same integers.
    const double clamped = std::clamp(static_cast<double>(value), -1.0, 1.0);
    const auto one = static_cast<double>(std::int64_t{1} << (bits - 1));
    const auto scaled = static_cast<std::int64_t>(std::llround(clamped * one));
    const std::int64_t low = -(std::int64_t{1} << (bits - 1));
    const std::int64_t high = (std::int64_t{1} << (bits - 1)) - 1;
    return static_cast<std::int32_t>(std::clamp(scaled, low, high));
}

} // namespace

// ---------------------------------------------------------------------------
// The writer itself
// ---------------------------------------------------------------------------

struct FlacWriter::Encoder {
    std::ostream* stream = nullptr;
    SampleRate sampleRate;
    ChannelLayout layout;
    FlacOptions options;
    int bitsPerSample = 16;
    int channels = 0;
    std::uint32_t rateHz = 0;
    SampleCount blockFrames = 4096;

    SampleCount framesAccepted = 0;
    std::uint64_t frameNumber = 0;
    std::uint32_t smallestFrame = 0;
    std::uint32_t largestFrame = 0;
    bool finished = false;

    Md5 md5;

    /// One buffer per channel, holding the samples of the block being filled,
    /// plus the two decorrelated signals a stereo block may use instead.
    std::vector<std::vector<std::int32_t>> pending;
    std::vector<std::int32_t> mid;
    std::vector<std::int32_t> side;
    SampleCount pendingFrames = 0;

    std::vector<std::int32_t> residualScratch;
    std::vector<std::uint64_t> sumScratch;
    std::vector<std::byte> frameScratch;
    std::array<std::byte, 8> interleaveScratch{};

    [[nodiscard]] Status emitBlock();
    [[nodiscard]] Status patchStreamInfo();

    /// STREAMINFO, as 34 bytes. Written once with the unknown fields zeroed and
    /// again from finish() with them filled in, which is why it is built in one
    /// place rather than patched field by field.
    [[nodiscard]] std::vector<std::byte>
    streamInfo(const std::array<std::byte, 16>& signature) const;
};

namespace {

void appendMetadataHeader(std::vector<std::byte>& out, std::uint8_t type, bool last,
                          std::uint32_t length) {
    out.push_back(static_cast<std::byte>((last ? 0x80u : 0u) | (type & 0x7Fu)));
    out.push_back(static_cast<std::byte>((length >> 16) & 0xFFu));
    out.push_back(static_cast<std::byte>((length >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(length & 0xFFu));
}

} // namespace

std::vector<std::byte>
FlacWriter::Encoder::streamInfo(const std::array<std::byte, 16>& signature) const {
    std::vector<std::byte> block;
    block.reserve(34);

    const auto blockSize = static_cast<std::uint32_t>(blockFrames);
    block.push_back(static_cast<std::byte>((blockSize >> 8) & 0xFFu));
    block.push_back(static_cast<std::byte>(blockSize & 0xFFu));
    block.push_back(static_cast<std::byte>((blockSize >> 8) & 0xFFu));
    block.push_back(static_cast<std::byte>(blockSize & 0xFFu));

    for (const std::uint32_t size : {smallestFrame, largestFrame}) {
        block.push_back(static_cast<std::byte>((size >> 16) & 0xFFu));
        block.push_back(static_cast<std::byte>((size >> 8) & 0xFFu));
        block.push_back(static_cast<std::byte>(size & 0xFFu));
    }

    // Twenty bits of rate, three of channel count, five of depth and
    // thirty-six of length, packed into eight bytes with no regard for byte
    // boundaries.
    const auto total = static_cast<std::uint64_t>(std::max<SampleCount>(framesAccepted, 0));
    const std::uint64_t packed = (static_cast<std::uint64_t>(rateHz) << 44) |
                                 (static_cast<std::uint64_t>(channels - 1) << 41) |
                                 (static_cast<std::uint64_t>(bitsPerSample - 1) << 36) |
                                 (total & 0xFFFFFFFFFull);
    for (int shift = 56; shift >= 0; shift -= 8) {
        block.push_back(static_cast<std::byte>((packed >> shift) & 0xFFu));
    }

    block.insert(block.end(), signature.begin(), signature.end());
    return block;
}

Status FlacWriter::Encoder::emitBlock() {
    const SampleCount count = pendingFrames;
    if (count <= 0) {
        return Status{};
    }

    // The MD5 in STREAMINFO covers the samples as they would be laid out
    // uncompressed: interleaved, little-endian, signed, at the coded depth.
    const auto sampleBytes = static_cast<std::size_t>(bitsPerSample / 8);
    for (SampleCount i = 0; i < count; ++i) {
        for (int channel = 0; channel < channels; ++channel) {
            const auto value = static_cast<std::uint32_t>(
                pending[static_cast<std::size_t>(channel)][static_cast<std::size_t>(i)]);
            for (std::size_t b = 0; b < sampleBytes; ++b) {
                interleaveScratch[b] = static_cast<std::byte>((value >> (8u * b)) & 0xFFu);
            }
            md5.update(interleaveScratch.data(), sampleBytes);
        }
    }

    // Stereo decorrelation. A left and a right channel of the same performance
    // are mostly the same signal, so coding their difference costs far less
    // than coding both -- but which of the four arrangements wins depends on
    // the material, so all four are costed and the cheapest is used.
    int assignment = channels - 1;
    const SubframePlan* first = nullptr;
    const SubframePlan* second = nullptr;
    const std::int32_t* firstSamples = nullptr;
    const std::int32_t* secondSamples = nullptr;
    int firstDepth = bitsPerSample;
    int secondDepth = bitsPerSample;

    std::vector<SubframePlan> plans;
    plans.reserve(static_cast<std::size_t>(channels) + 2);

    if (channels == 2) {
        const std::int32_t* left = pending[0].data();
        const std::int32_t* right = pending[1].data();
        for (SampleCount i = 0; i < count; ++i) {
            mid[static_cast<std::size_t>(i)] = (left[i] + right[i]) >> 1;
            side[static_cast<std::size_t>(i)] = left[i] - right[i];
        }

        const SubframePlan leftPlan =
            planSubframe(left, count, bitsPerSample, residualScratch, sumScratch);
        const SubframePlan rightPlan =
            planSubframe(right, count, bitsPerSample, residualScratch, sumScratch);
        const SubframePlan midPlan =
            planSubframe(mid.data(), count, bitsPerSample, residualScratch, sumScratch);
        const SubframePlan sidePlan =
            planSubframe(side.data(), count, bitsPerSample + 1, residualScratch, sumScratch);

        plans = {leftPlan, rightPlan, midPlan, sidePlan};
        const std::uint64_t independent = leftPlan.bits + rightPlan.bits;
        const std::uint64_t leftSide = leftPlan.bits + sidePlan.bits;
        const std::uint64_t rightSide = sidePlan.bits + rightPlan.bits;
        const std::uint64_t midSide = midPlan.bits + sidePlan.bits;
        const std::uint64_t best = std::min({independent, leftSide, rightSide, midSide});

        if (best == independent) {
            assignment = 1;
            first = &plans[0];
            second = &plans[1];
            firstSamples = left;
            secondSamples = right;
        } else if (best == leftSide) {
            assignment = 0b1000;
            first = &plans[0];
            second = &plans[3];
            firstSamples = left;
            secondSamples = side.data();
            secondDepth = bitsPerSample + 1;
        } else if (best == rightSide) {
            assignment = 0b1001;
            first = &plans[3];
            second = &plans[1];
            firstSamples = side.data();
            secondSamples = right;
            firstDepth = bitsPerSample + 1;
        } else {
            assignment = 0b1010;
            first = &plans[2];
            second = &plans[3];
            firstSamples = mid.data();
            secondSamples = side.data();
            secondDepth = bitsPerSample + 1;
        }
    } else {
        for (int channel = 0; channel < channels; ++channel) {
            plans.push_back(planSubframe(pending[static_cast<std::size_t>(channel)].data(), count,
                                         bitsPerSample, residualScratch, sumScratch));
        }
    }

    frameScratch.clear();
    BitWriter header{frameScratch};

    const BlockSizeCode blockCode = blockSizeCode(count);
    const SampleRateCode rateCode = sampleRateCode(rateHz);

    header.writeBits(0b11111111111110u, 14);
    header.writeBits(0u, 1); // reserved
    header.writeBits(0u, 1); // fixed block size, so what follows is a frame number
    header.writeBits(blockCode.code, 4);
    header.writeBits(rateCode.code, 4);
    header.writeBits(static_cast<std::uint32_t>(assignment), 4);
    header.writeBits(bitsPerSampleCode(bitsPerSample), 3);
    header.writeBits(0u, 1); // reserved
    appendUtf8Number(header, frameNumber);
    if (blockCode.extraBits > 0) {
        header.writeBits(static_cast<std::uint32_t>(count - 1), blockCode.extraBits);
    }
    if (rateCode.extraBits > 0) {
        header.writeBits(rateCode.extraValue, rateCode.extraBits);
    }

    // Every field above is a whole number of bytes in total, so the header ends
    // aligned and its CRC covers exactly the bytes emitted so far.
    frameScratch.push_back(static_cast<std::byte>(crc8(frameScratch.data(), frameScratch.size())));

    BitWriter body{frameScratch};
    if (channels == 2) {
        writeSubframe(body, *first, firstSamples, count, firstDepth, residualScratch);
        writeSubframe(body, *second, secondSamples, count, secondDepth, residualScratch);
    } else {
        for (int channel = 0; channel < channels; ++channel) {
            writeSubframe(body, plans[static_cast<std::size_t>(channel)],
                          pending[static_cast<std::size_t>(channel)].data(), count, bitsPerSample,
                          residualScratch);
        }
    }
    body.alignToByte();

    const std::uint16_t footer = crc16(frameScratch.data(), frameScratch.size());
    frameScratch.push_back(static_cast<std::byte>((footer >> 8) & 0xFFu));
    frameScratch.push_back(static_cast<std::byte>(footer & 0xFFu));

    stream->write(reinterpret_cast<const char*>(frameScratch.data()),
                  static_cast<std::streamsize>(frameScratch.size()));
    if (!*stream) {
        return Error{ErrorCode::IoFailure, "failed to write a FLAC frame"};
    }

    const auto frameBytes = static_cast<std::uint32_t>(frameScratch.size());
    smallestFrame = smallestFrame == 0 ? frameBytes : std::min(smallestFrame, frameBytes);
    largestFrame = std::max(largestFrame, frameBytes);

    ++frameNumber;
    pendingFrames = 0;
    return Status{};
}

Status FlacWriter::Encoder::patchStreamInfo() {
    const std::array<std::byte, 16> signature = md5.finish();
    const std::vector<std::byte> block = streamInfo(signature);

    const auto endPosition = stream->tellp();
    stream->seekp(8); // "fLaC" and the STREAMINFO block header
    stream->write(reinterpret_cast<const char*>(block.data()),
                  static_cast<std::streamsize>(block.size()));
    stream->seekp(endPosition);
    stream->flush();

    if (!*stream) {
        return Error{ErrorCode::IoFailure, "failed to patch FLAC STREAMINFO"};
    }
    return Status{};
}

FlacWriter::FlacWriter(std::unique_ptr<Encoder> encoder) noexcept : encoder_(std::move(encoder)) {}

FlacWriter::FlacWriter(FlacWriter&&) noexcept = default;

FlacWriter& FlacWriter::operator=(FlacWriter&&) noexcept = default;

FlacWriter::~FlacWriter() = default;

Result<FlacWriter> FlacWriter::create(std::ostream& stream, SampleRate sampleRate,
                                      ChannelLayout layout, FlacOptions options) {
    if (!sampleRate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "invalid sample rate"};
    }
    if (layout.count() <= 0 || layout.count() > kMaxFlacChannels) {
        return Error{ErrorCode::InvalidArgument,
                     "FLAC carries at most " + std::to_string(kMaxFlacChannels) + " channels"};
    }
    if (options.format != SampleFormat::PcmInt16 && options.format != SampleFormat::PcmInt24) {
        return Error{ErrorCode::InvalidArgument, "FLAC output is 16- or 24-bit integer PCM; " +
                                                     std::string{toString(options.format)} +
                                                     " is not one of them"};
    }
    if (options.blockFrames < 16 || options.blockFrames > 65535) {
        return Error{ErrorCode::InvalidArgument, "FLAC block size must be between 16 and 65535"};
    }

    const auto rateHz = static_cast<std::uint32_t>(std::llround(sampleRate.hz()));
    if (rateHz == 0 || rateHz > 1048575u) {
        return Error{ErrorCode::InvalidArgument,
                     "FLAC stores its sample rate in twenty bits and cannot express " +
                         std::to_string(sampleRate.hz()) + " Hz"};
    }

    auto encoder = std::make_unique<Encoder>();
    encoder->stream = &stream;
    encoder->sampleRate = sampleRate;
    encoder->layout = layout;
    encoder->bitsPerSample = bytesPerSample(options.format) * 8;
    encoder->channels = layout.count();
    encoder->rateHz = rateHz;
    encoder->blockFrames = options.blockFrames;
    encoder->options = std::move(options);

    encoder->pending.assign(
        static_cast<std::size_t>(encoder->channels),
        std::vector<std::int32_t>(static_cast<std::size_t>(encoder->blockFrames), 0));
    if (encoder->channels == 2) {
        encoder->mid.assign(static_cast<std::size_t>(encoder->blockFrames), 0);
        encoder->side.assign(static_cast<std::size_t>(encoder->blockFrames), 0);
    }

    const std::vector<std::byte> comment = vorbisComment(encoder->options.metadata);

    std::vector<std::byte> header;
    header.reserve(64 + comment.size());
    for (const char character : std::string_view{"fLaC"}) {
        header.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }

    appendMetadataHeader(header, std::uint8_t{0}, comment.empty(), 34);
    const std::array<std::byte, 16> unknownSignature{};
    const std::vector<std::byte> initialInfo = encoder->streamInfo(unknownSignature);
    header.insert(header.end(), initialInfo.begin(), initialInfo.end());

    if (!comment.empty()) {
        appendMetadataHeader(header, std::uint8_t{4}, true,
                             static_cast<std::uint32_t>(comment.size()));
        header.insert(header.end(), comment.begin(), comment.end());
    }

    stream.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    if (!stream) {
        return Error{ErrorCode::IoFailure, "failed to write FLAC header"};
    }

    return FlacWriter{std::move(encoder)};
}

Status FlacWriter::write(ConstAudioBufferView frames) {
    if (encoder_ == nullptr || encoder_->finished) {
        return Error{ErrorCode::InvalidArgument, "writer is not open"};
    }
    if (frames.isEmpty()) {
        return Status{};
    }

    Encoder& encoder = *encoder_;
    SampleCount done = 0;
    while (done < frames.frames()) {
        const SampleCount batch =
            std::min(encoder.blockFrames - encoder.pendingFrames, frames.frames() - done);

        for (int channel = 0; channel < encoder.channels; ++channel) {
            // Channels the caller did not supply are written as silence rather
            // than left holding the previous block.
            const bool haveChannel = channel < frames.channelCount();
            const float* in = haveChannel ? frames.channel(channel) + done : nullptr;
            std::int32_t* out =
                encoder.pending[static_cast<std::size_t>(channel)].data() + encoder.pendingFrames;
            for (SampleCount i = 0; i < batch; ++i) {
                out[i] = quantise(haveChannel ? in[i] : 0.0f, encoder.bitsPerSample);
            }
        }

        encoder.pendingFrames += batch;
        encoder.framesAccepted += batch;
        done += batch;

        if (encoder.pendingFrames == encoder.blockFrames) {
            if (auto status = encoder.emitBlock(); !status) {
                return status;
            }
        }
    }

    return Status{};
}

Status FlacWriter::finish() {
    if (encoder_ == nullptr || encoder_->finished) {
        return Error{ErrorCode::InvalidArgument, "writer is not open"};
    }
    Encoder& encoder = *encoder_;
    encoder.finished = true;

    if (auto status = encoder.emitBlock(); !status) {
        return status;
    }
    return encoder.patchStreamInfo();
}

SampleCount FlacWriter::framesWritten() const noexcept {
    return encoder_ == nullptr ? 0 : encoder_->framesAccepted;
}

Status FlacWriter::writeFile(const std::filesystem::path& path, ConstAudioBufferView frames,
                             SampleRate sampleRate, ChannelLayout layout, FlacOptions options) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        return Error{ErrorCode::IoFailure, "cannot create " + path.string()};
    }

    auto writer = FlacWriter::create(stream, sampleRate, layout, std::move(options));
    if (!writer) {
        return writer.error();
    }
    if (auto status = writer.value().write(frames); !status) {
        return status;
    }
    return writer.value().finish();
}

} // namespace sa::io
