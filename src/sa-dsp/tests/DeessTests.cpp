#include <sa/dsp/Deess.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};

/// Speech-shaped material with sibilants in it: a low tone for the voice, and
/// bursts of high-frequency noise for the esses.
///
/// Crude, and deliberately so. What is being tested is that the high band is
/// acted on and the low band is not, and that needs the two to be separable by
/// construction so the test can check each one on its own.
struct Voice {
    AudioBuffer audio;
    std::vector<bool> sibilant;
};

[[nodiscard]] Voice speech(double sibilantAmplitude = 0.5, SampleCount frames = 48000 * 2) {
    std::mt19937 engine{19};
    std::normal_distribution<double> hiss{0.0, 1.0};

    Voice out{AudioBuffer{ChannelLayout::mono(), frames},
              std::vector<bool>(static_cast<std::size_t>(frames))};
    // A one-pole high-pass on the noise puts its energy where sibilance is.
    double previous = 0.0;
    double state = 0.0;
    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate.hz();
        // A sibilant every 400 ms, lasting 100.
        const bool ess = std::fmod(t, 0.4) < 0.1;
        out.sibilant[static_cast<std::size_t>(i)] = ess;

        const double vowel = 0.3 * std::sin(2.0 * std::numbers::pi * 220.0 * t);
        double noise = 0.0;
        if (ess) {
            const double raw = hiss(engine);
            state = 0.85 * (state + raw - previous);
            previous = raw;
            noise = sibilantAmplitude * state * 0.2;
        } else {
            state = 0.0;
            previous = 0.0;
        }
        out.audio.channel(0)[i] = static_cast<float>(vowel + noise);
    }
    return out;
}

/// Energy above `hz`, by a cheap one-pole split. Enough to say which band
/// moved, which is the whole question here.
[[nodiscard]] double highEnergy(const AudioBuffer& audio, const std::vector<bool>& mask,
                                bool wantSibilant, double hz = 5000.0) {
    const double w = 2.0 * std::numbers::pi * hz / kRate.hz();
    const double alpha = std::sin(w) / (1.0 + std::cos(w));
    double low = 0.0;
    double total = 0.0;
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        const double value = audio.channel(0)[i];
        low += alpha * (value - low);
        if (mask[static_cast<std::size_t>(i)] == wantSibilant) {
            const double high = value - low;
            total += high * high;
        }
    }
    return total;
}

[[nodiscard]] double lowEnergy(const AudioBuffer& audio, double hz = 5000.0) {
    const double w = 2.0 * std::numbers::pi * hz / kRate.hz();
    const double alpha = std::sin(w) / (1.0 + std::cos(w));
    double low = 0.0;
    double total = 0.0;
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        low += alpha * (audio.channel(0)[i] - low);
        total += low * low;
    }
    return total;
}

[[nodiscard]] double decibels(double ratio) {
    return 10.0 * std::log10(std::max(ratio, 1e-30));
}

} // namespace

TEST_CASE("De-essing takes energy out of the sibilants") {
    Voice before = speech();
    Voice after = speech();

    const auto report = deess(after.audio.view(), kRate);
    REQUIRE(report);
    REQUIRE(report.value().peakReductionDb > 3.0);

    const double was = highEnergy(before.audio, before.sibilant, true);
    const double now = highEnergy(after.audio, after.sibilant, true);
    REQUIRE(decibels(now / was) < -3.0);
}

TEST_CASE("And leaves the rest of the voice where it was") {
    // The property that makes it a de-esser rather than a low-pass. The low
    // band is never touched, so the vowels come through at the same level.
    Voice before = speech();
    Voice after = speech();

    REQUIRE(deess(after.audio.view(), kRate));

    const double was = lowEnergy(before.audio);
    const double now = lowEnergy(after.audio);
    REQUIRE(decibels(now / was) == Approx(0.0).margin(0.2));
}

