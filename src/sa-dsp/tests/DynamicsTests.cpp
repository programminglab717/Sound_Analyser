#include <sa/core/RealtimeGuard.h>
#include <sa/dsp/Dynamics.h>
#include <sa/dsp/EnvelopeFollower.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

double toGain(double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

double toDecibels(double gain) {
    return 20.0 * std::log10(std::abs(gain));
}

SampleCount samplesFor(double seconds) {
    return static_cast<SampleCount>(std::lround(seconds * kRate));
}

/// A square wave, the only periodic signal whose magnitude is constant at every
/// sample. That makes it the right probe for a peak-detecting dynamics
/// processor: the detector sees one unchanging level, so the settled gain is
/// exactly what the transfer curve says and the timing is exactly what the time
/// constants say. A sine would smear both with intra-cycle ripple.
std::vector<float> squareWave(double amplitudeDb, double frequency, SampleCount count) {
    const auto amplitude = static_cast<float>(toGain(amplitudeDb));
    const auto half = static_cast<SampleCount>(kRate / (2.0 * frequency));
    std::vector<float> signal(static_cast<std::size_t>(count));
    for (SampleCount i = 0; i < count; ++i) {
        const bool high = ((i / half) % 2) == 0;
        signal[static_cast<std::size_t>(i)] = high ? amplitude : -amplitude;
    }
    return signal;
}

std::vector<float> whiteNoise(std::size_t count, unsigned seed, float scale) {
    std::mt19937 rng{seed};
    std::uniform_real_distribution<float> dist{-scale, scale};
    std::vector<float> noise(count);
    for (float& sample : noise) {
        sample = dist(rng);
    }
    return noise;
}

/// Soft-knee compressor curve, written out here from the published formulation
/// (Reiss & McPherson) rather than called from the module -- including the
/// above-knee branch as "threshold plus the overshoot divided by the ratio",
/// which is the form a specification states rather than the form the module
/// computes.
double expectedCompressorGainDb(double levelDb, double thresholdDb, double ratio, double kneeDb) {
    const double over = levelDb - thresholdDb;
    if (2.0 * over < -kneeDb) {
        return 0.0;
    }
    if (kneeDb > 0.0 && 2.0 * std::abs(over) <= kneeDb) {
        const double distance = over + kneeDb / 2.0;
        return (1.0 / ratio - 1.0) * distance * distance / (2.0 * kneeDb);
    }
    return thresholdDb + over / ratio - levelDb;
}

double expectedExpanderGainDb(double levelDb, double thresholdDb, double ratio, double kneeDb) {
    const double over = levelDb - thresholdDb;
    if (2.0 * over > kneeDb) {
        return 0.0;
    }
    if (kneeDb > 0.0 && 2.0 * std::abs(over) <= kneeDb) {
        const double distance = over - kneeDb / 2.0;
        return (1.0 - ratio) * distance * distance / (2.0 * kneeDb);
    }
    return thresholdDb + over * ratio - levelDb;
}

Compressor makeCompressor(const CompressorSettings& settings) {
    Result<Compressor> result = Compressor::create(kSampleRate48000, settings);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

Expander makeExpander(const ExpanderSettings& settings) {
    Result<Expander> result = Expander::create(kSampleRate48000, settings);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

Gate makeGate(const GateSettings& settings) {
    Result<Gate> result = Gate::create(kSampleRate48000, settings);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

Limiter makeLimiter(const LimiterSettings& settings) {
    Result<Limiter> result = Limiter::create(kSampleRate48000, settings);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

/// Largest output magnitude over a signal.
template <typename Processor>
double peakOf(Processor& processor, const std::vector<float>& signal) {
    double peak = 0.0;
    for (float sample : signal) {
        peak = std::max(peak, static_cast<double>(std::abs(processor.processSample(sample))));
    }
    return peak;
}

/// Largest output magnitude once the envelope has settled. The first three
/// quarters of the signal go in to let the attack finish; only the last quarter
/// is measured, because a whole-run maximum would report the very first
/// sample -- which arrives before the processor has done anything at all.
template <typename Processor>
double settledPeak(Processor& processor, const std::vector<float>& signal) {
    const std::size_t from = signal.size() * 3 / 4;
    double peak = 0.0;
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const double out = std::abs(static_cast<double>(processor.processSample(signal[i])));
        if (i >= from) {
            peak = std::max(peak, out);
        }
    }
    return peak;
}

} // namespace

// ---------------------------------------------------------------------------
// Envelope follower
// ---------------------------------------------------------------------------

TEST_CASE("A time constant means 63.2% of a step", "[dsp][dynamics][envelope]") {
    // The whole suite's timing claims rest on this convention, so it is pinned
    // here rather than assumed everywhere else.
    Result<EnvelopeFollower> result = EnvelopeFollower::create(
        kSampleRate48000, EnvelopeFollower::Settings{.attackSeconds = 0.010,
                                                     .releaseSeconds = 0.200,
                                                     .detector = EnvelopeFollower::Detector::Peak});
    REQUIRE(result.hasValue());
    EnvelopeFollower follower = std::move(result).value();

    const SampleCount attack = samplesFor(0.010);
    double value = 0.0;
    for (SampleCount i = 0; i < attack; ++i) {
        value = follower.process(1.0f);
    }
    CHECK(value == Approx(1.0 - std::exp(-1.0)).margin(1e-4));

    // Twice the constant is 86.5%, not 100%: a one-pole never arrives.
    for (SampleCount i = 0; i < attack; ++i) {
        value = follower.process(1.0f);
    }
    CHECK(value == Approx(1.0 - std::exp(-2.0)).margin(1e-4));
}

TEST_CASE("An RMS detector reads the energy, a peak detector the waveform",
          "[dsp][dynamics][envelope]") {
    const double amplitude = 0.8;
    std::vector<float> sine(static_cast<std::size_t>(kRate));
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate));
    }

    Result<EnvelopeFollower> rmsResult = EnvelopeFollower::create(
        kSampleRate48000, EnvelopeFollower::Settings{.attackSeconds = 0.100,
                                                     .releaseSeconds = 0.100,
                                                     .detector = EnvelopeFollower::Detector::Rms});
    REQUIRE(rmsResult.hasValue());
    EnvelopeFollower rms = std::move(rmsResult).value();
    double rmsValue = 0.0;
    for (float sample : sine) {
        rmsValue = rms.process(sample);
    }
    // The RMS of a sine is its amplitude over root two -- 3 dB below the peak.
    CHECK(rmsValue == Approx(amplitude / std::numbers::sqrt2).epsilon(0.01));

    Result<EnvelopeFollower> peakResult = EnvelopeFollower::create(
        kSampleRate48000, EnvelopeFollower::Settings{.attackSeconds = 0.0,
                                                     .releaseSeconds = 1.0,
                                                     .detector = EnvelopeFollower::Detector::Peak});
    REQUIRE(peakResult.hasValue());
    EnvelopeFollower peak = std::move(peakResult).value();
    double peakValue = 0.0;
    for (float sample : sine) {
        peakValue = std::max(peakValue, peak.process(sample));
    }
    CHECK(peakValue == Approx(amplitude).epsilon(0.001));
}

