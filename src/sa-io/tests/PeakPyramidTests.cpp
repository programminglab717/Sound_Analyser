#include <sa/core/RealtimeGuard.h>
#include <sa/io/PeakPyramid.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace sa;
using Catch::Approx;

namespace {

/// Ground truth, computed straight from the source. Every pyramid assertion
/// below is checked against this rather than against another pyramid value --
/// otherwise a consistent-but-wrong aggregation would pass.
struct Reference {
    float minimum = 0.0f;
    float maximum = 0.0f;
    float rms = 0.0f;
};

Reference bruteForce(const AudioBuffer& buffer, int channel, SampleIndex start, SampleIndex end) {
    start = std::max<SampleIndex>(0, start);
    end = std::min<SampleIndex>(buffer.frames(), end);

    Reference result;
    if (end <= start) {
        return result;
    }

    const float* samples = buffer.channel(channel);
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    double sumOfSquares = 0.0;

    for (SampleIndex i = start; i < end; ++i) {
        minimum = std::min(minimum, samples[i]);
        maximum = std::max(maximum, samples[i]);
        sumOfSquares += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }

    result.minimum = minimum;
    result.maximum = maximum;
    result.rms = static_cast<float>(std::sqrt(sumOfSquares / static_cast<double>(end - start)));
    return result;
}

AudioBuffer makeNoise(int channels, SampleCount frames, unsigned seed = 1234) {
    AudioBuffer buffer{ChannelLayout::discrete(channels), frames};
    std::mt19937 rng{seed};
    std::uniform_real_distribution<float> dist{-1.0f, 1.0f};
    for (int channel = 0; channel < channels; ++channel) {
        float* samples = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            samples[i] = dist(rng);
        }
    }
    return buffer;
}

PeakPyramid buildOrFail(const AudioBuffer& buffer,
                        SampleCount binSize = PeakPyramid::kDefaultBaseBinSize) {
    auto result = PeakPyramid::build(buffer.constView(), binSize);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

} // namespace

TEST_CASE("Building rejects a non-power-of-two bin size", "[io][peaks]") {
    const AudioBuffer buffer{ChannelLayout::mono(), 1024};

    CHECK_FALSE(PeakPyramid::build(buffer.constView(), 100).hasValue());
    CHECK_FALSE(PeakPyramid::build(buffer.constView(), 0).hasValue());
    CHECK_FALSE(PeakPyramid::build(buffer.constView(), 1).hasValue());
    CHECK_FALSE(PeakPyramid::build(buffer.constView(), -256).hasValue());
    CHECK(PeakPyramid::build(buffer.constView(), 256).hasValue());
}

TEST_CASE("An empty source yields an empty pyramid, not an error", "[io][peaks]") {
    const AudioBuffer empty;
    const auto pyramid = buildOrFail(empty);
    CHECK(pyramid.isEmpty());
    CHECK(pyramid.levelCount() == 0);
    CHECK(pyramid.memoryFootprint() == 0);
}

TEST_CASE("Level 0 summarises exactly the samples in its bin", "[io][peaks]") {
    const auto buffer = makeNoise(2, 4096);
    const auto pyramid = buildOrFail(buffer, 256);

    REQUIRE(pyramid.frameCountAt(0) == 16);

    for (int channel = 0; channel < 2; ++channel) {
        for (SampleCount frame = 0; frame < pyramid.frameCountAt(0); ++frame) {
            const auto expected = bruteForce(buffer, channel, frame * 256, (frame + 1) * 256);
            const PeakFrame& actual = pyramid.frameAt(0, channel, frame);

            INFO("channel " << channel << " frame " << frame);
            CHECK(actual.minimum == expected.minimum);
            CHECK(actual.maximum == expected.maximum);
            CHECK(actual.rms == Approx(expected.rms).epsilon(1e-5));
        }
    }
}

TEST_CASE("Every level aggregates exactly from the source", "[io][peaks]") {
    // The real risk is a level that is self-consistent but has drifted from the
    // source, so each level is checked against brute force, not its child.
    const auto buffer = makeNoise(1, 40000);
    const auto pyramid = buildOrFail(buffer, 256);
    REQUIRE(pyramid.levelCount() > 3);

    for (int level = 0; level < pyramid.levelCount(); ++level) {
        const SampleCount binSize = pyramid.binSizeAt(level);
        for (SampleCount frame = 0; frame < pyramid.frameCountAt(level); ++frame) {
            const auto expected = bruteForce(buffer, 0, frame * binSize, (frame + 1) * binSize);
            const PeakFrame& actual = pyramid.frameAt(level, 0, frame);

            INFO("level " << level << " frame " << frame);
            CHECK(actual.minimum == expected.minimum);
            CHECK(actual.maximum == expected.maximum);
            CHECK(actual.rms == Approx(expected.rms).epsilon(1e-4));
        }
    }
}

