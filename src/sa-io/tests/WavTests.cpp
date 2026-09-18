#include <sa/io/WavReader.h>
#include <sa/io/WavWriter.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <span>
#include <sstream>
#include <vector>

using namespace sa;
using namespace sa::io;
using Catch::Approx;

namespace {

std::vector<std::byte> toBytes(const std::string& text) {
    std::vector<std::byte> bytes(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return bytes;
}

AudioBuffer makeSignal(int channels, SampleCount frames) {
    AudioBuffer buffer{channels == 1   ? ChannelLayout::mono()
                       : channels == 2 ? ChannelLayout::stereo()
                                       : ChannelLayout::discrete(channels),
                       frames};
    for (int channel = 0; channel < channels; ++channel) {
        float* samples = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            samples[i] =
                0.8f * static_cast<float>(std::sin(2.0 * std::numbers::pi * (1.0 + channel) *
                                                   static_cast<double>(i) / 64.0));
        }
    }
    return buffer;
}

/// Encode a buffer to WAV bytes in memory.
std::vector<std::byte> encode(ConstAudioBufferView frames, SampleRate rate, ChannelLayout layout,
                              WavOptions options = {}) {
    std::ostringstream stream{std::ios::binary};
    auto writer = WavWriter::create(stream, rate, layout, std::move(options));
    REQUIRE(writer.hasValue());
    REQUIRE(writer.value().write(frames).ok());
    REQUIRE(writer.value().finish().ok());
    return toBytes(stream.str());
}

} // namespace

TEST_CASE("Round-trip through every supported sample format", "[io][wav]") {
    struct Case {
        SampleFormat format;
        float tolerance; ///< half a least-significant bit of the target depth
    };

    const AudioBuffer source = makeSignal(2, 1000);

    for (const Case& test :
         {Case{SampleFormat::PcmUInt8, 1.0f / 256.0f},
          Case{SampleFormat::PcmInt16, 1.0f / 65536.0f},
          Case{SampleFormat::PcmInt24, 1.0f / 16777216.0f}, Case{SampleFormat::PcmInt32, 1e-6f},
          Case{SampleFormat::Float32, 1e-9f}, Case{SampleFormat::Float64, 1e-9f}}) {
        WavOptions options;
        options.format = test.format;
        const auto bytes =
            encode(source.constView(), kSampleRate48000, ChannelLayout::stereo(), options);

        auto reader = WavReader::fromMemory(bytes);
        INFO("format " << toString(test.format));
        REQUIRE(reader.hasValue());

        const AudioFileInfo& info = reader.value().info();
        CHECK(info.frameCount == 1000);
        CHECK(info.channelCount() == 2);
        CHECK(info.sampleRate == kSampleRate48000);
        CHECK(info.format == test.format);

        auto restored = reader.value().readAll();
        REQUIRE(restored.hasValue());

        for (int channel = 0; channel < 2; ++channel) {
            for (SampleCount i = 0; i < 1000; ++i) {
                REQUIRE(restored.value().channel(channel)[i] ==
                        Approx(source.channel(channel)[i]).margin(test.tolerance));
            }
        }
    }
}

TEST_CASE("Full-scale and silence survive a round-trip", "[io][wav]") {
    AudioBuffer source{ChannelLayout::mono(), 6};
    const float values[] = {0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.0f};
    for (SampleCount i = 0; i < 6; ++i) {
        source.channel(0)[i] = values[i];
    }

    WavOptions options;
    options.format = SampleFormat::PcmInt24;
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), options);

    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());

    for (SampleCount i = 0; i < 6; ++i) {
        INFO("sample " << i);
        CHECK(restored.value().channel(0)[i] == Approx(values[i]).margin(1e-6));
    }
}

