#include <sa/io/AiffReader.h>
#include <sa/io/AiffWriter.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace sa;
using namespace sa::io;

namespace {

std::vector<std::byte> toBytes(const std::string& text) {
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return bytes;
}

std::vector<std::byte> encode(ConstAudioBufferView frames, SampleRate rate, ChannelLayout layout,
                              AiffOptions options = {}) {
    std::ostringstream stream{std::ios::binary};
    auto writer = AiffWriter::create(stream, rate, layout, std::move(options));
    REQUIRE(writer.hasValue());
    REQUIRE(writer.value().write(frames).ok());
    REQUIRE(writer.value().finish().ok());
    return toBytes(stream.str());
}

/// A signal made of exact codes at `bits`, so that a lossless round trip is
/// bit-identical rather than merely close.
///
/// Every sample is k / 2^(bits-1) for a whole k, which is what the file will
/// hold and what the reader will hand back. At 32 bits the codes are stepped by
/// 256: a float32 carries 24 bits of significand, so a full 32-bit code is not
/// representable in the buffer the audio passes through, and a test that
/// pretended otherwise would be measuring float32 rather than the writer.
AudioBuffer exactCodes(int channels, SampleCount frames, int bits, unsigned seed = 7u) {
    AudioBuffer buffer{channels == 1   ? ChannelLayout::mono()
                       : channels == 2 ? ChannelLayout::stereo()
                                       : ChannelLayout::discrete(channels),
                       frames};
    const auto scale = static_cast<double>(std::int64_t{1} << (bits - 1));
    const int step = bits >= 32 ? 256 : 1;
    const auto limit = static_cast<std::int64_t>(scale) / step;

    std::mt19937 generator{seed};
    std::uniform_int_distribution<std::int64_t> codes{-limit, limit - 1};
    for (int channel = 0; channel < channels; ++channel) {
        float* samples = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            samples[i] = static_cast<float>(static_cast<double>(codes(generator) * step) / scale);
        }
    }
    return buffer;
}

std::uint32_t readBigU32(const std::vector<std::byte>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) |
                static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + i]));
    }
    return value;
}

/// Offset of a four-character chunk tag, or npos.
std::size_t findTag(const std::vector<std::byte>& bytes, const char* tag) {
    for (std::size_t i = 0; i + 4 <= bytes.size(); ++i) {
        if (std::to_integer<char>(bytes[i]) == tag[0] &&
            std::to_integer<char>(bytes[i + 1]) == tag[1] &&
            std::to_integer<char>(bytes[i + 2]) == tag[2] &&
            std::to_integer<char>(bytes[i + 3]) == tag[3]) {
            return i;
        }
    }
    return std::string::npos;
}

} // namespace

TEST_CASE("AIFF round-trips bit-identically at every depth it writes", "[io][aiff][writer]") {
    for (const int bits : {16, 24, 32}) {
        const AudioBuffer source = exactCodes(2, 500, bits);

        AiffOptions options;
        options.format = bits == 16   ? SampleFormat::PcmInt16
                         : bits == 24 ? SampleFormat::PcmInt24
                                      : SampleFormat::PcmInt32;
        const auto bytes =
            encode(source.constView(), kSampleRate48000, ChannelLayout::stereo(), options);

        auto reader = AiffReader::fromMemory(bytes);
        INFO("bit depth " << bits);
        REQUIRE(reader.hasValue());

        const AudioFileInfo& info = reader.value().info();
        CHECK(info.frameCount == 500);
        CHECK(info.channelCount() == 2);
        CHECK(info.sampleRate == kSampleRate48000);
        CHECK(info.format == options.format);

        auto restored = reader.value().readAll();
        REQUIRE(restored.hasValue());

        // Not a tolerance. The samples were chosen to land on codes the file can
        // hold exactly, so anything but equality is a real defect in the writer.
        for (int channel = 0; channel < 2; ++channel) {
            for (SampleCount i = 0; i < 500; ++i) {
                REQUIRE(restored.value().channel(channel)[i] == source.channel(channel)[i]);
            }
        }
    }
}

TEST_CASE("AIFF samples are written big-endian", "[io][aiff][writer]") {
    // The one thing an AIFF writer is most likely to get wrong, so it is
    // checked against the bytes rather than inferred from a round trip through
    // our own reader -- which would agree with the writer either way round.
    AudioBuffer source{ChannelLayout::mono(), 1};
    source.channel(0)[0] = 0x1234 / 32768.0f;

    AiffOptions options;
    options.format = SampleFormat::PcmInt16;
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), options);

    const std::size_t sound = findTag(bytes, "SSND");
    REQUIRE(sound != std::string::npos);
    const std::size_t samples = sound + 16; // tag, size, offset, block size

    REQUIRE(bytes.size() >= samples + 2);
    CHECK(std::to_integer<std::uint8_t>(bytes[samples]) == 0x12u);
    CHECK(std::to_integer<std::uint8_t>(bytes[samples + 1]) == 0x34u);

    // And the same for 24-bit, where a byte-swap bug can hide in the middle
    // byte and still sound roughly right.
    AudioBuffer wide{ChannelLayout::mono(), 1};
    wide.channel(0)[0] = 0x123456 / 8388608.0f;
    options.format = SampleFormat::PcmInt24;
    const auto wideBytes =
        encode(wide.constView(), kSampleRate48000, ChannelLayout::mono(), options);
    const std::size_t wideSamples = findTag(wideBytes, "SSND") + 16;
    CHECK(std::to_integer<std::uint8_t>(wideBytes[wideSamples]) == 0x12u);
    CHECK(std::to_integer<std::uint8_t>(wideBytes[wideSamples + 1]) == 0x34u);
    CHECK(std::to_integer<std::uint8_t>(wideBytes[wideSamples + 2]) == 0x56u);
}