TEST_CASE("A single-sample transient survives to the top level", "[io][peaks]") {
    // The whole point of storing min and max rather than an average. If a click
    // vanishes when you zoom out, the display is lying about the audio and the
    // user cannot find the thing they opened the tool to fix.
    AudioBuffer buffer{ChannelLayout::mono(), 100000};
    buffer.channel(0)[54321] = 0.97f;
    buffer.channel(0)[54322] = -0.93f;

    const auto pyramid = buildOrFail(buffer, 256);
    REQUIRE(pyramid.levelCount() > 4);

    for (int level = 0; level < pyramid.levelCount(); ++level) {
        const SampleCount binSize = pyramid.binSizeAt(level);
        const SampleCount frame = 54321 / binSize;

        INFO("level " << level << " binSize " << binSize);
        CHECK(pyramid.frameAt(level, 0, frame).maximum == 0.97f);
        CHECK(pyramid.frameAt(level, 0, 54322 / binSize).minimum == -0.93f);
    }
}

TEST_CASE("A transient survives a full-file query at one pixel per file", "[io][peaks]") {
    AudioBuffer buffer{ChannelLayout::mono(), 200000};
    buffer.channel(0)[123456] = 0.88f;

    const auto pyramid = buildOrFail(buffer);
    PeakFrame single{};
    pyramid.query(0, 0, buffer.frames(), &single, 1);

    CHECK(single.maximum == 0.88f);
}

TEST_CASE("RMS of a DC signal equals its magnitude", "[io][peaks]") {
    AudioBuffer buffer{ChannelLayout::mono(), 8192};
    std::fill_n(buffer.channel(0), 8192, 0.5f);

    const auto pyramid = buildOrFail(buffer, 256);
    for (int level = 0; level < pyramid.levelCount(); ++level) {
        INFO("level " << level);
        CHECK(pyramid.frameAt(level, 0, 0).rms == Approx(0.5f).epsilon(1e-5));
    }
}

TEST_CASE("RMS combines through sum of squares, not by averaging RMS", "[io][peaks]") {
    // One loud bin, one silent bin. Correct answer is sqrt((1^2 + 0^2) / 2)
    // = 0.7071. Naively averaging the children's RMS gives 0.5 -- an error that
    // understates loud passages and would quietly misdraw every waveform.
    AudioBuffer buffer{ChannelLayout::mono(), 512};
    std::fill_n(buffer.channel(0), 256, 1.0f);
    std::fill_n(buffer.channel(0) + 256, 256, 0.0f);

    const auto pyramid = buildOrFail(buffer, 256);
    REQUIRE(pyramid.frameCountAt(0) == 2);
    CHECK(pyramid.frameAt(0, 0, 0).rms == Approx(1.0f).epsilon(1e-5));
    CHECK(pyramid.frameAt(0, 0, 1).rms == Approx(0.0f).margin(1e-6));

    PeakFrame combined{};
    pyramid.query(0, 0, 512, &combined, 1);
    CHECK(combined.rms == Approx(0.70710678f).epsilon(1e-5));
    CHECK(combined.rms != Approx(0.5f).epsilon(1e-3));
}

TEST_CASE("A partial final bin is weighted by its real sample count", "[io][peaks]") {
    // 300 frames at bin size 256 leaves a 44-sample tail. Treating it as a full
    // bin would skew both the tail's RMS and every level above it.
    AudioBuffer buffer{ChannelLayout::mono(), 300};
    std::fill_n(buffer.channel(0), 300, 0.25f);

    const auto pyramid = buildOrFail(buffer, 256);
    REQUIRE(pyramid.frameCountAt(0) == 2);
    CHECK(pyramid.samplesInFrame(0, 0) == 256);
    CHECK(pyramid.samplesInFrame(0, 1) == 44);

    CHECK(pyramid.frameAt(0, 0, 1).rms == Approx(0.25f).epsilon(1e-5));

    PeakFrame whole{};
    pyramid.query(0, 0, 300, &whole, 1);
    CHECK(whole.rms == Approx(0.25f).epsilon(1e-5));
}

TEST_CASE("query matches brute force over arbitrary ranges", "[io][peaks]") {
    const auto buffer = makeNoise(2, 50000, 99);
    const auto pyramid = buildOrFail(buffer, 256);

    std::mt19937 rng{7};
    std::uniform_int_distribution<SampleIndex> position{0, 49999};

    for (int trial = 0; trial < 200; ++trial) {
        SampleIndex start = position(rng);
        SampleIndex end = position(rng);
        if (start > end) {
            std::swap(start, end);
        }
        // Keep the range wide enough that the pyramid, not the source, is the
        // honest answer -- see shouldReadSource().
        end = std::max(end, start + 4096);
        end = std::min<SampleIndex>(end, buffer.frames());
        if (end <= start) {
            continue;
        }

        const int channel = trial % 2;
        PeakFrame actual{};
        pyramid.query(channel, start, end, &actual, 1);
        const auto expected = bruteForce(buffer, channel, start, end);

        INFO("trial " << trial << " range [" << start << ", " << end << ")");
        // The pyramid aggregates whole bins, so its range is a superset of the
        // exact one: peaks must bound the true values, never fall inside them.
        CHECK(actual.maximum >= expected.maximum);
        CHECK(actual.minimum <= expected.minimum);
        CHECK(actual.maximum <= 1.0f);
        CHECK(actual.minimum >= -1.0f);
    }
}