TEST_CASE("Out-of-range samples clamp instead of wrapping", "[io][wav]") {
    // Wrapping would flip polarity and sound like a click rather than the
    // clipping the user expects.
    AudioBuffer source{ChannelLayout::mono(), 4};
    source.channel(0)[0] = 2.5f;
    source.channel(0)[1] = -2.5f;
    source.channel(0)[2] = 1.0001f;
    source.channel(0)[3] = -1.0001f;

    WavOptions options;
    options.format = SampleFormat::PcmInt16;
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), options);

    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());

    CHECK(restored.value().channel(0)[0] == Approx(1.0f).margin(1e-3));
    CHECK(restored.value().channel(0)[1] == Approx(-1.0f).margin(1e-3));
    CHECK(restored.value().channel(0)[2] == Approx(1.0f).margin(1e-3));
    CHECK(restored.value().channel(0)[3] == Approx(-1.0f).margin(1e-3));
}

TEST_CASE("Float export preserves values past full scale", "[io][wav]") {
    // A float export exists precisely to keep headroom for later processing, so
    // clipping on the way out would destroy the reason to choose it. Clipping
    // is the user's decision, made later.
    AudioBuffer source{ChannelLayout::mono(), 3};
    source.channel(0)[0] = 2.5f;
    source.channel(0)[1] = -1.75f;
    source.channel(0)[2] = 0.5f;

    WavOptions options;
    options.format = SampleFormat::Float32;
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), options);

    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());

    CHECK(restored.value().channel(0)[0] == Approx(2.5f));
    CHECK(restored.value().channel(0)[1] == Approx(-1.75f));
    CHECK(restored.value().channel(0)[2] == Approx(0.5f));
}

TEST_CASE("Channel counts round-trip", "[io][wav]") {
    for (int channels : {1, 2, 6, 8, 16}) {
        const AudioBuffer source = makeSignal(channels, 200);
        const auto bytes = encode(source.constView(), kSampleRate44100, source.layout());

        auto reader = WavReader::fromMemory(bytes);
        INFO("channels " << channels);
        REQUIRE(reader.hasValue());
        CHECK(reader.value().info().channelCount() == channels);
        CHECK(reader.value().info().frameCount == 200);
    }
}

TEST_CASE("Reading is random-access and streamable", "[io][wav]") {
    const AudioBuffer source = makeSignal(2, 5000);
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::stereo());

    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    // Read the middle without touching the rest -- the whole point of streaming.
    AudioBuffer window{ChannelLayout::stereo(), 100};
    auto got = reader.value().read(2000, window.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 100);

    for (SampleCount i = 0; i < 100; ++i) {
        REQUIRE(window.channel(0)[i] == Approx(source.channel(0)[2000 + i]).margin(1e-4));
    }
}

TEST_CASE("Reads clamp at end of file", "[io][wav]") {
    const AudioBuffer source = makeSignal(1, 100);
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono());

    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    AudioBuffer destination{ChannelLayout::mono(), 500};
    auto got = reader.value().read(50, destination.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 50);

    got = reader.value().read(100, destination.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 0);

    got = reader.value().read(99999, destination.view());
    REQUIRE(got.hasValue());
    CHECK(got.value() == 0);

    CHECK_FALSE(reader.value().read(-1, destination.view()).hasValue());
}

TEST_CASE("Sequential block reads reassemble the file", "[io][wav]") {
    const AudioBuffer source = makeSignal(2, 4321);
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::stereo());

    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    AudioBuffer assembled{ChannelLayout::stereo(), 4321};
    AudioBuffer block{ChannelLayout::stereo(), 512};

    SampleIndex position = 0;
    while (position < 4321) {
        auto got = reader.value().read(position, block.view());
        REQUIRE(got.hasValue());
        if (got.value() == 0) {
            break;
        }
        for (int channel = 0; channel < 2; ++channel) {
            std::copy_n(block.channel(channel), got.value(), assembled.channel(channel) + position);
        }
        position += got.value();
    }

    CHECK(position == 4321);
    for (int channel = 0; channel < 2; ++channel) {
        for (SampleCount i = 0; i < 4321; ++i) {
            REQUIRE(assembled.channel(channel)[i] ==
                    Approx(source.channel(channel)[i]).margin(1e-4));
        }
    }
}