TEST_CASE("Envelope follower parameters are validated", "[dsp][dynamics][envelope]") {
    CHECK_FALSE(EnvelopeFollower::create(SampleRate{0.0}).hasValue());
    CHECK_FALSE(EnvelopeFollower::create(kSampleRate48000,
                                         EnvelopeFollower::Settings{.attackSeconds = -1.0})
                    .hasValue());
    CHECK_FALSE(
        EnvelopeFollower::create(
            kSampleRate48000,
            EnvelopeFollower::Settings{.releaseSeconds = std::numeric_limits<double>::infinity()})
            .hasValue());

    Result<EnvelopeFollower> result = EnvelopeFollower::create(kSampleRate48000);
    REQUIRE(result.hasValue());
    EnvelopeFollower follower = std::move(result).value();
    CHECK_FALSE(follower.setSettings(EnvelopeFollower::Settings{.attackSeconds = -0.1}).ok());
    CHECK(follower.setSettings(EnvelopeFollower::Settings{.attackSeconds = 0.0}).ok());
}

// ---------------------------------------------------------------------------
// Compressor
// ---------------------------------------------------------------------------

TEST_CASE("Compressor parameters are validated", "[dsp][dynamics][compressor]") {
    CHECK(Compressor::create(kSampleRate48000).hasValue());
    CHECK_FALSE(Compressor::create(SampleRate{0.0}).hasValue());

    const auto bad = [](auto&& mutate) {
        CompressorSettings settings;
        mutate(settings);
        return Compressor::create(kSampleRate48000, settings);
    };

    CHECK_FALSE(bad([](CompressorSettings& s) { s.ratio = 0.5; }).hasValue());
    CHECK_FALSE(bad([](CompressorSettings& s) {
                    s.ratio = std::numeric_limits<double>::infinity();
                }).hasValue());
    CHECK_FALSE(bad([](CompressorSettings& s) { s.attackSeconds = -0.001; }).hasValue());
    CHECK_FALSE(bad([](CompressorSettings& s) { s.releaseSeconds = -1.0; }).hasValue());
    CHECK_FALSE(bad([](CompressorSettings& s) { s.kneeDb = -6.0; }).hasValue());
    CHECK_FALSE(bad([](CompressorSettings& s) {
                    s.thresholdDb = std::numeric_limits<double>::quiet_NaN();
                }).hasValue());

    const Result<Compressor> rejected = bad([](CompressorSettings& s) { s.ratio = 0.25; });
    REQUIRE_FALSE(rejected.hasValue());
    CHECK(rejected.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("A signal below the threshold passes through untouched", "[dsp][dynamics][compressor]") {
    Compressor compressor = makeCompressor(CompressorSettings{.thresholdDb = -20.0,
                                                              .ratio = 4.0,
                                                              .attackSeconds = 0.005,
                                                              .releaseSeconds = 0.050,
                                                              .kneeDb = 0.0,
                                                              .makeupGainDb = 0.0});

    // Bit-identical, not merely close: with no gain to apply the compressor
    // must be arithmetically out of the way.
    const std::vector<float> signal = squareWave(-30.0, 100.0, samplesFor(0.5));
    for (float sample : signal) {
        REQUIRE(compressor.processSample(sample) == sample);
    }
    CHECK(compressor.gainReductionDb() == Approx(0.0).margin(1e-12));
}

TEST_CASE("A steady signal above the threshold is reduced by exactly the ratio",
          "[dsp][dynamics][compressor]") {
    struct Case {
        double levelDb;
        double thresholdDb;
        double ratio;
        double kneeDb;
        double makeupDb;
    };

    const Case cases[] = {
        {-10.0, -20.0, 4.0, 0.0, 0.0},  // straightforward 10 dB over, 4:1
        {-6.0, -30.0, 2.0, 0.0, 0.0},   // 24 dB over, gentle
        {-3.0, -40.0, 20.0, 0.0, 0.0},  // nearly limiting
        {-10.0, -20.0, 4.0, 0.0, 6.0},  // with makeup gain
        {-20.0, -20.0, 4.0, 12.0, 0.0}, // sitting in the middle of a soft knee
        {-23.0, -20.0, 4.0, 12.0, 0.0}, // lower half of the knee
        {-18.0, -20.0, 4.0, 12.0, 0.0}, // upper half of the knee
        {-30.0, -20.0, 4.0, 0.0, 0.0},  // below: no reduction at all
    };

    for (const Case& test : cases) {
        CompressorSettings settings{.thresholdDb = test.thresholdDb,
                                    .ratio = test.ratio,
                                    .attackSeconds = 0.001,
                                    .releaseSeconds = 0.001,
                                    .kneeDb = test.kneeDb,
                                    .makeupGainDb = test.makeupDb};
        Compressor compressor = makeCompressor(settings);

        const std::vector<float> signal = squareWave(test.levelDb, 200.0, samplesFor(0.5));
        const double peak = settledPeak(compressor, signal);

        const double expectedGain =
            expectedCompressorGainDb(test.levelDb, test.thresholdDb, test.ratio, test.kneeDb);
        INFO("level " << test.levelDb << " dB, threshold " << test.thresholdDb << " dB, ratio "
                      << test.ratio << ":1, knee " << test.kneeDb << " dB");
        CHECK(compressor.gainReductionDb() == Approx(-expectedGain).margin(1e-6));
        CHECK(toDecibels(peak) == Approx(test.levelDb + expectedGain + test.makeupDb).margin(1e-4));
    }
}

TEST_CASE("The static curve is the published soft-knee curve", "[dsp][dynamics][compressor]") {
    // Swept across the knee rather than sampled at three points: the knee is a
    // quadratic splice, and a splice that is continuous in value but not in
    // slope passes any three-point check while sounding like a different
    // compressor either side of the threshold.
    const CompressorSettings settings{.thresholdDb = -18.0,
                                      .ratio = 6.0,
                                      .attackSeconds = 0.01,
                                      .releaseSeconds = 0.1,
                                      .kneeDb = 9.0,
                                      .makeupGainDb = 0.0};

    double previousSlope = 0.0;
    for (int step = 0; step <= 600; ++step) {
        const double levelDb = -60.0 + static_cast<double>(step) * 0.1;
        const double expected = expectedCompressorGainDb(levelDb, settings.thresholdDb,
                                                         settings.ratio, settings.kneeDb);
        INFO("level " << levelDb);
        REQUIRE(compressorGainDb(settings, levelDb) == Approx(expected).margin(1e-9));

        const double slope = (compressorGainDb(settings, levelDb + 1e-4) -
                              compressorGainDb(settings, levelDb - 1e-4)) /
                             2e-4;
        if (step > 0) {
            REQUIRE(std::abs(slope - previousSlope) < 0.02);
        }
        previousSlope = slope;
    }

    // The ends of the curve are the ones a specification states outright.
    CHECK(compressorGainDb(settings, -60.0) == Approx(0.0));
    CHECK(compressorGainDb(settings, 0.0) == Approx(18.0 / 6.0 - 18.0));
}

TEST_CASE("Attack and release take the time they are configured for",
          "[dsp][dynamics][compressor]") {
    const double attackSeconds = 0.020;
    const double releaseSeconds = 0.050;
    Compressor compressor = makeCompressor(CompressorSettings{.thresholdDb = -20.0,
                                                              .ratio = 4.0,
                                                              .attackSeconds = attackSeconds,
                                                              .releaseSeconds = releaseSeconds,
                                                              .kneeDb = 0.0,
                                                              .makeupGainDb = 0.0});

    const double settled = 10.0 * (1.0 - 1.0 / 4.0); // 10 dB over at 4:1
    const std::vector<float> loud = squareWave(-10.0, 500.0, samplesFor(2.0));
    const std::vector<float> quiet = squareWave(-40.0, 500.0, samplesFor(2.0));

    for (SampleCount i = 0; i < samplesFor(attackSeconds); ++i) {
        static_cast<void>(compressor.processSample(loud[static_cast<std::size_t>(i)]));
    }
    CHECK(compressor.gainReductionDb() == Approx(settled * (1.0 - std::exp(-1.0))).margin(0.02));

    for (SampleCount i = samplesFor(attackSeconds); i < samplesFor(1.0); ++i) {
        static_cast<void>(compressor.processSample(loud[static_cast<std::size_t>(i)]));
    }
    CHECK(compressor.gainReductionDb() == Approx(settled).margin(1e-6));

    for (SampleCount i = 0; i < samplesFor(releaseSeconds); ++i) {
        static_cast<void>(compressor.processSample(quiet[static_cast<std::size_t>(i)]));
    }
    CHECK(compressor.gainReductionDb() == Approx(settled * std::exp(-1.0)).margin(0.02));
}

TEST_CASE("Zero attack and release are instantaneous, not frozen", "[dsp][dynamics][compressor]") {
    Compressor compressor = makeCompressor(CompressorSettings{.thresholdDb = -20.0,
                                                              .ratio = 4.0,
                                                              .attackSeconds = 0.0,
                                                              .releaseSeconds = 0.0,
                                                              .kneeDb = 0.0,
                                                              .makeupGainDb = 0.0});

    const auto loud = static_cast<float>(toGain(-10.0));
    static_cast<void>(compressor.processSample(loud));
    CHECK(compressor.gainReductionDb() == Approx(7.5).margin(1e-9));

    static_cast<void>(compressor.processSample(static_cast<float>(toGain(-40.0))));
    CHECK(compressor.gainReductionDb() == Approx(0.0).margin(1e-9));
}

TEST_CASE("A ratio of one is a wire and an enormous ratio is a limiter",
          "[dsp][dynamics][compressor]") {
    Compressor transparent = makeCompressor(CompressorSettings{.thresholdDb = -40.0,
                                                               .ratio = 1.0,
                                                               .attackSeconds = 0.0,
                                                               .releaseSeconds = 0.0,
                                                               .kneeDb = 0.0,
                                                               .makeupGainDb = 0.0});
    const std::vector<float> signal = squareWave(-6.0, 100.0, samplesFor(0.1));
    for (float sample : signal) {
        REQUIRE(transparent.processSample(sample) == sample);
    }

    Compressor brickwall = makeCompressor(CompressorSettings{.thresholdDb = -20.0,
                                                             .ratio = 100000.0,
                                                             .attackSeconds = 0.0,
                                                             .releaseSeconds = 0.0,
                                                             .kneeDb = 0.0,
                                                             .makeupGainDb = 0.0});
    const double peak = settledPeak(brickwall, signal);
    // Everything over the threshold is squeezed into the last hundred-thousandth
    // of a decibel, so the output sits on the threshold.
    CHECK(toDecibels(peak) == Approx(-20.0).margin(0.01));
}

TEST_CASE("Compressor settings can be replaced after construction", "[dsp][dynamics][compressor]") {
    Compressor compressor = makeCompressor(CompressorSettings{});
    CHECK_FALSE(compressor.setSettings(CompressorSettings{.ratio = 0.1}).ok());
    // The rejected change must leave the processor exactly as it was.
    CHECK(compressor.settings().ratio == Approx(CompressorSettings{}.ratio));

    REQUIRE(compressor
                .setSettings(CompressorSettings{.thresholdDb = -12.0,
                                                .ratio = 8.0,
                                                .attackSeconds = 0.0,
                                                .releaseSeconds = 0.0,
                                                .kneeDb = 0.0,
                                                .makeupGainDb = 0.0})
                .ok());
    static_cast<void>(compressor.processSample(static_cast<float>(toGain(-4.0))));
    CHECK(compressor.gainReductionDb() == Approx(8.0 * (1.0 - 1.0 / 8.0)).margin(1e-9));
}

// ---------------------------------------------------------------------------
// Expander
// ---------------------------------------------------------------------------

TEST_CASE("Expander parameters are validated", "[dsp][dynamics][expander]") {
    CHECK(Expander::create(kSampleRate48000).hasValue());
    CHECK_FALSE(Expander::create(SampleRate{0.0}).hasValue());
    CHECK_FALSE(Expander::create(kSampleRate48000, ExpanderSettings{.ratio = 0.5}).hasValue());
    CHECK_FALSE(
        Expander::create(kSampleRate48000, ExpanderSettings{.attackSeconds = -1.0}).hasValue());
    CHECK_FALSE(
        Expander::create(kSampleRate48000, ExpanderSettings{.detectorSeconds = -1.0}).hasValue());
}

TEST_CASE("An expander leaves signal above its threshold alone", "[dsp][dynamics][expander]") {
    Expander expander = makeExpander(ExpanderSettings{.thresholdDb = -40.0,
                                                      .ratio = 2.0,
                                                      .attackSeconds = 0.005,
                                                      .releaseSeconds = 0.050,
                                                      .kneeDb = 0.0,
                                                      .makeupGainDb = 0.0,
                                                      .detectorSeconds = 0.0});

    const std::vector<float> signal = squareWave(-20.0, 200.0, samplesFor(0.2));
    for (float sample : signal) {
        REQUIRE(expander.processSample(sample) == sample);
    }
    CHECK(expander.gainReductionDb() == Approx(0.0).margin(1e-12));
}

TEST_CASE("An expander pushes down what is under its threshold", "[dsp][dynamics][expander]") {
    struct Case {
        double levelDb;
        double thresholdDb;
        double ratio;
        double kneeDb;
    };

    const Case cases[] = {
        {-50.0, -40.0, 2.0, 0.0}, {-60.0, -40.0, 3.0, 0.0}, {-45.0, -40.0, 4.0, 0.0},
        {-40.0, -40.0, 2.0, 8.0}, {-43.0, -40.0, 2.0, 8.0}, {-37.0, -40.0, 2.0, 8.0},
    };

    for (const Case& test : cases) {
        Expander expander = makeExpander(ExpanderSettings{.thresholdDb = test.thresholdDb,
                                                          .ratio = test.ratio,
                                                          .attackSeconds = 0.001,
                                                          .releaseSeconds = 0.001,
                                                          .kneeDb = test.kneeDb,
                                                          .makeupGainDb = 0.0,
                                                          .detectorSeconds = 0.0});

        const std::vector<float> signal = squareWave(test.levelDb, 200.0, samplesFor(0.5));
        const double peak = settledPeak(expander, signal);
        const double expected =
            expectedExpanderGainDb(test.levelDb, test.thresholdDb, test.ratio, test.kneeDb);

        INFO("level " << test.levelDb << " dB, threshold " << test.thresholdDb
                      << " dB, ratio 1:" << test.ratio << ", knee " << test.kneeDb << " dB");
        CHECK(expander.gainReductionDb() == Approx(-expected).margin(1e-6));
        CHECK(toDecibels(peak) == Approx(test.levelDb + expected).margin(1e-4));
    }
}

TEST_CASE("An expander's attack is the time it takes to open", "[dsp][dynamics][expander]") {
    // The opposite convention to the compressor, and the one a user expects
    // from the control: attack opens the expander, release closes it.
    const double attackSeconds = 0.020;
    const double releaseSeconds = 0.050;
    Expander expander = makeExpander(ExpanderSettings{.thresholdDb = -40.0,
                                                      .ratio = 2.0,
                                                      .attackSeconds = attackSeconds,
                                                      .releaseSeconds = releaseSeconds,
                                                      .kneeDb = 0.0,
                                                      .makeupGainDb = 0.0,
                                                      .detectorSeconds = 0.0});

    const std::vector<float> quiet = squareWave(-60.0, 500.0, samplesFor(1.0));
    const std::vector<float> loud = squareWave(-20.0, 500.0, samplesFor(1.0));
    const double closed = 20.0; // 20 dB under threshold at 1:2

    for (float sample : quiet) {
        static_cast<void>(expander.processSample(sample));
    }
    REQUIRE(expander.gainReductionDb() == Approx(closed).margin(1e-6));

    for (SampleCount i = 0; i < samplesFor(attackSeconds); ++i) {
        static_cast<void>(expander.processSample(loud[static_cast<std::size_t>(i)]));
    }
    CHECK(expander.gainReductionDb() == Approx(closed * std::exp(-1.0)).margin(0.02));

    for (SampleCount i = samplesFor(attackSeconds); i < samplesFor(1.0); ++i) {
        static_cast<void>(expander.processSample(loud[static_cast<std::size_t>(i)]));
    }
    REQUIRE(expander.gainReductionDb() == Approx(0.0).margin(1e-6));

    for (SampleCount i = 0; i < samplesFor(releaseSeconds); ++i) {
        static_cast<void>(expander.processSample(quiet[static_cast<std::size_t>(i)]));
    }
    CHECK(expander.gainReductionDb() == Approx(closed * (1.0 - std::exp(-1.0))).margin(0.02));
}

TEST_CASE("The expander's detector stops it chattering on a sine", "[dsp][dynamics][expander]") {
    // A sine crosses zero twice a cycle, so a threshold comparison made on raw
    // samples reverses itself hundreds of times a second and modulates the gain
    // at twice the signal frequency. The peak detector is what makes the
    // decision once per envelope instead. What is measured is the peak-to-peak
    // swing of the gain reduction after everything has settled: that swing is
    // the chatter.
    std::vector<float> sine(static_cast<std::size_t>(samplesFor(0.5)));
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = static_cast<float>(toGain(-34.0) * std::sin(2.0 * std::numbers::pi * 100.0 *
                                                              static_cast<double>(i) / kRate));
    }

    const auto ripple = [&sine](double detectorSeconds) {
        Expander expander = makeExpander(ExpanderSettings{.thresholdDb = -40.0,
                                                          .ratio = 4.0,
                                                          .attackSeconds = 0.001,
                                                          .releaseSeconds = 0.010,
                                                          .kneeDb = 0.0,
                                                          .makeupGainDb = 0.0,
                                                          .detectorSeconds = detectorSeconds});
        double lowest = 1e9;
        double highest = -1e9;
        for (std::size_t i = 0; i < sine.size(); ++i) {
            static_cast<void>(expander.processSample(sine[i]));
            if (i >= sine.size() / 2) {
                lowest = std::min(lowest, expander.gainReductionDb());
                highest = std::max(highest, expander.gainReductionDb());
            }
        }
        return highest - lowest;
    };

    const double withDetector = ripple(0.030);
    const double without = ripple(0.0);
    INFO("with detector " << withDetector << " dB, without " << without << " dB");
    // With a detector the envelope never dips below the threshold at all, so
    // the gain does not move. Without one the same signal swings it by several
    // decibels at 200 Hz, which is heard as a buzz on top of the note.
    CHECK(withDetector < 0.1);
    CHECK(without > 3.0);
}

// ---------------------------------------------------------------------------
// Gate
// ---------------------------------------------------------------------------

TEST_CASE("Gate parameters are validated", "[dsp][dynamics][gate]") {
    CHECK(Gate::create(kSampleRate48000).hasValue());
    CHECK_FALSE(Gate::create(SampleRate{0.0}).hasValue());
    CHECK_FALSE(Gate::create(kSampleRate48000, GateSettings{.hysteresisDb = -1.0}).hasValue());
    CHECK_FALSE(Gate::create(kSampleRate48000, GateSettings{.holdSeconds = -1.0}).hasValue());
    // A positive range would be a gate that turns the noise up.
    CHECK_FALSE(Gate::create(kSampleRate48000, GateSettings{.rangeDb = 6.0}).hasValue());

    // Settings are checked before anything is built from them. The hold time
    // becomes a sample count during construction, and converting a NaN to an
    // integer is undefined -- so this must be rejected, not merely survived.
    const double notANumber = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(Gate::create(kSampleRate48000, GateSettings{.holdSeconds = notANumber}).hasValue());
    CHECK_FALSE(
        Gate::create(kSampleRate48000, GateSettings{.detectorSeconds = notANumber}).hasValue());
    CHECK_FALSE(Gate::create(kSampleRate48000, GateSettings{.rangeDb = notANumber}).hasValue());
    CHECK_FALSE(Limiter::create(kSampleRate48000, LimiterSettings{.lookAheadSeconds = notANumber})
                    .hasValue());
    CHECK_FALSE(Expander::create(kSampleRate48000, ExpanderSettings{.detectorSeconds = notANumber})
                    .hasValue());
}

TEST_CASE("The gate opens and closes at the thresholds it was given", "[dsp][dynamics][gate]") {
    const double thresholdDb = -30.0;
    const double hysteresisDb = 6.0;
    const double rangeDb = -80.0;
    Gate gate = makeGate(GateSettings{.thresholdDb = thresholdDb,
                                      .hysteresisDb = hysteresisDb,
                                      .attackSeconds = 0.0,
                                      .holdSeconds = 0.0,
                                      .releaseSeconds = 0.0,
                                      .rangeDb = rangeDb,
                                      .detectorSeconds = 0.0});

    const auto runAt = [&gate](double levelDb) {
        const std::vector<float> signal = squareWave(levelDb, 500.0, samplesFor(0.05));
        double peak = 0.0;
        for (float sample : signal) {
            peak = std::max(peak, static_cast<double>(std::abs(gate.processSample(sample))));
        }
        return toDecibels(peak) - levelDb;
    };

    // Starts closed: a gate that starts open lets through exactly the noise it
    // was inserted to remove, for one release period after every seek.
    CHECK_FALSE(gate.isOpen());

    CHECK(runAt(-25.0) == Approx(0.0).margin(1e-4)); // above the open threshold
    CHECK(gate.isOpen());

    // Inside the hysteresis band: below the open threshold but not yet below
    // the close threshold, so nothing changes.
    CHECK(runAt(-34.0) == Approx(0.0).margin(1e-4));
    CHECK(gate.isOpen());

    CHECK(runAt(-40.0) == Approx(rangeDb).margin(1e-4)); // below the close threshold
    CHECK_FALSE(gate.isOpen());

    // Back inside the hysteresis band from below: still shut.
    CHECK(runAt(-32.0) == Approx(rangeDb).margin(1e-4));
    CHECK_FALSE(gate.isOpen());

    CHECK(runAt(-28.0) == Approx(0.0).margin(1e-4));
    CHECK(gate.isOpen());
}

TEST_CASE("Hysteresis stops the gate chattering", "[dsp][dynamics][gate]") {
    // A level wobbling a fifth of a decibel either side of the threshold: a
    // gate without hysteresis switches on every wobble, and the result is the
    // stuttering that makes naive gates unusable on a decaying tail.
    std::vector<float> signal(static_cast<std::size_t>(samplesFor(1.0)));
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const double envelope =
            toGain(-30.0) * (1.0 + 0.02 * std::sin(2.0 * std::numbers::pi * 200.0 *
                                                   static_cast<double>(i) / kRate));
        const bool high = ((static_cast<SampleCount>(i) / 24) % 2) == 0;
        signal[i] = static_cast<float>(high ? envelope : -envelope);
    }

    const auto transitions = [&signal](double hysteresisDb) {
        Gate gate = makeGate(GateSettings{.thresholdDb = -30.0,
                                          .hysteresisDb = hysteresisDb,
                                          .attackSeconds = 0.0,
                                          .holdSeconds = 0.0,
                                          .releaseSeconds = 0.0,
                                          .rangeDb = -60.0,
                                          .detectorSeconds = 0.0});
        int count = 0;
        bool previous = gate.isOpen();
        for (float sample : signal) {
            static_cast<void>(gate.processSample(sample));
            if (gate.isOpen() != previous) {
                ++count;
                previous = gate.isOpen();
            }
        }
        return count;
    };

    CHECK(transitions(0.0) > 100);
    CHECK(transitions(3.0) == 1);
}

