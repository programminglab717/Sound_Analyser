#include <sa/io/AudioFile.h>
#include <sa/io/FlacReader.h>
#include <sa/io/FlacWriter.h>
#include <sa/io/WavWriter.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <numbers>
#include <random>
#include <span>
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

ChannelLayout layoutFor(int channels) {
    return channels == 1   ? ChannelLayout::mono()
           : channels == 2 ? ChannelLayout::stereo()
                           : ChannelLayout::discrete(channels);
}

std::vector<std::byte> encode(ConstAudioBufferView frames, SampleRate rate, ChannelLayout layout,
                              FlacOptions options = {}) {
    std::ostringstream stream{std::ios::binary};
    auto writer = FlacWriter::create(stream, rate, layout, std::move(options));
    REQUIRE(writer.hasValue());
    REQUIRE(writer.value().write(frames).ok());
    REQUIRE(writer.value().finish().ok());
    return toBytes(stream.str());
}

/// Assert that every sample came back exactly as it went in.
///
/// FLAC is lossless, so this is equality and not a tolerance. A margin here
/// would hide precisely the defect the test exists to catch: a predictor or a
/// Rice parameter that is nearly right puts a handful of samples out by a code,
/// which no tolerance worth having would notice and which is not lossless.
void requireIdentical(const AudioBuffer& restored, const AudioBuffer& source) {
    REQUIRE(restored.frames() == source.frames());
    REQUIRE(restored.channelCount() == source.channelCount());
    for (int channel = 0; channel < source.channelCount(); ++channel) {
        for (SampleCount i = 0; i < source.frames(); ++i) {
            INFO("channel " << channel << " frame " << i);
            REQUIRE(restored.channel(channel)[i] == source.channel(channel)[i]);
        }
    }
}

/// A buffer whose samples land exactly on codes at `bits`, so a lossless round
/// trip is bit-identical rather than merely close.
AudioBuffer exactCodes(int channels, SampleCount frames, int bits,
                       const std::function<double(int, SampleCount)>& shape) {
    AudioBuffer buffer{layoutFor(channels), frames};
    const auto scale = static_cast<double>(std::int64_t{1} << (bits - 1));
    const auto limit = static_cast<std::int64_t>(scale);
    for (int channel = 0; channel < channels; ++channel) {
        float* samples = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            const auto code =
                std::clamp(static_cast<std::int64_t>(std::llround(shape(channel, i) * scale)),
                           -limit, limit - 1);
            samples[i] = static_cast<float>(static_cast<double>(code) / scale);
        }
    }
    return buffer;
}

AudioBuffer noiseBuffer(int channels, SampleCount frames, int bits, unsigned seed = 11u) {
    std::mt19937 generator{seed};
    std::uniform_real_distribution<double> values{-1.0, 1.0};
    return exactCodes(channels, frames, bits, [&](int, SampleCount) { return values(generator); });
}

AudioBuffer toneBuffer(int channels, SampleCount frames, int bits) {
    return exactCodes(channels, frames, bits, [](int channel, SampleCount i) {
        return 0.7 *
               std::sin(2.0 * std::numbers::pi * (1.0 + channel) * static_cast<double>(i) / 97.0);
    });
}

/// Decode with FlacReader, which is dr_flac -- an implementation with nothing
/// in common with the encoder under test. A round trip through our own decoder
/// would agree with our own mistakes.
AudioBuffer decode(const std::vector<std::byte>& bytes, SampleCount expectedFrames,
                   int expectedChannels, SampleRate expectedRate) {
    auto reader = FlacReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == expectedFrames);
    CHECK(reader.value().info().channelCount() == expectedChannels);
    CHECK(reader.value().info().sampleRate == expectedRate);
    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());
    return std::move(restored).value();
}

} // namespace

TEST_CASE("A FLAC round trip is bit-identical at 16 and 24 bits", "[io][flac][writer]") {
    for (const int bits : {16, 24}) {
        const SampleFormat format = bits == 16 ? SampleFormat::PcmInt16 : SampleFormat::PcmInt24;

        for (const int channels : {1, 2, 5}) {
            for (const bool tonal : {true, false}) {
                const AudioBuffer source =
                    tonal ? toneBuffer(channels, 3000, bits) : noiseBuffer(channels, 3000, bits);
                FlacOptions options;
                options.format = format;

                const auto bytes =
                    encode(source.constView(), kSampleRate48000, layoutFor(channels), options);
                INFO("bits " << bits << " channels " << channels << " tonal " << tonal);
                const AudioBuffer restored = decode(bytes, 3000, channels, kSampleRate48000);
                requireIdentical(restored, source);
            }
        }
    }
}

