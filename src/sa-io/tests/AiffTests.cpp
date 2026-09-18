#include <sa/io/AiffReader.h>
#include <sa/io/PeakPyramid.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::io;
using Catch::Approx;

namespace {

void appendBigU16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void appendBigU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void appendTag(std::vector<std::byte>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>(tag[i]));
    }
}

/// Encode a sample rate as the 80-bit IEEE 754 extended float AIFF requires:
/// 15-bit exponent biased by 16383, then a 64-bit mantissa with an explicit
/// leading integer bit.
void appendExtendedFloat(std::vector<std::byte>& out, double value) {
    if (value == 0.0) {
        out.insert(out.end(), 10, std::byte{0});
        return;
    }
    int exponent = 0;
    const double fraction = std::frexp(value, &exponent); // value = fraction * 2^exponent
    const auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 64));
    appendBigU16(out, static_cast<std::uint16_t>(exponent + 16382));
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((mantissa >> shift) & 0xFFu));
    }
}

struct AiffSpec {
    int channels = 1;
    SampleCount frames = 0;
    int bits = 16;
    double sampleRate = 48000.0;
    const char* compression = nullptr; ///< null for plain AIFF, else AIFC
    std::uint32_t soundOffset = 0;
    std::vector<std::byte> extraChunks;
    /// Override the frame count written into COMM, to test a lying header.
    SampleCount declaredFrames = -1;
};

/// Build an AIFF file around raw sample bytes.
std::vector<std::byte> buildAiff(const AiffSpec& spec, const std::vector<std::byte>& samples) {
    std::vector<std::byte> common;
    appendBigU16(common, static_cast<std::uint16_t>(spec.channels));
    appendBigU32(common, static_cast<std::uint32_t>(spec.declaredFrames >= 0 ? spec.declaredFrames
                                                                             : spec.frames));
    appendBigU16(common, static_cast<std::uint16_t>(spec.bits));
    appendExtendedFloat(common, spec.sampleRate);
    if (spec.compression != nullptr) {
        appendTag(common, spec.compression);
        common.push_back(std::byte{0}); // empty pascal-string description
        common.push_back(std::byte{0});
    }

    std::vector<std::byte> body;
    appendTag(body, spec.compression != nullptr ? "AIFC" : "AIFF");

    appendTag(body, "COMM");
    appendBigU32(body, static_cast<std::uint32_t>(common.size()));
    body.insert(body.end(), common.begin(), common.end());
    if (common.size() % 2 != 0) {
        body.push_back(std::byte{0});
    }

    body.insert(body.end(), spec.extraChunks.begin(), spec.extraChunks.end());

    appendTag(body, "SSND");
    appendBigU32(body, static_cast<std::uint32_t>(8 + spec.soundOffset + samples.size()));
    appendBigU32(body, spec.soundOffset);
    appendBigU32(body, 0); // block size
    body.insert(body.end(), spec.soundOffset, std::byte{0});
    body.insert(body.end(), samples.begin(), samples.end());

    std::vector<std::byte> file;
    appendTag(file, "FORM");
    appendBigU32(file, static_cast<std::uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

/// Encode floats as big-endian (or byte-swapped) integer PCM.
std::vector<std::byte> encodePcm(const std::vector<float>& interleaved, int bits,
                                 bool littleEndian) {
    const int bytes = bits / 8;
    std::vector<std::byte> out;
    out.reserve(interleaved.size() * static_cast<std::size_t>(bytes));

    for (float value : interleaved) {
        const double clamped = std::clamp(static_cast<double>(value), -1.0, 1.0);
        std::int64_t scaled = 0;
        if (bits == 16) {
            scaled = std::clamp<std::int64_t>(std::llround(clamped * 32768.0), -32768, 32767);
        } else if (bits == 24) {
            scaled = std::clamp<std::int64_t>(std::llround(clamped * 8388608.0), -8388608, 8388607);
        } else {
            scaled = std::clamp<std::int64_t>(std::llround(clamped * 2147483648.0), -2147483648LL,
                                              2147483647LL);
        }
        const auto raw = static_cast<std::uint32_t>(static_cast<std::int32_t>(scaled));

        if (littleEndian) {
            for (int i = 0; i < bytes; ++i) {
                out.push_back(static_cast<std::byte>((raw >> (8 * i)) & 0xFFu));
            }
        } else {
            for (int i = bytes - 1; i >= 0; --i) {
                out.push_back(static_cast<std::byte>((raw >> (8 * i)) & 0xFFu));
            }
        }
    }
    return out;
}

std::vector<float> rampSamples(std::size_t count) {
    std::vector<float> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.05)) * 0.7f;
    }
    return values;
}

} // namespace