TEST_CASE("Hold keeps the gate open across a short dropout", "[dsp][dynamics][gate]") {
    Gate gate = makeGate(GateSettings{.thresholdDb = -30.0,
                                      .hysteresisDb = 0.0,
                                      .attackSeconds = 0.0,
                                      .holdSeconds = 0.010,
                                      .releaseSeconds = 0.0,
                                      .rangeDb = -80.0,
                                      .detectorSeconds = 0.0});

    const auto feed = [&gate](const std::vector<float>& block) {
        for (float sample : block) {
            static_cast<void>(gate.processSample(sample));
        }
    };

    feed(squareWave(-20.0, 500.0, samplesFor(0.050)));
    REQUIRE(gate.isOpen());

    // Five milliseconds of nothing, inside the ten-millisecond hold.
    feed(std::vector<float>(static_cast<std::size_t>(samplesFor(0.005)), 0.0f));
    CHECK(gate.isOpen());

    feed(squareWave(-20.0, 500.0, samplesFor(0.005)));
    CHECK(gate.isOpen());

    // Thirty milliseconds is well past it.
    feed(std::vector<float>(static_cast<std::size_t>(samplesFor(0.030)), 0.0f));
    CHECK_FALSE(gate.isOpen());
}

TEST_CASE("Gate attack and release take their configured time", "[dsp][dynamics][gate]") {
    const double attackSeconds = 0.010;
    const double releaseSeconds = 0.040;
    const double rangeDb = -40.0;
    Gate gate = makeGate(GateSettings{.thresholdDb = -30.0,
                                      .hysteresisDb = 0.0,
                                      .attackSeconds = attackSeconds,
                                      .holdSeconds = 0.0,
                                      .releaseSeconds = releaseSeconds,
                                      .rangeDb = rangeDb,
                                      .detectorSeconds = 0.0});

    const std::vector<float> loud = squareWave(-10.0, 500.0, samplesFor(1.0));
    REQUIRE(gate.gainReductionDb() == Approx(-rangeDb).margin(1e-9));

    for (SampleCount i = 0; i < samplesFor(attackSeconds); ++i) {
        static_cast<void>(gate.processSample(loud[static_cast<std::size_t>(i)]));
    }
    CHECK(gate.gainReductionDb() == Approx(-rangeDb * std::exp(-1.0)).margin(0.02));

    for (SampleCount i = samplesFor(attackSeconds); i < samplesFor(1.0); ++i) {
        static_cast<void>(gate.processSample(loud[static_cast<std::size_t>(i)]));
    }
    REQUIRE(gate.gainReductionDb() == Approx(0.0).margin(1e-6));

    for (SampleCount i = 0; i < samplesFor(releaseSeconds); ++i) {
        static_cast<void>(gate.processSample(0.0f));
    }
    CHECK(gate.gainReductionDb() == Approx(-rangeDb * (1.0 - std::exp(-1.0))).margin(0.02));
}

