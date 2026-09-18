#include <sa/core/RealtimeGuard.h>
#include <sa/spectral/SpectrogramPyramid.h>

#include <algorithm>
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

TEST_CASE("Bin-edge render matches the linear render given linear edges",
          "[spectral][pyramid][render]") {
    const auto buffer = makeSine(16384, 40.0, 0.8f);
    const auto pyramid = buildOrFail(buffer);

    constexpr int kRows = 64;
    constexpr int kColumns = 48;
    const int bins = pyramid.binCount();

    std::vector<std::uint8_t> viaBins(kRows * kColumns, 0);
    std::vector<std::uint8_t> viaEdges(kRows * kColumns, 0);

    pyramid.render(0, 16384, 0, kRows, kColumns, viaBins.data());

    std::vector<float> edges(kRows + 1);
    for (int row = 0; row <= kRows; ++row) {
        edges[static_cast<std::size_t>(row)] =
            static_cast<float>(bins) * static_cast<float>(row) / static_cast<float>(kRows);
    }
    pyramid.render(0, 16384, edges.data(), kRows, kColumns, viaEdges.data());

    // The two paths round their bin ranges differently -- integer division
    // against floor/ceil -- so they are allowed to differ by the odd bin at a
    // boundary, but they must agree on where the energy is.
    int differing = 0;
    for (std::size_t i = 0; i < viaBins.size(); ++i) {
        if (viaBins[i] != viaEdges[i]) {
            ++differing;
        }
    }
    CHECK(differing < kRows * kColumns / 10);
}

TEST_CASE("A log frequency axis keeps a low tone visible", "[spectral][pyramid][render]") {
    // Bin 4 of 1025 sits in the bottom 0.4% of a linear axis: on a 600-row
    // display it lands in row 2 and is invisible. That is exactly the problem a
    // log axis exists to solve, so check the same tone lands well up the view.
    const auto buffer = makeSine(32768, 4.0, 0.9f);
    const auto pyramid = buildOrFail(buffer);

    constexpr int kRows = 256;
    constexpr int kColumns = 8;
    const auto bins = static_cast<float>(pyramid.binCount());

    // Log edges from bin 1 to the top; row 0 is the bottom of the view.
    std::vector<float> edges(kRows + 1);
    const float logLow = std::log(1.0f);
    const float logHigh = std::log(bins);
    for (int row = 0; row <= kRows; ++row) {
        const float t = static_cast<float>(row) / static_cast<float>(kRows);
        edges[static_cast<std::size_t>(row)] = std::exp(logLow + (logHigh - logLow) * t);
    }

    std::vector<std::uint8_t> tile(kRows * kColumns, 0);
    pyramid.render(0, 32768, edges.data(), kRows, kColumns, tile.data());

    int loudestRow = 0;
    std::uint8_t loudest = 0;
    for (int row = 0; row < kRows; ++row) {
        const std::uint8_t value = tile[static_cast<std::size_t>(row) * kColumns + 4];
        if (value > loudest) {
            loudest = value;
            loudestRow = row;
        }
    }

    CHECK(loudest > 200);
    // log(4)/log(1025) is about 0.2, so the peak belongs a fifth of the way up.
    CHECK(loudestRow > kRows / 8);
    CHECK(loudestRow < kRows / 2);
}

TEST_CASE("Bin-edge render survives degenerate edges", "[spectral][pyramid][render]") {
    const auto buffer = makeSine(8192, 20.0);
    const auto pyramid = buildOrFail(buffer);

    std::vector<std::uint8_t> tile(16 * 8, 7);

    // Null table, zero rows, and edges running off both ends of the spectrum.
    pyramid.render(0, 8192, nullptr, 16, 8, tile.data());
    CHECK(tile[0] == 7);

    std::vector<float> edges(17);
    for (int row = 0; row <= 16; ++row) {
        edges[static_cast<std::size_t>(row)] =
            -50.0f + static_cast<float>(row) * static_cast<float>(pyramid.binCount() + 100) / 16.0f;
    }
    CHECK_NOTHROW(pyramid.render(0, 8192, edges.data(), 16, 8, tile.data()));

    // Every edge identical: each row is still forced to one bin, never zero.
    std::fill(edges.begin(), edges.end(), 3.0f);
    CHECK_NOTHROW(pyramid.render(0, 8192, edges.data(), 16, 8, tile.data()));
}

