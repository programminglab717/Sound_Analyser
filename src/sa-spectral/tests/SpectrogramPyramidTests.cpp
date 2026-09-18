#include <sa/core/RealtimeGuard.h>
#include <sa/spectral/SpectrogramPyramid.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

using namespace sa;
using namespace sa::spectral;
using Catch::Approx;

namespace {

AudioBuffer makeSine(SampleCount frames, double binsPerCycle, float amplitude = 1.0f,
                     int fftSize = 2048) {
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    float* samples = buffer.channel(0);
    for (SampleCount i = 0; i < frames; ++i) {
        samples[i] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * binsPerCycle *
                                                             static_cast<double>(i) /
                                                             static_cast<double>(fftSize)));
    }
    return buffer;
}

SpectrogramPyramid buildOrFail(const AudioBuffer& buffer, const SpectrogramConfig& config = {}) {
    auto result = SpectrogramPyramid::build(buffer.constView(), 0, config);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

} // namespace

TEST_CASE("Build validates its inputs", "[spectral][pyramid]") {
    const AudioBuffer buffer{ChannelLayout::mono(), 8192};

    CHECK(SpectrogramPyramid::build(buffer.constView(), 0).hasValue());
    CHECK_FALSE(SpectrogramPyramid::build(buffer.constView(), 1).hasValue());
    CHECK_FALSE(SpectrogramPyramid::build(buffer.constView(), -1).hasValue());

    SpectrogramConfig badRange;
    badRange.minimumDecibels = 0.0f;
    badRange.maximumDecibels = -120.0f;
    CHECK_FALSE(SpectrogramPyramid::build(buffer.constView(), 0, badRange).hasValue());

    SpectrogramConfig badFft;
    badFft.fftSize = 1000;
    CHECK_FALSE(SpectrogramPyramid::build(buffer.constView(), 0, badFft).hasValue());
}

TEST_CASE("An empty source yields an empty pyramid", "[spectral][pyramid]") {
    const AudioBuffer empty;
    const auto pyramid = buildOrFail(empty);
    CHECK(pyramid.isEmpty());
    CHECK(pyramid.memoryFootprint() == 0);
}

TEST_CASE("A full-scale sine reads close to 0 dBFS", "[spectral][pyramid]") {
    // Magnitudes are normalised by the window's coherent gain so the reading is
    // an absolute dBFS value, not an arbitrary scale that shifts when the
    // window changes.
    const auto buffer = makeSine(65536, 100.0, 1.0f);
    const auto pyramid = buildOrFail(buffer);

    std::uint8_t loudest = 0;
    for (int bin = 0; bin < pyramid.binCount(); ++bin) {
        loudest = std::max(loudest, pyramid.magnitudeAt(0, pyramid.frameCountAt(0) / 2, bin));
    }
    CHECK(pyramid.toDecibels(loudest) == Approx(0.0f).margin(1.0f));
}

TEST_CASE("Halving amplitude drops the reading by 6 dB", "[spectral][pyramid]") {
    const auto loud = buildOrFail(makeSine(65536, 100.0, 1.0f));
    const auto quiet = buildOrFail(makeSine(65536, 100.0, 0.5f));

    const auto peak = [](const SpectrogramPyramid& pyramid) {
        std::uint8_t best = 0;
        for (int bin = 0; bin < pyramid.binCount(); ++bin) {
            best = std::max(best, pyramid.magnitudeAt(0, pyramid.frameCountAt(0) / 2, bin));
        }
        return pyramid.toDecibels(best);
    };

    CHECK(peak(loud) - peak(quiet) == Approx(6.02f).margin(1.0f));
}

TEST_CASE("A sine lands in the expected bin", "[spectral][pyramid]") {
    const auto buffer = makeSine(65536, 100.0, 1.0f);
    const auto pyramid = buildOrFail(buffer);

    int loudestBin = 0;
    std::uint8_t loudest = 0;
    for (int bin = 0; bin < pyramid.binCount(); ++bin) {
        const std::uint8_t value = pyramid.magnitudeAt(0, pyramid.frameCountAt(0) / 2, bin);
        if (value > loudest) {
            loudest = value;
            loudestBin = bin;
        }
    }
    CHECK(loudestBin == 100);
}

TEST_CASE("Silence sits at the bottom of the range", "[spectral][pyramid]") {
    const AudioBuffer silence{ChannelLayout::mono(), 32768};
    const auto pyramid = buildOrFail(silence);

    for (int level = 0; level < pyramid.levelCount(); ++level) {
        for (int bin = 0; bin < pyramid.binCount(); bin += 37) {
            REQUIRE(pyramid.magnitudeAt(level, 0, bin) == 0);
        }
    }
}

