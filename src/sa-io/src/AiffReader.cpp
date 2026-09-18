#include <sa/io/AiffReader.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <string>

namespace sa::io {

namespace {

std::uint16_t readBigU16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) << 8) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1])));
}

std::uint32_t readBigU32(const std::byte* p) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) << 24) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 8) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3]));
}

std::uint64_t readBigU64(const std::byte* p) noexcept {
    return (static_cast<std::uint64_t>(readBigU32(p)) << 32) |
           static_cast<std::uint64_t>(readBigU32(p + 4));
}

/// Explicit rather than std::byteswap, which is C++23.
std::uint64_t readLittleU64(const std::byte* p) noexcept {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i]));
    }
    return value;
}

constexpr std::uint32_t fourCc(const char (&tag)[5]) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(tag[2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(tag[3]));
}

constexpr std::uint32_t kForm = fourCc("FORM");
constexpr std::uint32_t kAiff = fourCc("AIFF");
constexpr std::uint32_t kAifc = fourCc("AIFC");
constexpr std::uint32_t kComm = fourCc("COMM");
constexpr std::uint32_t kSsnd = fourCc("SSND");
constexpr std::uint32_t kName = fourCc("NAME");
constexpr std::uint32_t kAuth = fourCc("AUTH");
constexpr std::uint32_t kAnno = fourCc("ANNO");
constexpr std::uint32_t kNone = fourCc("NONE");
constexpr std::uint32_t kSowt = fourCc("sowt");
constexpr std::uint32_t kFl32 = fourCc("fl32");
constexpr std::uint32_t kFl32Upper = fourCc("FL32");
constexpr std::uint32_t kFl64 = fourCc("fl64");
constexpr std::uint32_t kFl64Upper = fourCc("FL64");

constexpr std::uint32_t kMaxMetadataChunkBytes = 16u * 1024u * 1024u;

/// Decode the 80-bit IEEE 754 extended float AIFF uses for its sample rate.
///
/// No mainstream compiler exposes this type portably, so it is unpacked by
/// hand: one sign bit, a 15-bit exponent biased by 16383, and a 64-bit mantissa
/// with an explicit leading integer bit (unlike the implicit bit in float and
/// double).
double readExtendedFloat(const std::byte* p) noexcept {
    const std::uint16_t signAndExponent = readBigU16(p);
    const std::uint64_t mantissa = readBigU64(p + 2);

    const bool negative = (signAndExponent & 0x8000u) != 0;
    const auto exponent = static_cast<int>(signAndExponent & 0x7FFFu);

    if (exponent == 0 && mantissa == 0) {
        return 0.0;
    }
    if (exponent == 0x7FFF) {
        return 0.0; // infinity or NaN: no valid sample rate, caller rejects it
    }

    const double value = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    return negative ? -value : value;
}

SampleFormat resolveFormat(std::uint16_t bits, std::uint32_t compression) noexcept {
    if (compression == kFl32 || compression == kFl32Upper) {
        return SampleFormat::Float32;
    }
    if (compression == kFl64 || compression == kFl64Upper) {
        return SampleFormat::Float64;
    }
    if (compression != kNone && compression != kSowt) {
        return SampleFormat::Unknown; // genuinely compressed
    }
    switch (bits) {
    case 8:
        return SampleFormat::PcmInt16; // handled below as 8-bit signed
    case 16:
        return SampleFormat::PcmInt16;
    case 24:
        return SampleFormat::PcmInt24;
    case 32:
        return SampleFormat::PcmInt32;
    default:
        return SampleFormat::Unknown;
    }
}

float decodeSample(const std::byte* p, SampleFormat format, bool littleEndian) noexcept {
    // Read the raw word in the container's byte order, then interpret.
    switch (format) {
    case SampleFormat::PcmInt16: {
        const std::uint16_t raw =
            littleEndian ? static_cast<std::uint16_t>(
                               static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) |
                               static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1]) << 8))
                         : readBigU16(p);
        return static_cast<float>(static_cast<std::int16_t>(raw)) / 32768.0f;
    }
    case SampleFormat::PcmInt24: {
        const std::uint32_t raw =
            littleEndian
                ? (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16))
                : ((static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) << 16) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8) |
                   static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])));
        const std::int32_t value = (raw & 0x800000u) != 0
                                       ? static_cast<std::int32_t>(raw | 0xFF000000u)
                                       : static_cast<std::int32_t>(raw);
        return static_cast<float>(value) / 8388608.0f;
    }
    case SampleFormat::PcmInt32: {
        const std::uint32_t raw =
            littleEndian
                ? (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24))
                : readBigU32(p);
        return static_cast<float>(static_cast<std::int32_t>(raw)) / 2147483648.0f;
    }
    case SampleFormat::Float32: {
        const std::uint32_t raw =
            littleEndian
                ? (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16) |
                   (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24))
                : readBigU32(p);
        return std::bit_cast<float>(raw);
    }
    case SampleFormat::Float64: {
        const std::uint64_t raw = littleEndian ? readLittleU64(p) : readBigU64(p);
        return static_cast<float>(std::bit_cast<double>(raw));
    }
    case SampleFormat::PcmUInt8:
    case SampleFormat::Unknown:
        return 0.0f;
    }
    return 0.0f;
}