TEST_CASE("Signals that break a predictor still round-trip exactly", "[io][flac][writer]") {
    struct Case {
        const char* name;
        std::function<double(int, SampleCount)> shape;
    };

    const Case cases[] = {
        {"silence", [](int, SampleCount) { return 0.0; }},
        {"a constant that is not silence", [](int, SampleCount) { return 0.25; }},
        {"full scale both ways", [](int, SampleCount i) { return (i % 2) == 0 ? 1.0 : -1.0; }},
        {"a single impulse in silence", [](int, SampleCount i) { return i == 1234 ? -1.0 : 0.0; }},
        {"a ramp through zero",
         [](int, SampleCount i) { return static_cast<double>(i - 1000) / 1000.0; }},
        {"alternating extremes of the code range",
         [](int, SampleCount i) { return (i % 3) == 0 ? 1.0 : ((i % 3) == 1 ? -1.0 : 0.0); }},
    };

    for (const Case& test : cases) {
        const AudioBuffer source = exactCodes(2, 2000, 24, test.shape);
        FlacOptions options;
        options.format = SampleFormat::PcmInt24;
        const auto bytes =
            encode(source.constView(), kSampleRate48000, ChannelLayout::stereo(), options);
        INFO(test.name);
        requireIdentical(decode(bytes, 2000, 2, kSampleRate48000), source);
    }
}

TEST_CASE("A FLAC of many blocks reassembles exactly", "[io][flac][writer]") {
    // A small block size on purpose, so that a short buffer becomes many frames
    // and the frame numbering, the CRCs and the partial final block are all
    // exercised. A partial final block is the usual case in a real file and the
    // usual place an encoder goes wrong.
    for (const SampleCount frames :
         {SampleCount{16}, SampleCount{63}, SampleCount{64}, SampleCount{65}, SampleCount{1000}}) {
        const AudioBuffer source = noiseBuffer(2, frames, 16);
        FlacOptions options;
        options.format = SampleFormat::PcmInt16;
        options.blockFrames = 16;

        const auto bytes =
            encode(source.constView(), kSampleRate44100, ChannelLayout::stereo(), options);
        INFO("frames " << frames);
        requireIdentical(decode(bytes, frames, 2, kSampleRate44100), source);
    }
}

TEST_CASE("Streamed FLAC writes in uneven blocks reassemble exactly", "[io][flac][writer]") {
    const AudioBuffer source = toneBuffer(2, 5000, 24);

    std::ostringstream stream{std::ios::binary};
    FlacOptions options;
    options.format = SampleFormat::PcmInt24;
    options.blockFrames = 512;
    auto writer = FlacWriter::create(stream, kSampleRate48000, ChannelLayout::stereo(), options);
    REQUIRE(writer.hasValue());

    SampleCount cursor = 0;
    for (const SampleCount span : {SampleCount{1}, SampleCount{511}, SampleCount{512},
                                   SampleCount{513}, SampleCount{2463}, SampleCount{1000}}) {
        REQUIRE(writer.value().write(source.constView().subRange(cursor, span)).ok());
        cursor += span;
    }
    REQUIRE(cursor == 5000);
    REQUIRE(writer.value().finish().ok());
    CHECK(writer.value().framesWritten() == 5000);

    // Named, not a temporary: fromMemory does not copy, so a reader built over
    // the result of an expression can outlive the bytes it is reading.
    const auto bytes = toBytes(stream.str());
    requireIdentical(decode(bytes, 5000, 2, kSampleRate48000), source);
}

TEST_CASE("Channel counts round-trip, mono to the eight FLAC allows", "[io][flac][writer]") {
    for (const int channels : {1, 2, 3, 4, 5, 6, 7, 8}) {
        const AudioBuffer source = noiseBuffer(channels, 700, 16);
        FlacOptions options;
        options.format = SampleFormat::PcmInt16;
        const auto bytes =
            encode(source.constView(), kSampleRate48000, layoutFor(channels), options);
        INFO("channels " << channels);
        requireIdentical(decode(bytes, 700, channels, kSampleRate48000), source);
    }
}