TEST_CASE("A one-frame transient survives to every level", "[spectral][pyramid]") {
    // The reason upper levels max-combine instead of averaging. A click that
    // fades as you zoom out is a display that hides the defect the user opened
    // the tool to fix.
    AudioBuffer buffer{ChannelLayout::mono(), 262144};
    buffer.channel(0)[131072] = 1.0f; // single-sample impulse: broadband

    SpectrogramConfig config;
    config.fftSize = 1024;
    config.hopSize = 256;
    const auto pyramid = buildOrFail(buffer, config);
    REQUIRE(pyramid.levelCount() > 5);

    for (int level = 0; level < pyramid.levelCount(); ++level) {
        const SampleCount frame = 131072 / pyramid.hopAt(level);
        std::uint8_t loudest = 0;
        // The impulse spans the frames whose window covers it.
        for (SampleCount f = frame - 1; f <= frame + 1; ++f) {
            for (int bin = 0; bin < pyramid.binCount(); ++bin) {
                loudest = std::max(loudest, pyramid.magnitudeAt(level, f, bin));
            }
        }
        INFO("level " << level << " hop " << pyramid.hopAt(level));
        CHECK(loudest > 0);
        CHECK(pyramid.toDecibels(loudest) > -90.0f);
    }
}

TEST_CASE("A transient survives a whole-file render", "[spectral][pyramid]") {
    AudioBuffer buffer{ChannelLayout::mono(), 500000};
    buffer.channel(0)[250000] = 1.0f;

    const auto pyramid = buildOrFail(buffer);
    std::vector<std::uint8_t> tile(64 * 128);
    pyramid.render(0, buffer.frames(), 0, 64, 128, tile.data());

    std::uint8_t loudest = 0;
    for (std::uint8_t value : tile) {
        loudest = std::max(loudest, value);
    }
    CHECK(pyramid.toDecibels(loudest) > -90.0f);
}

TEST_CASE("Levels halve in time resolution and storage stays bounded", "[spectral][pyramid]") {
    const auto buffer = makeSine(1 << 18, 50.0);
    const auto pyramid = buildOrFail(buffer);

    for (int level = 1; level < pyramid.levelCount(); ++level) {
        CHECK(pyramid.hopAt(level) == pyramid.hopAt(level - 1) * 2);
        CHECK(pyramid.frameCountAt(level) <= pyramid.frameCountAt(level - 1));
    }

    // The geometric series bounds the total at 2x level 0, plus up to one extra
    // frame per level because each level rounds its frame count up.
    const auto bins = static_cast<std::size_t>(pyramid.binCount());
    const std::size_t levelZero = static_cast<std::size_t>(pyramid.frameCountAt(0)) * bins;
    const std::size_t slack = static_cast<std::size_t>(pyramid.levelCount()) * bins;
    CHECK(pyramid.memoryFootprint() <= levelZero * 2 + slack);
}

TEST_CASE("Level selection tracks zoom", "[spectral][pyramid]") {
    const auto buffer = makeSine(1 << 20, 50.0);
    SpectrogramConfig config;
    config.hopSize = 512;
    const auto pyramid = buildOrFail(buffer, config);

    CHECK(pyramid.hopAt(pyramid.levelForSamplesPerColumn(512)) == 512);
    CHECK(pyramid.hopAt(pyramid.levelForSamplesPerColumn(1024)) == 1024);
    CHECK(pyramid.hopAt(pyramid.levelForSamplesPerColumn(5000)) == 4096);
}

TEST_CASE("render fills the requested tile", "[spectral][pyramid]") {
    const auto buffer = makeSine(200000, 80.0);
    const auto pyramid = buildOrFail(buffer);

    const int rows = 256;
    const int columns = 512;
    std::vector<std::uint8_t> tile(static_cast<std::size_t>(rows * columns), 7);
    pyramid.render(0, buffer.frames(), 0, rows, columns, tile.data());

    int nonSilent = 0;
    for (std::uint8_t value : tile) {
        if (value > 0) {
            ++nonSilent;
        }
    }
    CHECK(nonSilent > 0);
}

TEST_CASE("Degenerate render arguments are safe", "[spectral][pyramid]") {
    const auto buffer = makeSine(65536, 50.0);
    const auto pyramid = buildOrFail(buffer);
    std::vector<std::uint8_t> tile(64 * 64, 9);

    pyramid.render(0, buffer.frames(), 0, 0, 64, tile.data());    // no rows
    pyramid.render(0, buffer.frames(), 0, 64, 0, tile.data());    // no columns
    pyramid.render(100, 50, 0, 8, 8, tile.data());                // end before start
    pyramid.render(0, buffer.frames(), 0, 8, 8, nullptr);         // null output
    pyramid.render(0, buffer.frames(), 99999, 8, 8, tile.data()); // bin past the end
    SUCCEED("no crash on degenerate input");
}

TEST_CASE("Rendering past the end reads as silence", "[spectral][pyramid]") {
    const auto buffer = makeSine(65536, 50.0);
    const auto pyramid = buildOrFail(buffer);

    std::vector<std::uint8_t> tile(32 * 32, 200);
    pyramid.render(10000000, 20000000, 0, 32, 32, tile.data());
    for (std::uint8_t value : tile) {
        CHECK(value == 0);
    }
}

TEST_CASE("render allocates nothing", "[spectral][pyramid][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    const auto buffer = makeSine(500000, 60.0);
    const auto pyramid = buildOrFail(buffer);
    std::vector<std::uint8_t> tile(1080 * 1920);

    const rt::ScopedAudioThread guard;
    const rt::AllocationScope scope;
    pyramid.render(0, buffer.frames(), 0, 1080, 1920, tile.data());
    CHECK(scope.count() == 0);
}