TEST_CASE("Material with no sibilance is left alone") {
    // A steady tone well below the split, at a level that would trigger any
    // broadband compressor. Nothing should happen to it.
    AudioBuffer audio{ChannelLayout::mono(), 48000};
    AudioBuffer original{ChannelLayout::mono(), 48000};
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        const auto value = static_cast<float>(
            0.7 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / kRate.hz()));
        audio.channel(0)[i] = value;
        original.channel(0)[i] = value;
    }

    const auto report = deess(audio.view(), kRate);
    REQUIRE(report);
    REQUIRE(report.value().peakReductionDb < 1.0);
    REQUIRE(report.value().fractionReduced < 0.01);

    // The crossover's phase response means the sum is not sample-identical, so
    // this is a level check rather than a byte one -- and the comment in
    // Deess.cpp says why that is the price of splitting at all.
    REQUIRE(decibels(lowEnergy(audio) / lowEnergy(original)) == Approx(0.0).margin(0.1));
}

TEST_CASE("The reduction limit is a limit") {
    // A very low threshold and a very high ratio would pull the band down
    // enormously; the stop is what keeps it from removing the consonant
    // altogether.
    Voice voice = speech(1.0);
    DeessSettings settings;
    settings.thresholdDb = -60.0;
    settings.ratio = 40.0;
    settings.maximumReductionDb = 6.0;

    const auto report = deess(voice.audio.view(), kRate, settings);
    REQUIRE(report);
    REQUIRE(report.value().peakReductionDb <= Approx(6.0).margin(0.01));
}

TEST_CASE("A harder setting removes more") {
    const auto energyWith = [](double threshold, double ratio) {
        Voice voice = speech();
        DeessSettings settings;
        settings.thresholdDb = threshold;
        settings.ratio = ratio;
        REQUIRE(deess(voice.audio.view(), kRate, settings));
        return highEnergy(voice.audio, voice.sibilant, true);
    };

    const double gentle = energyWith(-20.0, 3.0);
    const double firm = energyWith(-35.0, 12.0);
    REQUIRE(firm < gentle);
}

TEST_CASE("The report says how much of the material was acted on") {
    Voice voice = speech();
    const auto report = deess(voice.audio.view(), kRate);
    REQUIRE(report);
    // Sibilants are 100 ms in every 400, so a quarter of the material. The
    // envelope's release stretches that a little.
    REQUIRE(report.value().fractionReduced > 0.1);
    REQUIRE(report.value().fractionReduced < 0.6);
}

TEST_CASE("Impossible de-esser settings are refused") {
    AudioBuffer audio{ChannelLayout::mono(), 1000};
    DeessSettings settings;

    settings.frequencyHz = 0.0;
    REQUIRE_FALSE(deess(audio.view(), kRate, settings));
    settings.frequencyHz = 30000.0; // Above Nyquist at 48 kHz.
    REQUIRE_FALSE(deess(audio.view(), kRate, settings));

    settings = {};
    settings.maximumReductionDb = -1.0;
    REQUIRE_FALSE(deess(audio.view(), kRate, settings));

    settings = {};
    REQUIRE_FALSE(deess(audio.view(), kRate, settings, 2000));
}

TEST_CASE("De-essing an empty buffer is a no-op") {
    AudioBuffer audio{ChannelLayout::stereo(), 0};
    const auto report = deess(audio.view(), kRate);
    REQUIRE(report);
    REQUIRE(report.value().peakReductionDb == 0.0);
}

TEST_CASE("Both channels of a stereo pair are de-essed") {
    AudioBuffer audio{ChannelLayout::stereo(), 48000};
    std::mt19937 engine{5};
    std::normal_distribution<float> hiss{0.0f, 0.3f};
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        const float value = hiss(engine);
        audio.channel(0)[i] = value;
        audio.channel(1)[i] = value;
    }

    const auto report = deess(audio.view(), kRate);
    REQUIRE(report);
    // Identical input to both channels must give identical output, which it
    // only does if each channel has its own filter state and its own envelope.
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        REQUIRE(audio.channel(0)[i] == audio.channel(1)[i]);
    }
}