TEST_CASE("Sample rates survive the round trip", "[io][flac][writer]") {
    // The coded rates, the kHz escape, the plain-hertz escape and the
    // tens-of-hertz escape, because each is a different branch of the frame
    // header and only one of them is the common case.
    for (const double hz : {8000.0, 16000.0, 22050.0, 24000.0, 32000.0, 44100.0, 48000.0, 88200.0,
                            96000.0, 176400.0, 192000.0, 11025.0, 37000.0, 64000.0, 705600.0}) {
        const AudioBuffer source = noiseBuffer(1, 200, 16);
        FlacOptions options;
        options.format = SampleFormat::PcmInt16;
        const auto bytes =
            encode(source.constView(), SampleRate{hz}, ChannelLayout::mono(), options);
        INFO("rate " << hz);
        requireIdentical(decode(bytes, 200, 1, SampleRate{hz}), source);
    }
}

TEST_CASE("FLAC metadata survives the round trip", "[io][flac][writer][metadata]") {
    const AudioBuffer source = toneBuffer(2, 400, 16);
    FlacOptions options;
    options.format = SampleFormat::PcmInt16;
    options.metadata.title = "Rehearsal take 4";
    options.metadata.artist = "The Quartet";
    options.metadata.comment = "Second desk, slightly flat";
    options.metadata.date = "2026-09-19";
    options.metadata.software = "Auscultate";

    const auto bytes =
        encode(source.constView(), kSampleRate44100, ChannelLayout::stereo(), options);
    auto reader = FlacReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    CHECK(reader.value().metadata().title == "Rehearsal take 4");
    CHECK(reader.value().metadata().artist == "The Quartet");
    CHECK(reader.value().metadata().comment == "Second desk, slightly flat");
    CHECK(reader.value().metadata().date == "2026-09-19");
    CHECK(reader.value().metadata().software == "Auscultate");

    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());
    requireIdentical(restored.value(), source);
}

TEST_CASE("A FLAC with no metadata still opens", "[io][flac][writer][metadata]") {
    const AudioBuffer source = toneBuffer(1, 200, 16);
    FlacOptions options;
    options.format = SampleFormat::PcmInt16;
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), options);

    auto reader = FlacReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().metadata().title.empty());
    CHECK(reader.value().metadata().artist.empty());
}

TEST_CASE("The FLAC this writes is smaller than the WAV it replaces", "[io][flac][writer]") {
    // The point of the format. Tonal material at 24 bits is where the fixed
    // predictors earn their keep; the bound is loose on purpose, because the
    // claim is "materially smaller", not a particular ratio.
    const AudioBuffer source = toneBuffer(2, 20000, 24);

    FlacOptions flacOptions;
    flacOptions.format = SampleFormat::PcmInt24;
    const auto flac =
        encode(source.constView(), kSampleRate48000, ChannelLayout::stereo(), flacOptions);

    std::ostringstream wavStream{std::ios::binary};
    WavOptions wavOptions;
    wavOptions.format = SampleFormat::PcmInt24;
    auto wavWriter =
        WavWriter::create(wavStream, kSampleRate48000, ChannelLayout::stereo(), wavOptions);
    REQUIRE(wavWriter.hasValue());
    REQUIRE(wavWriter.value().write(source.constView()).ok());
    REQUIRE(wavWriter.value().finish().ok());
    const auto wav = toBytes(wavStream.str());

    CHECK(flac.size() < wav.size() / 2);
}

TEST_CASE("Sixteen-bit audio written as a 24-bit FLAC costs almost nothing extra",
          "[io][flac][writer]") {
    // Every sample has eight trailing zeroes, and FLAC has a field that says
    // so. Without it the file would be about half as big again for no
    // information at all.
    const AudioBuffer source = toneBuffer(1, 8000, 16);

    FlacOptions narrow;
    narrow.format = SampleFormat::PcmInt16;
    const auto atSixteen =
        encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), narrow);

    FlacOptions wide;
    wide.format = SampleFormat::PcmInt24;
    const auto atTwentyFour =
        encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), wide);

    CHECK(atTwentyFour.size() < atSixteen.size() + atSixteen.size() / 10);

    // And it still comes back exactly, which is the part that matters.
    requireIdentical(decode(atTwentyFour, 8000, 1, kSampleRate48000), source);
}