TEST_CASE("The 80-bit extended sample rate is written back exactly", "[io][aiff][writer]") {
    for (const double hz : {8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0, 44056.0}) {
        const AudioBuffer source = exactCodes(1, 8, 16);
        const auto bytes =
            encode(source.constView(), SampleRate{hz}, ChannelLayout::mono(), AiffOptions{});
        auto reader = AiffReader::fromMemory(bytes);
        INFO("rate " << hz);
        REQUIRE(reader.hasValue());
        CHECK(reader.value().info().sampleRate.hz() == hz);
    }
}

TEST_CASE("Channel counts round-trip, mono to many", "[io][aiff][writer]") {
    for (const int channels : {1, 2, 3, 6, 8}) {
        const AudioBuffer source = exactCodes(channels, 97, 24);
        const ChannelLayout layout = channels == 1   ? ChannelLayout::mono()
                                     : channels == 2 ? ChannelLayout::stereo()
                                                     : ChannelLayout::discrete(channels);
        const auto bytes = encode(source.constView(), kSampleRate48000, layout, AiffOptions{});

        auto reader = AiffReader::fromMemory(bytes);
        INFO("channels " << channels);
        REQUIRE(reader.hasValue());
        CHECK(reader.value().info().channelCount() == channels);
        CHECK(reader.value().info().frameCount == 97);

        auto restored = reader.value().readAll();
        REQUIRE(restored.hasValue());
        for (int channel = 0; channel < channels; ++channel) {
            for (SampleCount i = 0; i < 97; ++i) {
                REQUIRE(restored.value().channel(channel)[i] == source.channel(channel)[i]);
            }
        }
    }
}

TEST_CASE("An odd-length AIFF data chunk is padded and still parses", "[io][aiff][writer]") {
    // One channel of 24-bit at an odd frame count: 3 * 7 = 21 bytes of audio,
    // which IFF requires be followed by a pad byte that is counted in the FORM
    // size and not in SSND's.
    const AudioBuffer source = exactCodes(1, 7, 24);
    AiffOptions options;
    options.format = SampleFormat::PcmInt24;
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), options);

    CHECK(bytes.size() % 2 == 0);
    CHECK(readBigU32(bytes, 4) == bytes.size() - 8);

    const std::size_t sound = findTag(bytes, "SSND");
    REQUIRE(sound != std::string::npos);
    CHECK(readBigU32(bytes, sound + 4) == 8 + 21);
    // The pad is the last byte and it is outside the declared chunk length.
    CHECK(sound + 8 + 8 + 21 + 1 == bytes.size());

    auto reader = AiffReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == 7);
    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());
    for (SampleCount i = 0; i < 7; ++i) {
        REQUIRE(restored.value().channel(0)[i] == source.channel(0)[i]);
    }
}

TEST_CASE("An odd-length AIFF text chunk is padded too", "[io][aiff][writer]") {
    // A five-character title: the chunk body is odd, so what follows it must
    // still start on an even offset or every later chunk is misread.
    const AudioBuffer source = exactCodes(1, 8, 16);
    AiffOptions options;
    options.metadata.title = "Three";
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), options);

    auto reader = AiffReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().metadata().title == "Three");
    CHECK(reader.value().info().frameCount == 8);
}

TEST_CASE("AIFF text metadata survives the round trip", "[io][aiff][writer][metadata]") {
    const AudioBuffer source = exactCodes(2, 64, 16);
    AiffOptions options;
    options.metadata.title = "Rehearsal take 4";
    options.metadata.artist = "The Quartet";
    options.metadata.comment = "Second desk, slightly flat";

    const auto bytes =
        encode(source.constView(), kSampleRate44100, ChannelLayout::stereo(), options);
    auto reader = AiffReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    CHECK(reader.value().metadata().title == "Rehearsal take 4");
    CHECK(reader.value().metadata().artist == "The Quartet");
    CHECK(reader.value().metadata().comment == "Second desk, slightly flat");
    CHECK(reader.value().info().sampleRate == kSampleRate44100);
}