TEST_CASE("Broadcast Wave timecode survives a round-trip", "[io][wav][metadata]") {
    // Losing bext timeReference makes a file useless to the next person in a
    // post-production chain -- it is the reference that lines audio up to picture.
    const AudioBuffer source = makeSignal(1, 64);

    WavOptions options;
    options.metadata.broadcast.present = true;
    options.metadata.broadcast.description = "Scene 7 Take 3";
    options.metadata.broadcast.originator = "Sound Analyser";
    options.metadata.broadcast.originatorReference = "SA0001";
    options.metadata.broadcast.originationDate = "2026-09-18";
    options.metadata.broadcast.originationTime = "14:30:00";
    options.metadata.broadcast.timeReference = 2540160000ULL;

    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), options);

    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    const BroadcastInfo& broadcast = reader.value().metadata().broadcast;
    CHECK(broadcast.present);
    CHECK(broadcast.description == "Scene 7 Take 3");
    CHECK(broadcast.originator == "Sound Analyser");
    CHECK(broadcast.originatorReference == "SA0001");
    CHECK(broadcast.originationDate == "2026-09-18");
    CHECK(broadcast.originationTime == "14:30:00");
    CHECK(broadcast.timeReference == 2540160000ULL);

    // Audio must still decode correctly with a chunk between fmt and data.
    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());
    CHECK(restored.value().frames() == 64);
    CHECK(restored.value().channel(0)[10] == Approx(source.channel(0)[10]).margin(1e-4));
}

TEST_CASE("Writer rejects invalid configurations", "[io][wav]") {
    std::ostringstream stream{std::ios::binary};
    CHECK_FALSE(WavWriter::create(stream, SampleRate{0.0}, ChannelLayout::stereo()).hasValue());
    CHECK_FALSE(WavWriter::create(stream, kSampleRate48000, ChannelLayout::discrete(0)).hasValue());

    WavOptions bad;
    bad.format = SampleFormat::Unknown;
    CHECK_FALSE(WavWriter::create(stream, kSampleRate48000, ChannelLayout::mono(), bad).hasValue());
}

TEST_CASE("An empty file is valid and reads as zero frames", "[io][wav]") {
    const AudioBuffer empty{ChannelLayout::mono(), 0};
    std::ostringstream stream{std::ios::binary};
    auto writer = WavWriter::create(stream, kSampleRate48000, ChannelLayout::mono());
    REQUIRE(writer.hasValue());
    REQUIRE(writer.value().finish().ok());

    const auto bytes = toBytes(stream.str());
    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == 0);

    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());
    CHECK(restored.value().isEmpty());
}

TEST_CASE("Duration is reported from the frame count", "[io][wav]") {
    const AudioBuffer source = makeSignal(1, 48000);
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono());
    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().durationSeconds() == Approx(1.0));
}

TEST_CASE("Writing to a file and reading it back works", "[io][wav]") {
    const auto path = std::filesystem::temp_directory_path() / "sa-wav-roundtrip.wav";
    const AudioBuffer source = makeSignal(2, 777);

    REQUIRE(
        WavWriter::writeFile(path, source.constView(), kSampleRate44100, ChannelLayout::stereo())
            .ok());

    // The reader is scoped so it closes before the file is removed. POSIX
    // happily unlinks an open file; Windows refuses, and that difference is a
    // real property of the reader rather than a quirk of this test -- see the
    // note on WavReader about holding the handle open.
    {
        auto reader = WavReader::open(path);
        REQUIRE(reader.hasValue());
        CHECK(reader.value().info().frameCount == 777);
        CHECK(reader.value().info().sampleRate == kSampleRate44100);

        auto restored = reader.value().readAll();
        REQUIRE(restored.hasValue());
        CHECK(restored.value().channel(1)[500] == Approx(source.channel(1)[500]).margin(1e-4));
    }

    std::filesystem::remove(path);
}

TEST_CASE("Opening a missing file fails cleanly", "[io][wav]") {
    auto reader = WavReader::open("/nonexistent/path/does-not-exist.wav");
    REQUIRE_FALSE(reader.hasValue());
    CHECK(reader.error().code() == ErrorCode::IoFailure);
}