// ---------------------------------------------------------------------------
// Limiter
// ---------------------------------------------------------------------------

TEST_CASE("Limiter parameters are validated", "[dsp][dynamics][limiter]") {
    CHECK(Limiter::create(kSampleRate48000).hasValue());
    CHECK_FALSE(Limiter::create(SampleRate{0.0}).hasValue());
    CHECK_FALSE(
        Limiter::create(kSampleRate48000, LimiterSettings{.lookAheadSeconds = -0.001}).hasValue());
    CHECK_FALSE(
        Limiter::create(kSampleRate48000,
                        LimiterSettings{.lookAheadSeconds = Limiter::kMaxLookAheadSeconds + 0.001})
            .hasValue());
    CHECK_FALSE(
        Limiter::create(kSampleRate48000, LimiterSettings{.releaseSeconds = -1.0}).hasValue());
}

TEST_CASE("The limiter never lets anything past its ceiling", "[dsp][dynamics][limiter]") {
    // The one property that actually matters. Everything else about a limiter
    // is taste; this is the contract.
    for (double ceilingDb : {0.0, -0.3, -1.0, -6.0}) {
        for (double lookAheadSeconds : {0.0, 0.001, 0.005, Limiter::kMaxLookAheadSeconds}) {
            Limiter limiter = makeLimiter(LimiterSettings{.ceilingDb = ceilingDb,
                                                          .releaseSeconds = 0.050,
                                                          .lookAheadSeconds = lookAheadSeconds});

            std::vector<float> signal =
                whiteNoise(static_cast<std::size_t>(samplesFor(0.5)), 7, 4.0f);
            // A full-scale transient out of nowhere, the case that catches a
            // limiter whose envelope is the only thing holding the ceiling.
            signal[1000] = 12.0f;
            signal[1001] = -12.0f;
            // A sudden sustained step, which an envelope handles differently
            // from a lone spike.
            for (std::size_t i = 5000; i < 9000; ++i) {
                signal[i] = 8.0f;
            }
            // Alternating full scale: the fastest a sampled signal can move.
            for (std::size_t i = 12000; i < 13000; ++i) {
                signal[i] = (i % 2 == 0) ? 6.0f : -6.0f;
            }

            const double ceiling = toGain(ceilingDb);
            const double peak = peakOf(limiter, signal);

            INFO("ceiling " << ceilingDb << " dB, look-ahead " << lookAheadSeconds << " s");
            // The only slack is the rounding of the final multiply into float32.
            CHECK(peak <= ceiling * (1.0 + 1e-6));
            // And it must actually be working, not simply muting everything.
            CHECK(peak > ceiling * 0.99);
        }
    }
}