TEST_CASE("Bin-edge render does not allocate", "[spectral][pyramid][render][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    const auto buffer = makeSine(16384, 30.0);
    const auto pyramid = buildOrFail(buffer);

    constexpr int kRows = 32;
    constexpr int kColumns = 32;
    std::vector<float> edges(kRows + 1);
    for (int row = 0; row <= kRows; ++row) {
        edges[static_cast<std::size_t>(row)] =
            static_cast<float>(pyramid.binCount()) * static_cast<float>(row) / kRows;
    }
    std::vector<std::uint8_t> tile(kRows * kColumns);

    const rt::ScopedAudioThread guard;
    const rt::AllocationScope scope;
    pyramid.render(0, 16384, edges.data(), kRows, kColumns, tile.data());
    CHECK(scope.count() == 0);
}

TEST_CASE("Sub-bin rows interpolate instead of stepping", "[spectral][pyramid][render]") {
    // A tone at bin 8 with rows ten times finer than bins. Nearest-bin sampling
    // would give ten identical rows then a jump; interpolation has to produce a
    // monotone climb into the peak.
    const auto buffer = makeSine(16384, 8.0, 0.9f);
    const auto pyramid = buildOrFail(buffer);

    constexpr int kRows = 60;
    constexpr int kColumns = 4;

    // Rows spanning bins 5 to 11: 6 bins over 60 rows, so 0.1 bins per row.
    std::vector<float> edges(kRows + 1);
    for (int row = 0; row <= kRows; ++row) {
        edges[static_cast<std::size_t>(row)] =
            5.0f + 6.0f * static_cast<float>(row) / static_cast<float>(kRows);
    }

    std::vector<std::uint8_t> tile(kRows * kColumns, 0);
    pyramid.render(0, 16384, edges.data(), kRows, kColumns, tile.data());

    std::vector<int> profile(kRows);
    for (int row = 0; row < kRows; ++row) {
        profile[static_cast<std::size_t>(row)] = tile[static_cast<std::size_t>(row) * kColumns + 2];
    }

    // Nearest-bin sampling gives at most 7 distinct values across these rows.
    // Interpolation gives many more; that difference is the whole point.
    std::vector<int> distinct = profile;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    CHECK(distinct.size() > 15);

    // And the climb into bin 8 -- row 30 -- must be monotone, not stepped.
    const int peakRow = 30;
    int decreases = 0;
    for (int row = 6; row < peakRow; ++row) {
        if (profile[static_cast<std::size_t>(row)] < profile[static_cast<std::size_t>(row) - 1]) {
            ++decreases;
        }
    }
    CHECK(decreases == 0);
    CHECK(profile[peakRow] > profile[6]);
}

namespace {

/// An AudioSource over a buffer, so the streaming build can be compared against
/// the one-shot build on exactly the same samples.
class BufferSource final : public io::AudioSource {
public:
    explicit BufferSource(const AudioBuffer& audio) : audio_(&audio) {
        info_.sampleRate = SampleRate{48000.0};
        info_.layout = audio.layout();
        info_.frameCount = audio.frames();
        info_.format = io::SampleFormat::Float32;
    }

    [[nodiscard]] const io::AudioFileInfo& info() const noexcept override { return info_; }

