#include <sa/analysis/Spectrum.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

[[nodiscard]] std::vector<float> tone(SampleCount count, double frequency, double amplitude) {
    std::vector<float> samples(static_cast<std::size_t>(count));
    for (SampleCount i = 0; i < count; ++i) {
        samples[static_cast<std::size_t>(i)] =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                                    static_cast<double>(i) / kRate));
    }
    return samples;
}

[[nodiscard]] int loudestBin(const std::vector<float>& curve) {
    return static_cast<int>(
        std::distance(curve.begin(), std::max_element(curve.begin(), curve.end())));
}

} // namespace

TEST_CASE("A full-scale sine on a bin centre reads zero dBFS", "[analysis][spectrum]") {
    // The normalisation everybody assumes and nobody is told. Without it two
    // spectra taken with different windows cannot be compared, and a reader
    // has no idea what the vertical axis means.
    SpectrumSettings settings;
    settings.fftSize = 8192;
    auto made = SpectrumAnalyser::create(SampleRate{kRate}, settings);
    REQUIRE(made);
    SpectrumAnalyser& analyser = made.value();

    // Bin 200 of 8192 at 48 kHz: exactly 1171.875 Hz, so no leakage to argue
    // about.
    const double frequency = analyser.binFrequency(200);
    CHECK(frequency == Approx(1171.875));

    const std::vector<float> samples = tone(48000, frequency, 1.0);
    analyser.add(samples.data(), 48000);
    REQUIRE(analyser.frameCount() > 0);

    const std::vector<float> average = analyser.averageDb();
    CHECK(loudestBin(average) == 200);
    INFO("bin 200 reads " << average[200] << " dBFS");
    CHECK(average[200] == Approx(0.0).margin(0.1));
}

TEST_CASE("Half scale is six decibels down", "[analysis][spectrum]") {
    auto made = SpectrumAnalyser::create(SampleRate{kRate});
    REQUIRE(made);
    SpectrumAnalyser& analyser = made.value();

    const double frequency = analyser.binFrequency(200);
    const std::vector<float> samples = tone(48000, frequency, 0.5);
    analyser.add(samples.data(), 48000);

    const std::vector<float> average = analyser.averageDb();
    CHECK(average[200] == Approx(-6.0206).margin(0.1));
}

TEST_CASE("Feeding it in odd little pieces gives the same answer", "[analysis][spectrum]") {
    // The contract that makes it usable from a streaming reader: the caller's
    // block size is its own business.
    const std::vector<float> samples = tone(96000, 1000.0, 0.7);

    auto whole = SpectrumAnalyser::create(SampleRate{kRate});
    auto pieces = SpectrumAnalyser::create(SampleRate{kRate});
    REQUIRE(whole);
    REQUIRE(pieces);

    whole.value().add(samples.data(), 96000);
    for (SampleCount at = 0; at < 96000; at += 37) {
        pieces.value().add(samples.data() + at, std::min<SampleCount>(37, 96000 - at));
    }

    REQUIRE(whole.value().frameCount() == pieces.value().frameCount());
    const std::vector<float> a = whole.value().averageDb();
    const std::vector<float> b = pieces.value().averageDb();
    REQUIRE(a.size() == b.size());

    double worst = 0.0;
    for (std::size_t bin = 0; bin < a.size(); ++bin) {
        worst = std::max(worst, std::abs(static_cast<double>(a[bin] - b[bin])));
    }
    INFO("worst difference " << worst << " dB");
    CHECK(worst < 1e-3);
}

TEST_CASE("The peak curve sits above the average, and says something different",
          "[analysis][spectrum]") {
    // A tone that is only there for part of the passage: its average is pulled
    // down by the silence, its peak is not. That difference is the whole reason
    // both curves exist.
    std::vector<float> samples(96000, 0.0f);
    const std::vector<float> burst = tone(24000, 3000.0, 0.8);
    std::copy(burst.begin(), burst.end(), samples.begin() + 36000);

    auto made = SpectrumAnalyser::create(SampleRate{kRate});
    REQUIRE(made);
    made.value().add(samples.data(), 96000);

    const std::vector<float> average = made.value().averageDb();
    const std::vector<float> peak = made.value().peakDb();
    const int bin = loudestBin(peak);
    const auto at = static_cast<std::size_t>(bin);
    INFO("bin " << bin << " at " << made.value().binFrequency(bin) << " Hz: average " << average[at]
                << ", peak " << peak[at]);

    CHECK(made.value().binFrequency(bin) == Approx(3000.0).margin(20.0));
    CHECK(peak[at] > average[at] + 5.0);
    // Every bin: the peak is never below the average.
    for (std::size_t i = 0; i < average.size(); ++i) {
        REQUIRE(peak[i] >= average[i] - 1e-3f);
    }
}