TEST_CASE("Below the ceiling the limiter is a delay line and nothing else",
          "[dsp][dynamics][limiter]") {
    Limiter limiter = makeLimiter(
        LimiterSettings{.ceilingDb = -0.3, .releaseSeconds = 0.050, .lookAheadSeconds = 0.005});
    const SampleCount latency = limiter.latencySamples();
    CHECK(latency == samplesFor(0.005));

    std::vector<float> signal(static_cast<std::size_t>(samplesFor(0.05)));
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRate));
    }

    std::vector<float> output(signal.size());
    limiter.process(signal.data(), output.data(), static_cast<SampleCount>(signal.size()));

    for (SampleCount i = 0; i < latency; ++i) {
        REQUIRE(output[static_cast<std::size_t>(i)] == 0.0f);
    }
    for (std::size_t i = static_cast<std::size_t>(latency); i < signal.size(); ++i) {
        // Bit-identical: below the ceiling the gain is exactly one.
        REQUIRE(output[i] == signal[i - static_cast<std::size_t>(latency)]);
    }
}

TEST_CASE("Look-ahead is what keeps the gain from being yanked", "[dsp][dynamics][limiter]") {
    // With look-ahead the limiter starts turning down before the transient
    // arrives, so the gain moves in small steps. Without it the gain can only
    // move once the peak is already at the output, and the whole reduction
    // happens in a single sample -- which is audible as a click on the
    // transient it was supposed to be controlling.
    const auto largestStep = [](double lookAheadSeconds) {
        Limiter limiter = makeLimiter(LimiterSettings{
            .ceilingDb = -0.3, .releaseSeconds = 0.100, .lookAheadSeconds = lookAheadSeconds});
        std::vector<float> signal(static_cast<std::size_t>(samplesFor(0.05)), 0.1f);
        for (std::size_t i = 2000; i < 2400; ++i) {
            signal[i] = 4.0f;
        }

        double previous = 0.0;
        double largest = 0.0;
        for (float sample : signal) {
            static_cast<void>(limiter.processSample(sample));
            largest = std::max(largest, std::abs(limiter.gainReductionDb() - previous));
            previous = limiter.gainReductionDb();
        }
        return largest;
    };

    CHECK(largestStep(0.005) < 0.5);
    CHECK(largestStep(0.0) > 5.0);
}