TEST_CASE("Out-of-range samples clamp instead of wrapping", "[io][aiff][writer]") {
    AudioBuffer source{ChannelLayout::mono(), 4};
    const float values[] = {2.0f, -2.0f, 1.0f, -1.0f};
    for (SampleCount i = 0; i < 4; ++i) {
        source.channel(0)[i] = values[i];
    }

    AiffOptions options;
    options.format = SampleFormat::PcmInt16;
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), options);
    auto reader = AiffReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());

    // Full scale positive is one code short of 1.0 at 16 bits; negative full
    // scale is exact. Wrapping would put the first sample at the bottom of the
    // range instead of the top, which is the failure this guards.
    CHECK(restored.value().channel(0)[0] == 32767.0f / 32768.0f);
    CHECK(restored.value().channel(0)[1] == -1.0f);
    CHECK(restored.value().channel(0)[2] == 32767.0f / 32768.0f);
    CHECK(restored.value().channel(0)[3] == -1.0f);
}

TEST_CASE("The AIFF writer refuses what AIFF cannot hold", "[io][aiff][writer]") {
    std::ostringstream stream{std::ios::binary};

    for (const SampleFormat format : {SampleFormat::Float32, SampleFormat::Float64,
                                      SampleFormat::PcmUInt8, SampleFormat::Unknown}) {
        AiffOptions options;
        options.format = format;
        auto writer = AiffWriter::create(stream, kSampleRate48000, ChannelLayout::mono(), options);
        INFO("format " << toString(format));
        CHECK_FALSE(writer.hasValue());
        CHECK(writer.error().code() == ErrorCode::InvalidArgument);
    }

    CHECK_FALSE(AiffWriter::create(stream, SampleRate{0.0}, ChannelLayout::stereo()).hasValue());
    CHECK_FALSE(AiffWriter::create(stream, kSampleRate48000, ChannelLayout{}).hasValue());
}

TEST_CASE("An empty AIFF is valid and reads as zero frames", "[io][aiff][writer]") {
    std::ostringstream stream{std::ios::binary};
    auto writer = AiffWriter::create(stream, kSampleRate48000, ChannelLayout::stereo());
    REQUIRE(writer.hasValue());
    REQUIRE(writer.value().finish().ok());

    // Named, not a temporary: fromMemory does not copy, so a reader over the
    // result of an expression outlives the bytes it is reading.
    const auto bytes = toBytes(stream.str());
    auto reader = AiffReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == 0);
    CHECK(reader.value().info().channelCount() == 2);
}

TEST_CASE("Streamed AIFF writes reassemble into one file", "[io][aiff][writer]") {
    const AudioBuffer source = exactCodes(2, 1000, 24);

    std::ostringstream stream{std::ios::binary};
    AiffOptions options;
    options.format = SampleFormat::PcmInt24;
    auto writer = AiffWriter::create(stream, kSampleRate48000, ChannelLayout::stereo(), options);
    REQUIRE(writer.hasValue());

    // Uneven blocks on purpose: a writer that only ever sees one call cannot
    // get the running frame count wrong.
    SampleCount cursor = 0;
    for (const SampleCount span :
         {SampleCount{1}, SampleCount{63}, SampleCount{256}, SampleCount{680}}) {
        REQUIRE(writer.value().write(source.constView().subRange(cursor, span)).ok());
        cursor += span;
    }
    REQUIRE(writer.value().finish().ok());
    CHECK(writer.value().framesWritten() == 1000);

    const auto bytes = toBytes(stream.str());
    auto reader = AiffReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == 1000);
    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());
    for (int channel = 0; channel < 2; ++channel) {
        for (SampleCount i = 0; i < 1000; ++i) {
            REQUIRE(restored.value().channel(channel)[i] == source.channel(channel)[i]);
        }
    }
}

TEST_CASE("Writing an AIFF to a path and reading it back works", "[io][aiff][writer]") {
    const auto path = std::filesystem::temp_directory_path() / "sa-aiff-writer-test.aiff";
    std::filesystem::remove(path);

    const AudioBuffer source = exactCodes(1, 333, 16);
    AiffOptions options;
    options.format = SampleFormat::PcmInt16;
    options.metadata.title = "On disk";
    REQUIRE(AiffWriter::writeFile(path, source.constView(), kSampleRate44100, ChannelLayout::mono(),
                                  options)
                .ok());

    // Scoped so the reader closes before the file is removed. POSIX unlinks an
    // open file happily; Windows refuses, and std::filesystem::remove without an
    // error_code throws when it does -- failing the test for a reason that has
    // nothing to do with what it is testing. WavTests has carried this same note
    // since before these writers existed.
    {
        auto reader = AiffReader::open(path);
        REQUIRE(reader.hasValue());
        CHECK(reader.value().info().frameCount == 333);
        CHECK(reader.value().metadata().title == "On disk");
        auto restored = reader.value().readAll();
        REQUIRE(restored.hasValue());
        for (SampleCount i = 0; i < 333; ++i) {
            REQUIRE(restored.value().channel(0)[i] == source.channel(0)[i]);
        }
    }

    std::filesystem::remove(path);
}