TEST_CASE("The writer's output is recognised as FLAC by the sniffer", "[io][flac][writer]") {
    const AudioBuffer source = toneBuffer(2, 500, 16);
    FlacOptions options;
    options.format = SampleFormat::PcmInt16;
    options.metadata.title = "Sniffable";
    const auto bytes =
        encode(source.constView(), kSampleRate48000, ChannelLayout::stereo(), options);

    CHECK(detectFormat(std::span<const std::byte>{bytes}) == ContainerFormat::Flac);

    const auto path = std::filesystem::temp_directory_path() / "sa-flac-writer-sniff.flac";
    std::filesystem::remove(path);
    REQUIRE(FlacWriter::writeFile(path, source.constView(), kSampleRate48000,
                                  ChannelLayout::stereo(), options)
                .ok());

    auto opened = openAudioFile(path);
    REQUIRE(opened.hasValue());
    CHECK(opened.value()->info().frameCount == 500);
    CHECK(opened.value()->info().channelCount() == 2);

    auto reader = FlacReader::open(path);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().metadata().title == "Sniffable");
    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());
    requireIdentical(restored.value(), source);

    std::filesystem::remove(path);
}

TEST_CASE("An empty FLAC is valid and reads as zero frames", "[io][flac][writer]") {
    std::ostringstream stream{std::ios::binary};
    auto writer = FlacWriter::create(stream, kSampleRate48000, ChannelLayout::stereo());
    REQUIRE(writer.hasValue());
    REQUIRE(writer.value().finish().ok());

    const auto bytes = toBytes(stream.str());
    auto reader = FlacReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == 0);
    CHECK(reader.value().info().channelCount() == 2);
}

TEST_CASE("The FLAC writer refuses what FLAC cannot hold", "[io][flac][writer]") {
    std::ostringstream stream{std::ios::binary};

    for (const SampleFormat format :
         {SampleFormat::Float32, SampleFormat::Float64, SampleFormat::PcmUInt8,
          SampleFormat::PcmInt32, SampleFormat::Unknown}) {
        FlacOptions options;
        options.format = format;
        auto writer = FlacWriter::create(stream, kSampleRate48000, ChannelLayout::mono(), options);
        INFO("format " << toString(format));
        CHECK_FALSE(writer.hasValue());
        CHECK(writer.error().code() == ErrorCode::InvalidArgument);
    }

    // Nine channels: our buffers carry up to 64, FLAC's channel field carries
    // eight, and silently dropping one would be worse than refusing.
    CHECK_FALSE(
        FlacWriter::create(stream, kSampleRate48000, ChannelLayout::discrete(9)).hasValue());
    CHECK_FALSE(FlacWriter::create(stream, SampleRate{0.0}, ChannelLayout::mono()).hasValue());
    CHECK_FALSE(FlacWriter::create(stream, kSampleRate48000, ChannelLayout{}).hasValue());

    FlacOptions tiny;
    tiny.blockFrames = 8;
    CHECK_FALSE(
        FlacWriter::create(stream, kSampleRate48000, ChannelLayout::mono(), tiny).hasValue());
}

TEST_CASE("Out-of-range samples clamp instead of wrapping", "[io][flac][writer]") {
    AudioBuffer source{ChannelLayout::mono(), 4};
    const float values[] = {2.0f, -2.0f, 1.0f, -1.0f};
    for (SampleCount i = 0; i < 4; ++i) {
        source.channel(0)[i] = values[i];
    }

    FlacOptions options;
    options.format = SampleFormat::PcmInt16;
    const auto bytes = encode(source.constView(), kSampleRate48000, ChannelLayout::mono(), options);
    const AudioBuffer restored = decode(bytes, 4, 1, kSampleRate48000);

    CHECK(restored.channel(0)[0] == 32767.0f / 32768.0f);
    CHECK(restored.channel(0)[1] == -1.0f);
    CHECK(restored.channel(0)[2] == 32767.0f / 32768.0f);
    CHECK(restored.channel(0)[3] == -1.0f);
}

TEST_CASE("A FLAC written to a path round-trips through the file system", "[io][flac][writer]") {
    const auto path = std::filesystem::temp_directory_path() / "sa-flac-writer-test.flac";
    std::filesystem::remove(path);

    const AudioBuffer source = noiseBuffer(2, 4097, 24);
    FlacOptions options;
    options.format = SampleFormat::PcmInt24;
    REQUIRE(FlacWriter::writeFile(path, source.constView(), kSampleRate96000,
                                  ChannelLayout::stereo(), options)
                .ok());

    auto reader = FlacReader::open(path);
    REQUIRE(reader.hasValue());
    CHECK(reader.value().info().frameCount == 4097);
    CHECK(reader.value().info().sampleRate == kSampleRate96000);
    auto restored = reader.value().readAll();
    REQUIRE(restored.hasValue());
    requireIdentical(restored.value(), source);

    std::filesystem::remove(path);
}