    [[nodiscard]] Result<SampleCount> read(SampleIndex startFrame,
                                           AudioBufferView destination) const override {
        if (startFrame < 0 || startFrame >= info_.frameCount) {
            return SampleCount{0};
        }
        const SampleCount count =
            std::min<SampleCount>(destination.frames(), info_.frameCount - startFrame);
        for (int channel = 0; channel < destination.channelCount(); ++channel) {
            std::copy_n(audio_->channel(channel) + startFrame, count, destination.channel(channel));
        }
        return count;
    }

private:
    const AudioBuffer* audio_;
    io::AudioFileInfo info_;
};

} // namespace

TEST_CASE("The streaming build is bit-identical to the one-shot build",
          "[spectral][pyramid][streaming]") {
    // This is the whole claim. A streaming implementation that shifts every
    // frame by a hop still looks like a spectrogram; it just disagrees with the
    // waveform beside it, and nothing but this test would say so.
    for (const SampleCount frames : {8192, 20000, 48000, 65537}) {
        const auto buffer = makeSine(frames, 37.0, 0.7f);
        const BufferSource source{buffer};

        SpectrogramConfig config;
        config.fftSize = 2048;
        config.hopSize = 512;

        const auto direct = buildOrFail(buffer, config);
        auto streamed = SpectrogramPyramid::buildStreaming(source, 0, config);
        REQUIRE(streamed.hasValue());

        REQUIRE(streamed.value().levelCount() == direct.levelCount());
        REQUIRE(streamed.value().binCount() == direct.binCount());
        REQUIRE(streamed.value().sourceFrames() == direct.sourceFrames());

        for (int level = 0; level < direct.levelCount(); ++level) {
            REQUIRE(streamed.value().frameCountAt(level) == direct.frameCountAt(level));
            for (SampleCount frame = 0; frame < direct.frameCountAt(level); ++frame) {
                const std::uint8_t* a = direct.frameData(level, frame);
                const std::uint8_t* b = streamed.value().frameData(level, frame);
                REQUIRE(a != nullptr);
                REQUIRE(b != nullptr);
                for (int bin = 0; bin < direct.binCount(); ++bin) {
                    if (a[bin] != b[bin]) {
                        FAIL("level " << level << " frame " << frame << " bin " << bin << ": "
                                      << int(a[bin]) << " vs " << int(b[bin]) << " at " << frames
                                      << " frames");
                    }
                }
            }
        }
    }
}

TEST_CASE("The streaming build validates its inputs", "[spectral][pyramid][streaming]") {
    const auto buffer = makeSine(8192, 20.0);
    const BufferSource source{buffer};

    CHECK_FALSE(SpectrogramPyramid::buildStreaming(source, -1).hasValue());
    CHECK_FALSE(SpectrogramPyramid::buildStreaming(source, 5).hasValue());

    SpectrogramConfig inverted;
    inverted.minimumDecibels = 0.0f;
    inverted.maximumDecibels = -120.0f;
    CHECK_FALSE(SpectrogramPyramid::buildStreaming(source, 0, inverted).hasValue());

    SpectrogramConfig odd;
    odd.fftSize = 1000;
    CHECK_FALSE(SpectrogramPyramid::buildStreaming(source, 0, odd).hasValue());

    const AudioBuffer empty{ChannelLayout::mono(), 0};
    const BufferSource nothing{empty};
    auto built = SpectrogramPyramid::buildStreaming(nothing, 0);
    REQUIRE(built.hasValue());
    CHECK(built.value().isEmpty());
}

TEST_CASE("A streaming build can be cancelled", "[spectral][pyramid][streaming]") {
    const auto buffer = makeSine(400000, 40.0);
    const BufferSource source{buffer};

    CancellationToken token;
    token.cancel();

    JobMonitor monitor;
    monitor.cancellation = &token;

    auto built = SpectrogramPyramid::buildStreaming(source, 0, SpectrogramConfig{}, monitor);
    REQUIRE_FALSE(built.hasValue());
    CHECK(built.error().code() == ErrorCode::Cancelled);
}