std::string readPascalOrPlainString(const std::byte* data, std::size_t length) {
    std::size_t actual = 0;
    while (actual < length && std::to_integer<char>(data[actual]) != '\0') {
        ++actual;
    }
    std::string text(reinterpret_cast<const char*>(data), actual);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
    return text;
}

} // namespace

Result<AiffReader> AiffReader::open(const std::filesystem::path& path) {
    auto source = FileByteSource::open(path);
    if (!source) {
        return source.error();
    }
    return parse(std::shared_ptr<const ByteSource>{std::move(source).value()});
}

Result<AiffReader> AiffReader::fromMemory(std::span<const std::byte> bytes) {
    return parse(std::make_shared<const MemoryByteSource>(bytes));
}

Result<AiffReader> AiffReader::parse(std::shared_ptr<const ByteSource> source) {
    std::array<std::byte, 12> header{};
    if (source->read(0, header) != header.size()) {
        return Error{ErrorCode::CorruptData, "file is too short to be an AIFF"};
    }
    if (readBigU32(header.data()) != kForm) {
        return Error{ErrorCode::UnsupportedFormat, "not a FORM container"};
    }
    const std::uint32_t formType = readBigU32(header.data() + 8);
    if (formType != kAiff && formType != kAifc) {
        return Error{ErrorCode::UnsupportedFormat, "FORM container is not AIFF or AIFC"};
    }

    AiffReader reader;
    reader.source_ = std::move(source);
    const ByteSource& bytes = *reader.source_;

    bool haveCommon = false;
    bool haveSound = false;
    SampleCount declaredFrames = 0;

    std::uint64_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        std::array<std::byte, 8> chunkHeader{};
        if (bytes.read(offset, chunkHeader) != chunkHeader.size()) {
            break;
        }
        const std::uint32_t id = readBigU32(chunkHeader.data());
        const std::uint32_t declaredSize = readBigU32(chunkHeader.data() + 4);
        const std::uint64_t bodyOffset = offset + 8;

        // Clamp the declared size to what the source actually holds before it
        // is used for anything, exactly as in the WAV parser.
        const std::uint64_t available = bytes.size() - std::min(bodyOffset, bytes.size());
        const std::uint64_t bodySize = std::min<std::uint64_t>(declaredSize, available);

        if (id == kComm && bodySize >= 18) {
            std::vector<std::byte> body(static_cast<std::size_t>(bodySize));
            if (bytes.read(bodyOffset, body) == body.size()) {
                const auto channels = readBigU16(body.data());
                declaredFrames = static_cast<SampleCount>(readBigU32(body.data() + 2));
                const auto bits = readBigU16(body.data() + 6);
                const double rate = readExtendedFloat(body.data() + 8);

                std::uint32_t compression = kNone;
                if (formType == kAifc && bodySize >= 22) {
                    compression = readBigU32(body.data() + 18);
                }

                if (channels == 0 || channels > kMaxChannels) {
                    return Error{ErrorCode::UnsupportedFormat, "unsupported channel count"};
                }
                const SampleRate sampleRate{rate};
                if (!sampleRate.isValid()) {
                    return Error{ErrorCode::UnsupportedFormat, "unsupported sample rate"};
                }

                const SampleFormat format = resolveFormat(bits, compression);
                if (format == SampleFormat::Unknown) {
                    return Error{ErrorCode::UnsupportedFormat,
                                 "unsupported AIFF encoding (" + std::to_string(bits) +
                                     " bits, compression " + std::to_string(compression) + ")"};
                }
                if (bits == 8) {
                    return Error{ErrorCode::UnsupportedFormat, "8-bit AIFF is not supported"};
                }

                reader.info_.sampleRate = sampleRate;
                reader.info_.layout = channels == 1   ? ChannelLayout::mono()
                                      : channels == 2 ? ChannelLayout::stereo()
                                                      : ChannelLayout::discrete(channels);
                reader.info_.format = format;
                reader.littleEndianSamples_ =
                    compression == kSowt || compression == kFl32 || compression == kFl64;
                reader.bytesPerFrame_ =
                    static_cast<std::uint32_t>(bytesPerSample(format)) * channels;
                haveCommon = true;
            }
        } else if (id == kSsnd && bodySize >= 8) {
            std::array<std::byte, 8> soundHeader{};
            if (bytes.read(bodyOffset, soundHeader) == soundHeader.size()) {
                // The SSND offset is a byte skip before the samples begin; it is
                // almost always zero but must be honoured when it is not.
                const std::uint32_t dataStart = readBigU32(soundHeader.data());
                const std::uint64_t samplesAt = bodyOffset + 8 + dataStart;
                if (samplesAt <= bytes.size()) {
                    reader.dataOffset_ = samplesAt;
                    haveSound = true;
                }
            }
        } else if ((id == kName || id == kAuth || id == kAnno) && bodySize > 0 &&
                   bodySize <= kMaxMetadataChunkBytes) {
            std::vector<std::byte> body(static_cast<std::size_t>(bodySize));
            if (bytes.read(bodyOffset, body) == body.size()) {
                std::string text = readPascalOrPlainString(body.data(), body.size());
                if (id == kName) {
                    reader.metadata_.title = std::move(text);
                } else if (id == kAuth) {
                    reader.metadata_.artist = std::move(text);
                } else {
                    reader.metadata_.comment = std::move(text);
                }
            }
        }

        // Advance is always at least 8, so a zero-size chunk cannot stall the
        // scan. IFF pads odd chunks to even, like RIFF.
        offset = bodyOffset + bodySize + (bodySize & 1u);
    }

    if (!haveCommon) {
        return Error{ErrorCode::CorruptData, "AIFF has no COMM chunk"};
    }
    if (!haveSound) {
        return Error{ErrorCode::CorruptData, "AIFF has no SSND chunk"};
    }
    if (reader.bytesPerFrame_ == 0) {
        return Error{ErrorCode::CorruptData, "AIFF frame size resolved to zero"};
    }

    // AIFF states its frame count in the header rather than implying it from
    // the data length, so a file can claim more than it carries. Trust the
    // smaller of the two.
    const std::uint64_t availableBytes = bytes.size() - reader.dataOffset_;
    const auto framesPresent = static_cast<SampleCount>(availableBytes / reader.bytesPerFrame_);
    reader.info_.frameCount = std::min(std::max<SampleCount>(0, declaredFrames), framesPresent);

    return reader;
}