TEST_CASE("Random access into a written FLAC lands on the right samples", "[io][flac][writer]") {
    // Seeking is where a wrong frame number or a wrong declared block size
    // shows up, and neither shows up in a sequential read.
    const AudioBuffer source = noiseBuffer(2, 9000, 16);
    FlacOptions options;
    options.format = SampleFormat::PcmInt16;
    options.blockFrames = 1024;
    const auto bytes =
        encode(source.constView(), kSampleRate48000, ChannelLayout::stereo(), options);

    auto reader = FlacReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    for (const SampleIndex start : {SampleIndex{0}, SampleIndex{1}, SampleIndex{1023},
                                    SampleIndex{1024}, SampleIndex{5000}, SampleIndex{8999}}) {
        AudioBuffer block{ChannelLayout::stereo(), 64};
        auto read = reader.value().read(start, block.view());
        REQUIRE(read.hasValue());
        const SampleCount got = read.value();
        REQUIRE(got > 0);
        for (int channel = 0; channel < 2; ++channel) {
            for (SampleCount i = 0; i < got; ++i) {
                INFO("start " << start << " channel " << channel << " offset " << i);
                REQUIRE(block.channel(channel)[i] == source.channel(channel)[start + i]);
            }
        }
    }
}

TEST_CASE("A randomised sweep of shapes stays lossless", "[io][flac][writer]") {
    // The spot checks above cover the cases somebody thought of. This walks a
    // grid of the things that change which branch of the encoder runs -- how
    // many channels, how deep, how the length falls against the block size, how
    // loud, and how many trailing zeroes the samples share -- so that a case
    // nobody thought of has somewhere to turn up.
    //
    // The seed is fixed, so a failure is a thing that can be reproduced rather
    // than a story about last Tuesday.
    std::mt19937 generator{20260919u};

    for (int trial = 0; trial < 40; ++trial) {
        const int channels = 1 + static_cast<int>(generator() % 8u);
        const int bits = (generator() % 2u) == 0 ? 16 : 24;
        const auto frames = static_cast<SampleCount>(1 + generator() % 700u);
        const SampleCount blockFrames = 16 + static_cast<SampleCount>(generator() % 200u);
        // Level in steps of 6 dB, down to the bottom of the range: quiet
        // material picks small Rice parameters, loud material picks large ones.
        const int attenuation = static_cast<int>(generator() % 20u);
        // Trailing zeroes, which is what wasted-bit coding exists for: a 16-bit
        // recording stored at 24 bits has eight of them in every sample.
        const int granularity = static_cast<int>(generator() % 9u);

        const auto scale = static_cast<double>(std::int64_t{1} << (bits - 1));
        const std::int64_t step = std::int64_t{1} << granularity;
        std::uniform_real_distribution<double> values{-1.0, 1.0};

        AudioBuffer source{layoutFor(channels), frames};
        for (int channel = 0; channel < channels; ++channel) {
            float* samples = source.channel(channel);
            for (SampleCount i = 0; i < frames; ++i) {
                const double wanted = std::ldexp(values(generator), -attenuation) * scale;
                const std::int64_t code = std::clamp(
                    static_cast<std::int64_t>(std::llround(wanted / static_cast<double>(step))) *
                        step,
                    -static_cast<std::int64_t>(scale), static_cast<std::int64_t>(scale) - 1);
                samples[i] = static_cast<float>(static_cast<double>(code) / scale);
            }
        }

        FlacOptions options;
        options.format = bits == 16 ? SampleFormat::PcmInt16 : SampleFormat::PcmInt24;
        options.blockFrames = blockFrames;

        const auto bytes =
            encode(source.constView(), kSampleRate44100, layoutFor(channels), options);
        INFO("trial " << trial << ": " << channels << " channels, " << bits << " bits, " << frames
                      << " frames, block " << blockFrames << ", -" << 6 * attenuation
                      << " dB, step " << step);
        requireIdentical(decode(bytes, frames, channels, kSampleRate44100), source);
    }
}
