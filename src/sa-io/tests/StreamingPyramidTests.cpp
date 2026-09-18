#include <sa/core/Cancellation.h>
#include <sa/io/PeakPyramid.h>
#include <sa/io/WavReader.h>
#include <sa/io/WavWriter.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
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

AudioBuffer makeNoise(int channels, SampleCount frames, unsigned seed = 99) {
    AudioBuffer buffer{channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo(), frames};
    std::mt19937 rng{seed};
    std::uniform_real_distribution<float> dist{-0.9f, 0.9f};
    for (int channel = 0; channel < channels; ++channel) {
        float* samples = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            samples[i] = dist(rng);
        }
    }
    return buffer;
}

/// Encode to float32 WAV so decoding is lossless and the two build paths can be
/// compared exactly rather than within a quantisation tolerance.
std::vector<std::byte> encodeLossless(const AudioBuffer& buffer) {
    std::ostringstream stream{std::ios::binary};
    WavOptions options;
    options.format = SampleFormat::Float32;
    auto writer = WavWriter::create(stream, kSampleRate48000, buffer.layout(), options);
    REQUIRE(writer.hasValue());
    REQUIRE(writer.value().write(buffer.constView()).ok());
    REQUIRE(writer.value().finish().ok());
    return toBytes(stream.str());
}

/// An AudioSource that returns at most `limit` frames per read, to force the
/// streaming path through short reads and odd block boundaries.
class ChoppySource final : public AudioSource {
public:
    ChoppySource(const AudioSource& inner, SampleCount limit) : inner_(&inner), limit_(limit) {}

    [[nodiscard]] const AudioFileInfo& info() const noexcept override { return inner_->info(); }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        const SampleCount capped = std::min(destination.frames(), limit_);
        return inner_->read(startFrame, destination.subRange(0, capped));
    }

private:
    const AudioSource* inner_;
    SampleCount limit_;
};

} // namespace

TEST_CASE("Streaming and in-memory builds agree exactly", "[io][peaks][streaming]") {
    // The two paths must not be able to drift: if streaming summarised bins
    // differently, waveforms would change appearance depending on how a file
    // happened to be loaded.
    for (SampleCount frames : {1000, 4096, 50000, 131072}) {
        const AudioBuffer source = makeNoise(2, frames);
        const auto bytes = encodeLossless(source);

        auto reader = WavReader::fromMemory(bytes);
        REQUIRE(reader.hasValue());

        auto inMemory = PeakPyramid::build(source.constView());
        auto streamed = PeakPyramid::buildStreaming(reader.value());
        REQUIRE(inMemory.hasValue());
        REQUIRE(streamed.hasValue());

        INFO("frames " << frames);
        REQUIRE(streamed.value().levelCount() == inMemory.value().levelCount());
        REQUIRE(streamed.value().sourceFrames() == inMemory.value().sourceFrames());

        for (int level = 0; level < inMemory.value().levelCount(); ++level) {
            REQUIRE(streamed.value().frameCountAt(level) == inMemory.value().frameCountAt(level));
            for (int channel = 0; channel < 2; ++channel) {
                for (SampleCount f = 0; f < inMemory.value().frameCountAt(level); ++f) {
                    const PeakFrame& expected = inMemory.value().frameAt(level, channel, f);
                    const PeakFrame& actual = streamed.value().frameAt(level, channel, f);
                    REQUIRE(actual.minimum == expected.minimum);
                    REQUIRE(actual.maximum == expected.maximum);
                    REQUIRE(actual.rms == Approx(expected.rms).epsilon(1e-6));
                }
            }
        }
    }
}

TEST_CASE("Short reads do not shift bin boundaries", "[io][peaks][streaming]") {
    // A source that returns fewer frames than asked is normal for a network or
    // decoder-backed read. If the summariser assumed full blocks, bins would
    // slide out of alignment and every level above would be wrong.
    const AudioBuffer source = makeNoise(2, 40000);
    const auto bytes = encodeLossless(source);

    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    auto expected = PeakPyramid::build(source.constView());
    REQUIRE(expected.hasValue());

    for (SampleCount limit : {1, 7, 256, 257, 4096}) {
        const ChoppySource choppy{reader.value(), limit};
        auto streamed = PeakPyramid::buildStreaming(choppy);

        INFO("read limit " << limit);
        REQUIRE(streamed.hasValue());
        REQUIRE(streamed.value().sourceFrames() == expected.value().sourceFrames());

        for (SampleCount f = 0; f < expected.value().frameCountAt(0); ++f) {
            REQUIRE(streamed.value().frameAt(0, 0, f).maximum ==
                    expected.value().frameAt(0, 0, f).maximum);
        }
    }
}