TEST_CASE("Big-endian PCM round-trips at every supported depth", "[io][aiff]") {
    const auto samples = rampSamples(400);

    for (int bits : {16, 24, 32}) {
        AiffSpec spec;
        spec.channels = 2;
        spec.frames = 200;
        spec.bits = bits;
        const auto file = buildAiff(spec, encodePcm(samples, bits, false));

        auto reader = AiffReader::fromMemory(file);
        INFO("bits " << bits);
        REQUIRE(reader.hasValue());
        CHECK(reader.value().info().channelCount() == 2);
        CHECK(reader.value().info().frameCount == 200);
        CHECK(reader.value().info().sampleRate == kSampleRate48000);

        auto decoded = reader.value().readAll();
        REQUIRE(decoded.hasValue());

        const float tolerance = bits == 16 ? 1.0f / 32768.0f : 1e-5f;
        for (SampleCount frame = 0; frame < 200; ++frame) {
            for (int channel = 0; channel < 2; ++channel) {
                const float expected = samples[static_cast<std::size_t>(frame * 2 + channel)];
                REQUIRE(decoded.value().channel(channel)[frame] ==
                        Approx(expected).margin(tolerance));
            }
        }
    }
}

TEST_CASE("The 80-bit extended sample rate decodes correctly", "[io][aiff]") {
    // Hand-unpacked because no compiler exposes the type portably; getting the
    // bias or the explicit leading bit wrong yields a plausible-looking but
    // wrong rate, which would silently retune every file.
    for (double rate : {8000.0, 22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
        AiffSpec spec;
        spec.frames = 10;
        spec.sampleRate = rate;
        const auto file = buildAiff(spec, encodePcm(rampSamples(10), 16, false));

        auto reader = AiffReader::fromMemory(file);
        INFO("rate " << rate);
        REQUIRE(reader.hasValue());
        CHECK(reader.value().info().sampleRate.hz() == Approx(rate).epsilon(1e-9));
    }
}

TEST_CASE("AIFC sowt is little-endian PCM", "[io][aiff]") {
    // sowt ("twos" reversed) is what macOS actually writes. Reading it as
    // big-endian produces loud noise rather than an obvious failure.
    const auto samples = rampSamples(100);

    AiffSpec spec;
    spec.frames = 100;
    spec.bits = 16;
    spec.compression = "sowt";
    const auto file = buildAiff(spec, encodePcm(samples, 16, true));

    auto reader = AiffReader::fromMemory(file);
    REQUIRE(reader.hasValue());
    auto decoded = reader.value().readAll();
    REQUIRE(decoded.hasValue());

    for (SampleCount i = 0; i < 100; ++i) {
        REQUIRE(decoded.value().channel(0)[i] ==
                Approx(samples[static_cast<std::size_t>(i)]).margin(1.0f / 32768.0f));
    }
}

TEST_CASE("AIFC NONE is big-endian PCM", "[io][aiff]") {
    const auto samples = rampSamples(64);
    AiffSpec spec;
    spec.frames = 64;
    spec.compression = "NONE";
    const auto file = buildAiff(spec, encodePcm(samples, 16, false));

    auto reader = AiffReader::fromMemory(file);
    REQUIRE(reader.hasValue());
    auto decoded = reader.value().readAll();
    REQUIRE(decoded.hasValue());
    CHECK(decoded.value().channel(0)[5] == Approx(samples[5]).margin(1.0f / 32768.0f));
}

TEST_CASE("Genuinely compressed AIFC is rejected, not decoded as noise", "[io][aiff]") {
    AiffSpec spec;
    spec.frames = 64;
    spec.compression = "ima4";
    const auto file = buildAiff(spec, encodePcm(rampSamples(64), 16, false));

    auto reader = AiffReader::fromMemory(file);
    REQUIRE_FALSE(reader.hasValue());
    CHECK(reader.error().code() == ErrorCode::UnsupportedFormat);
}

TEST_CASE("The SSND offset is honoured", "[io][aiff]") {
    // Almost always zero, which is exactly why an implementation that ignores
    // it passes casual testing and then misreads the rare file that sets it.
    const auto samples = rampSamples(50);
    AiffSpec spec;
    spec.frames = 50;
    spec.soundOffset = 16;
    const auto file = buildAiff(spec, encodePcm(samples, 16, false));

    auto reader = AiffReader::fromMemory(file);
    REQUIRE(reader.hasValue());
    auto decoded = reader.value().readAll();
    REQUIRE(decoded.hasValue());
    CHECK(decoded.value().channel(0)[0] == Approx(samples[0]).margin(1.0f / 32768.0f));
    CHECK(decoded.value().channel(0)[20] == Approx(samples[20]).margin(1.0f / 32768.0f));
}

TEST_CASE("A COMM frame count larger than the data is clamped", "[io][aiff]") {
    // AIFF states its frame count rather than implying it from the data length,
    // so the header can claim more than the file carries.
    AiffSpec spec;
    spec.frames = 100;
    spec.declaredFrames = 100000;
    const auto file = buildAiff(spec, encodePcm(rampSamples(100), 16, false));

    auto reader = AiffReader::fromMemory(file);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == 100);

    AudioBuffer destination{ChannelLayout::mono(), 1000};
    auto got = reader.value().read(0, destination.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 100);
}