TEST_CASE("Bin-aligned queries are exact", "[io][peaks]") {
    const auto buffer = makeNoise(1, 32768, 11);
    const auto pyramid = buildOrFail(buffer, 256);

    for (SampleIndex start = 0; start + 4096 <= 32768; start += 4096) {
        const auto expected = bruteForce(buffer, 0, start, start + 4096);
        PeakFrame actual{};
        pyramid.query(0, start, start + 4096, &actual, 1);

        INFO("range [" << start << ", " << start + 4096 << ")");
        CHECK(actual.minimum == expected.minimum);
        CHECK(actual.maximum == expected.maximum);
        CHECK(actual.rms == Approx(expected.rms).epsilon(1e-4));
    }
}

TEST_CASE("query fills every requested column", "[io][peaks]") {
    const auto buffer = makeNoise(1, 65536, 3);
    const auto pyramid = buildOrFail(buffer);

    std::vector<PeakFrame> columns(800);
    pyramid.query(0, 0, buffer.frames(), columns.data(), static_cast<int>(columns.size()));

    int nonSilent = 0;
    for (const PeakFrame& column : columns) {
        if (column.maximum != 0.0f || column.minimum != 0.0f) {
            ++nonSilent;
        }
    }
    CHECK(nonSilent == static_cast<int>(columns.size()));
}

TEST_CASE("Ranges beyond the source draw as silence, not garbage", "[io][peaks]") {
    const auto buffer = makeNoise(1, 4096);
    const auto pyramid = buildOrFail(buffer);

    std::vector<PeakFrame> columns(16);
    pyramid.query(0, 100000, 200000, columns.data(), 16);
    for (const PeakFrame& column : columns) {
        CHECK(column.minimum == 0.0f);
        CHECK(column.maximum == 0.0f);
        CHECK(column.rms == 0.0f);
    }
}

TEST_CASE("Degenerate query arguments are ignored safely", "[io][peaks]") {
    const auto buffer = makeNoise(1, 4096);
    const auto pyramid = buildOrFail(buffer);
    PeakFrame frame{1.0f, 1.0f, 1.0f};

    pyramid.query(0, 100, 50, &frame, 1); // end before start
    CHECK(frame.maximum == 0.0f);

    pyramid.query(9, 0, 4096, &frame, 1); // channel out of range
    CHECK(frame.maximum == 0.0f);

    pyramid.query(0, 0, 4096, nullptr, 4); // null output
    pyramid.query(0, 0, 4096, &frame, 0);  // no columns
    SUCCEED("no crash on degenerate input");
}

TEST_CASE("Level selection tracks zoom", "[io][peaks]") {
    const auto buffer = makeNoise(1, 1 << 20);
    const auto pyramid = buildOrFail(buffer, 256);

    CHECK(pyramid.levelForSamplesPerPixel(256) == 0);
    CHECK(pyramid.binSizeAt(pyramid.levelForSamplesPerPixel(512)) == 512);
    CHECK(pyramid.binSizeAt(pyramid.levelForSamplesPerPixel(1000)) == 512);
    CHECK(pyramid.binSizeAt(pyramid.levelForSamplesPerPixel(4096)) == 4096);

    // Zoomed in past level 0 the pyramid cannot answer honestly.
    CHECK(pyramid.shouldReadSource(64));
    CHECK(pyramid.shouldReadSource(255));
    CHECK_FALSE(pyramid.shouldReadSource(256));
    CHECK_FALSE(pyramid.shouldReadSource(100000));
}

TEST_CASE("Level sizes halve and storage stays bounded", "[io][peaks]") {
    const auto buffer = makeNoise(2, 1 << 20, 5);
    const auto pyramid = buildOrFail(buffer, 256);

    for (int level = 1; level < pyramid.levelCount(); ++level) {
        CHECK(pyramid.binSizeAt(level) == pyramid.binSizeAt(level - 1) * 2);
        CHECK(pyramid.frameCountAt(level) <= pyramid.frameCountAt(level - 1));
    }
    CHECK(pyramid.frameCountAt(pyramid.levelCount() - 1) <= PeakPyramid::kMinimumTopLevelFrames);

    // The geometric series over all levels must stay under 2x level 0.
    const std::size_t levelZero =
        static_cast<std::size_t>(pyramid.frameCountAt(0)) * 2 * sizeof(PeakFrame);
    CHECK(pyramid.memoryFootprint() < levelZero * 2);
}

TEST_CASE("query allocates nothing", "[io][peaks][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    const auto buffer = makeNoise(2, 200000);
    const auto pyramid = buildOrFail(buffer);
    std::vector<PeakFrame> columns(1920);

    const rt::ScopedAudioThread guard;
    const rt::AllocationScope scope;
    pyramid.query(0, 0, buffer.frames(), columns.data(), static_cast<int>(columns.size()));
    pyramid.query(1, 5000, 150000, columns.data(), static_cast<int>(columns.size()));

    CHECK(scope.count() == 0);
}