TEST_CASE("Progress is reported monotonically and reaches one", "[io][peaks][streaming]") {
    const AudioBuffer source = makeNoise(1, 200000);
    const auto bytes = encodeLossless(source);
    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    std::vector<double> reported;
    JobMonitor monitor;
    monitor.onProgress = [&](double fraction) { reported.push_back(fraction); };

    auto pyramid =
        PeakPyramid::buildStreaming(reader.value(), PeakPyramid::kDefaultBaseBinSize, monitor);
    REQUIRE(pyramid.hasValue());
    REQUIRE_FALSE(reported.empty());

    for (std::size_t i = 1; i < reported.size(); ++i) {
        REQUIRE(reported[i] >= reported[i - 1]);
    }
    CHECK(reported.front() > 0.0);
    CHECK(reported.back() == Approx(1.0));
}

TEST_CASE("Cancellation stops the build", "[io][peaks][streaming]") {
    // A user who opened the wrong two-hour file should not have to wait for it.
    const AudioBuffer source = makeNoise(2, 500000);
    const auto bytes = encodeLossless(source);
    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    CancellationToken token;
    JobMonitor monitor;
    monitor.cancellation = &token;
    int calls = 0;
    monitor.onProgress = [&](double) {
        if (++calls == 2) {
            token.cancel();
        }
    };

    auto pyramid =
        PeakPyramid::buildStreaming(reader.value(), PeakPyramid::kDefaultBaseBinSize, monitor);
    REQUIRE_FALSE(pyramid.hasValue());
    CHECK(pyramid.error().code() == ErrorCode::Cancelled);
}

TEST_CASE("Cancelling before the first read returns immediately", "[io][peaks][streaming]") {
    const AudioBuffer source = makeNoise(1, 10000);
    const auto bytes = encodeLossless(source);
    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    CancellationToken token;
    token.cancel();
    JobMonitor monitor;
    monitor.cancellation = &token;

    auto pyramid =
        PeakPyramid::buildStreaming(reader.value(), PeakPyramid::kDefaultBaseBinSize, monitor);
    REQUIRE_FALSE(pyramid.hasValue());
    CHECK(pyramid.error().code() == ErrorCode::Cancelled);
}

TEST_CASE("A source that ends early is summarised, not rejected", "[io][peaks][streaming]") {
    // A truncated file is common -- an interrupted transfer, a recorder that
    // lost power. Showing the audio that exists is more useful than refusing
    // the file, so the pyramid describes what was actually read.
    const AudioBuffer source = makeNoise(1, 20000);
    auto bytes = encodeLossless(source);
    bytes.resize(bytes.size() / 2);

    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    auto pyramid = PeakPyramid::buildStreaming(reader.value());
    REQUIRE(pyramid.hasValue());
    CHECK(pyramid.value().sourceFrames() > 0);
    CHECK(pyramid.value().sourceFrames() <= 20000);
    CHECK_FALSE(pyramid.value().isEmpty());
}

TEST_CASE("Streaming build validates its bin size", "[io][peaks][streaming]") {
    const AudioBuffer source = makeNoise(1, 1000);
    const auto bytes = encodeLossless(source);
    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    CHECK_FALSE(PeakPyramid::buildStreaming(reader.value(), 100).hasValue());
    CHECK_FALSE(PeakPyramid::buildStreaming(reader.value(), 0).hasValue());
    CHECK(PeakPyramid::buildStreaming(reader.value(), 512).hasValue());
}

TEST_CASE("An empty source streams to an empty pyramid", "[io][peaks][streaming]") {
    const AudioBuffer empty{ChannelLayout::mono(), 0};
    const auto bytes = encodeLossless(empty);
    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());

    auto pyramid = PeakPyramid::buildStreaming(reader.value());
    REQUIRE(pyramid.hasValue());
    CHECK(pyramid.value().isEmpty());
}

TEST_CASE("A transient survives the streaming path too", "[io][peaks][streaming]") {
    AudioBuffer source{ChannelLayout::mono(), 300000};
    source.channel(0)[150001] = 0.95f;
    const auto bytes = encodeLossless(source);

    auto reader = WavReader::fromMemory(bytes);
    REQUIRE(reader.hasValue());
    auto pyramid = PeakPyramid::buildStreaming(reader.value());
    REQUIRE(pyramid.hasValue());

    PeakFrame whole{};
    pyramid.value().query(0, 0, 300000, &whole, 1);
    CHECK(whole.maximum == Approx(0.95f).epsilon(1e-6));
}