TEST_CASE("Text metadata is recovered", "[io][aiff]") {
    std::vector<std::byte> extras;
    const auto addText = [&extras](const char* tag, const std::string& text) {
        appendTag(extras, tag);
        appendBigU32(extras, static_cast<std::uint32_t>(text.size()));
        for (char c : text) {
            extras.push_back(static_cast<std::byte>(c));
        }
        if (text.size() % 2 != 0) {
            extras.push_back(std::byte{0});
        }
    };
    addText("NAME", "Field Recording 12");
    addText("AUTH", "Sound Analyser");
    addText("ANNO", "Recorded at dawn");

    AiffSpec spec;
    spec.frames = 32;
    spec.extraChunks = extras;
    const auto file = buildAiff(spec, encodePcm(rampSamples(32), 16, false));

    auto reader = AiffReader::fromMemory(file);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().metadata().title == "Field Recording 12");
    CHECK(reader.value().metadata().artist == "Sound Analyser");
    CHECK(reader.value().metadata().comment == "Recorded at dawn");
}

TEST_CASE("Reads are random-access and clamp at the end", "[io][aiff]") {
    const auto samples = rampSamples(2000);
    AiffSpec spec;
    spec.frames = 2000;
    spec.bits = 24;
    const auto file = buildAiff(spec, encodePcm(samples, 24, false));

    auto reader = AiffReader::fromMemory(file);
    REQUIRE(reader.hasValue());

    AudioBuffer window{ChannelLayout::mono(), 100};
    auto got = reader.value().read(500, window.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 100);
    CHECK(window.channel(0)[0] == Approx(samples[500]).margin(1e-5));

    got = reader.value().read(1950, window.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 50);

    got = reader.value().read(5000, window.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 0);

    CHECK_FALSE(reader.value().read(-1, window.view()).hasValue());
}