Result<SampleCount> AiffReader::read(SampleIndex startFrame, AudioBufferView destination) const {
    if (source_ == nullptr) {
        return Error{ErrorCode::InvalidArgument, "reader is not open"};
    }
    if (startFrame < 0) {
        return Error{ErrorCode::OutOfRange, "startFrame is negative"};
    }
    if (destination.isEmpty() || startFrame >= info_.frameCount) {
        return SampleCount{0};
    }

    const SampleCount wanted = std::min(destination.frames(), info_.frameCount - startFrame);
    const int copyChannels = std::min(info_.channelCount(), destination.channelCount());
    const int sampleBytes = bytesPerSample(info_.format);

    constexpr std::size_t kBlockBytes = 16 * 1024;
    std::array<std::byte, kBlockBytes> block{};
    const auto framesPerBlock =
        static_cast<SampleCount>(kBlockBytes / std::max<std::uint32_t>(1, bytesPerFrame_));
    if (framesPerBlock == 0) {
        return Error{ErrorCode::UnsupportedFormat, "frame larger than the read block"};
    }

    SampleCount done = 0;
    while (done < wanted) {
        const SampleCount batch = std::min(framesPerBlock, wanted - done);
        const std::uint64_t byteOffset =
            dataOffset_ + static_cast<std::uint64_t>(startFrame + done) * bytesPerFrame_;
        const auto byteCount = static_cast<std::size_t>(batch) * bytesPerFrame_;

        const std::size_t got = source_->read(byteOffset, std::span{block.data(), byteCount});
        const auto framesGot = static_cast<SampleCount>(got / bytesPerFrame_);
        if (framesGot == 0) {
            break;
        }

        for (int channel = 0; channel < copyChannels; ++channel) {
            float* out = destination.channel(channel) + done;
            const std::byte* base = block.data() + static_cast<std::size_t>(channel) *
                                                       static_cast<std::size_t>(sampleBytes);
            for (SampleCount frame = 0; frame < framesGot; ++frame) {
                out[frame] = decodeSample(base + static_cast<std::size_t>(frame) * bytesPerFrame_,
                                          info_.format, littleEndianSamples_);
            }
        }

        done += framesGot;
        if (framesGot < batch) {
            break;
        }
    }

    return done;
}

Result<AudioBuffer> AiffReader::readAll() const {
    AudioBuffer buffer{info_.layout, info_.frameCount};
    if (info_.frameCount == 0) {
        return buffer;
    }
    auto framesRead = read(0, buffer.view());
    if (!framesRead) {
        return framesRead.error();
    }
    return buffer;
}

} // namespace sa::io