TEST_CASE("Limiter reset clears the delay line and the envelope", "[dsp][dynamics][limiter]") {
    Limiter limiter = makeLimiter(
        LimiterSettings{.ceilingDb = -0.3, .releaseSeconds = 0.050, .lookAheadSeconds = 0.005});

    const std::vector<float> block = whiteNoise(2048, 19, 3.0f);
    std::vector<float> first(block.size());
    std::vector<float> second(block.size());
    const auto count = static_cast<SampleCount>(block.size());

    limiter.process(block.data(), first.data(), count);
    limiter.process(block.data(), second.data(), count);
    limiter.reset();
    limiter.process(block.data(), second.data(), count);

    for (std::size_t i = 0; i < block.size(); ++i) {
        INFO("sample " << i);
        REQUIRE(second[i] == first[i]);
    }
}

// ---------------------------------------------------------------------------
// Shared properties
// ---------------------------------------------------------------------------

TEST_CASE("Degenerate dynamics blocks are no-ops", "[dsp][dynamics]") {
    float sentinel = 0.5f;

    Compressor compressor = makeCompressor(CompressorSettings{});
    compressor.process(&sentinel, &sentinel, 0);
    compressor.process(&sentinel, &sentinel, -4);

    Expander expander = makeExpander(ExpanderSettings{});
    expander.process(&sentinel, &sentinel, 0);

    Gate gate = makeGate(GateSettings{});
    gate.process(&sentinel, &sentinel, 0);

    Limiter limiter = makeLimiter(LimiterSettings{});
    limiter.process(&sentinel, &sentinel, 0);

    CHECK(sentinel == 0.5f);
}

TEST_CASE("Dynamics processing on the audio thread allocates nothing", "[dsp][dynamics][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    Compressor compressor = makeCompressor(CompressorSettings{});
    Expander expander = makeExpander(ExpanderSettings{});
    Gate gate = makeGate(GateSettings{});
    Limiter limiter = makeLimiter(LimiterSettings{});
    std::vector<float> block = whiteNoise(512, 3, 1.5f);
    std::vector<float> output(block.size());

    std::size_t allocations = 0;
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;

        const auto count = static_cast<SampleCount>(block.size());
        compressor.process(block.data(), output.data(), count);
        expander.processInPlace(output.data(), count);
        gate.processInPlace(output.data(), count);
        limiter.process(output.data(), output.data(), count);

        compressor.reset();
        expander.reset();
        gate.reset();
        limiter.reset();

        static_cast<void>(compressor.gainReductionDb());
        static_cast<void>(limiter.gainReductionDb());

        allocations = scope.count();
    }
    CHECK(allocations == 0);
}