TEST_CASE("An AIFF drives the streaming pyramid through AudioSource", "[io][aiff][streaming]") {
    // The point of the abstraction: analysis code never learns which container
    // the audio came from.
    const auto samples = rampSamples(20000);
    AiffSpec spec;
    spec.frames = 20000;
    spec.bits = 24;
    const auto file = buildAiff(spec, encodePcm(samples, 24, false));

    auto reader = AiffReader::fromMemory(file);
    REQUIRE(reader.hasValue());

    auto pyramid = PeakPyramid::buildStreaming(reader.value());
    REQUIRE(pyramid.hasValue());
    CHECK(pyramid.value().sourceFrames() == 20000);

    PeakFrame whole{};
    pyramid.value().query(0, 0, 20000, &whole, 1);
    CHECK(whole.maximum == Approx(0.7f).margin(0.01f));
}

TEST_CASE("Malformed AIFF input is rejected without crashing", "[io][aiff][robustness]") {
    const std::vector<std::byte> empty;
    CHECK_FALSE(AiffReader::fromMemory(empty).hasValue());

    AiffSpec spec;
    spec.frames = 128;
    const auto complete = buildAiff(spec, encodePcm(rampSamples(128), 16, false));

    // Truncation at every offset.
    for (std::size_t length = 0; length < complete.size(); ++length) {
        const std::vector<std::byte> truncated{
            complete.begin(), complete.begin() + static_cast<std::ptrdiff_t>(length)};
        auto reader = AiffReader::fromMemory(truncated);
        if (reader) {
            AudioBuffer destination{ChannelLayout::mono(), 256};
            (void)reader.value().read(0, destination.view());
            (void)reader.value().readAll();
        }
    }

    // Wrong container tags.
    auto notForm = complete;
    notForm[0] = std::byte{'X'};
    CHECK_FALSE(AiffReader::fromMemory(notForm).hasValue());

    auto notAiff = complete;
    notAiff[8] = std::byte{'X'};
    CHECK_FALSE(AiffReader::fromMemory(notAiff).hasValue());

    // Random corruption.
    std::mt19937 rng{31337};
    std::uniform_int_distribution<std::size_t> position{0, complete.size() - 1};
    std::uniform_int_distribution<int> value{0, 255};
    for (int trial = 0; trial < 2000; ++trial) {
        auto corrupted = complete;
        for (int i = 0; i < 1 + (trial % 6); ++i) {
            corrupted[position(rng)] = static_cast<std::byte>(value(rng));
        }
        auto reader = AiffReader::fromMemory(corrupted);
        if (reader) {
            AudioBuffer destination{ChannelLayout::stereo(), 256};
            (void)reader.value().read(0, destination.view());
            (void)reader.value().readAll();
        }
    }
    SUCCEED("malformed AIFF handled without crashing");
}

TEST_CASE("Degenerate AIFF parameters are rejected", "[io][aiff][robustness]") {
    AiffSpec zeroChannels;
    zeroChannels.channels = 0;
    zeroChannels.frames = 10;
    CHECK_FALSE(
        AiffReader::fromMemory(buildAiff(zeroChannels, encodePcm(rampSamples(10), 16, false)))
            .hasValue());

    AiffSpec oddBits;
    oddBits.frames = 10;
    oddBits.bits = 12;
    CHECK_FALSE(AiffReader::fromMemory(buildAiff(oddBits, encodePcm(rampSamples(10), 16, false)))
                    .hasValue());

    AiffSpec zeroRate;
    zeroRate.frames = 10;
    zeroRate.sampleRate = 0.0;
    CHECK_FALSE(AiffReader::fromMemory(buildAiff(zeroRate, encodePcm(rampSamples(10), 16, false)))
                    .hasValue());

    AiffSpec eightBit;
    eightBit.frames = 10;
    eightBit.bits = 8;
    CHECK_FALSE(AiffReader::fromMemory(buildAiff(eightBit, encodePcm(rampSamples(10), 16, false)))
                    .hasValue());
}