TEST_CASE("Noise comes out flat", "[analysis][spectrum]") {
    std::mt19937 engine{4242};
    std::normal_distribution<float> distribution{0.0f, 0.2f};
    std::vector<float> samples(192000);
    for (float& sample : samples) {
        sample = distribution(engine);
    }

    auto made = SpectrumAnalyser::create(SampleRate{kRate});
    REQUIRE(made);
    made.value().add(samples.data(), 192000);

    // Averaged over the middle of the band, away from the ends where the
    // window's own shape matters.
    const std::vector<float> average = made.value().averageDb();
    double lowest = 0.0;
    double highest = -300.0;
    for (int bin = 200; bin < 3800; ++bin) {
        lowest = std::min(lowest, static_cast<double>(average[static_cast<std::size_t>(bin)]));
        highest = std::max(highest, static_cast<double>(average[static_cast<std::size_t>(bin)]));
    }
    INFO("noise spans " << lowest << " to " << highest << " dB");
    CHECK(highest - lowest < 12.0);
}

TEST_CASE("Nothing fed means nothing claimed", "[analysis][spectrum]") {
    auto made = SpectrumAnalyser::create(SampleRate{kRate});
    REQUIRE(made);
    CHECK(made.value().frameCount() == 0);

    // Less than one window in: still nothing, rather than a curve built from
    // the zeros that happen to be in the history.
    const std::vector<float> samples = tone(1000, 1000.0, 0.5);
    made.value().add(samples.data(), 1000);
    CHECK(made.value().frameCount() == 0);

    for (const float value : made.value().averageDb()) {
        CHECK(value == SpectrumAnalyser::kSilenceDb);
    }
}

TEST_CASE("Resetting forgets everything", "[analysis][spectrum]") {
    auto made = SpectrumAnalyser::create(SampleRate{kRate});
    REQUIRE(made);
    const std::vector<float> samples = tone(48000, 1000.0, 0.5);
    made.value().add(samples.data(), 48000);
    REQUIRE(made.value().frameCount() > 0);

    made.value().reset();
    CHECK(made.value().frameCount() == 0);
    for (const float value : made.value().peakDb()) {
        CHECK(value == SpectrumAnalyser::kSilenceDb);
    }
}

TEST_CASE("The analyser refuses what it cannot do", "[analysis][spectrum]") {
    CHECK_FALSE(SpectrumAnalyser::create(SampleRate{0.0}).hasValue());

    SpectrumSettings settings;
    settings.fftSize = 1000; // Not a power of two.
    CHECK_FALSE(SpectrumAnalyser::create(SampleRate{kRate}, settings).hasValue());
    settings.fftSize = 32; // Too small to say anything.
    CHECK_FALSE(SpectrumAnalyser::create(SampleRate{kRate}, settings).hasValue());

    settings = SpectrumSettings{};
    settings.overlap = 0;
    CHECK_FALSE(SpectrumAnalyser::create(SampleRate{kRate}, settings).hasValue());
    settings.overlap = 3; // Does not divide 8192.
    CHECK_FALSE(SpectrumAnalyser::create(SampleRate{kRate}, settings).hasValue());
    settings.overlap = 4;
    CHECK(SpectrumAnalyser::create(SampleRate{kRate}, settings).hasValue());
}

TEST_CASE("A segment boundary does not smear one stretch into the next", "[analysis][spectrum]") {
    // A caller characterising a two-hour programme takes a stretch here and a
    // stretch there. Without a way to say "these are not adjacent", the frame
    // that straddles the join measures the edit rather than the audio.
    const std::vector<float> low = tone(24000, 500.0, 0.8);
    const std::vector<float> high = tone(24000, 6000.0, 0.8);

    auto made = SpectrumAnalyser::create(SampleRate{kRate});
    REQUIRE(made);
    SpectrumAnalyser& analyser = made.value();

    analyser.add(low.data(), 24000);
    analyser.startSegment();
    analyser.add(high.data(), 24000);

    const std::vector<float> peak = analyser.peakDb();

    // Both tones are there.
    const auto binOf = [&](double hz) {
        return static_cast<std::size_t>(std::lround(hz * analyser.fftSize() / kRate));
    };
    CHECK(peak[binOf(500.0)] > -12.0f);
    CHECK(peak[binOf(6000.0)] > -12.0f);

    // And the join has not thrown a broadband click across the spectrum: a
    // discontinuity between two pure tones would show up as energy everywhere,
    // and there is none at a frequency neither tone occupies.
    INFO("between the tones: " << peak[binOf(2500.0)] << " dB");
    CHECK(peak[binOf(2500.0)] < -40.0f);
}
