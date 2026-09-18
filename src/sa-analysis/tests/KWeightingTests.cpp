#include <sa/analysis/KWeighting.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <numbers>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

/// Magnitude response of a biquad at `frequency`, computed from the transfer
/// function rather than by running the filter -- so a coefficient error and a
/// difference-equation error cannot cancel each other out.
double magnitude(const BiquadCoefficients& c, double frequency, SampleRate rate) {
    const std::complex<double> z =
        std::exp(std::complex<double>{0.0, -2.0 * std::numbers::pi * frequency / rate.hz()});
    const std::complex<double> numerator = c.b0 + c.b1 * z + c.b2 * z * z;
    const std::complex<double> denominator = 1.0 + c.a1 * z + c.a2 * z * z;
    return std::abs(numerator / denominator);
}

double decibels(double magnitudeRatio) {
    return 20.0 * std::log10(magnitudeRatio);
}

/// The analogue corner frequency and Q a bilinear-transformed second-order
/// section came from, recovered from its denominator.
///
/// For every stage here the denominator is (1 + K/Q + K^2) normalised, so
/// (1 + a1 + a2) / (1 - a1 + a2) is exactly K^2 and the rest follows. Reading
/// the corner back out of the coefficients tests rate adaptation directly,
/// rather than inferring it from a magnitude probe -- which would be muddied by
/// the fact that BS.1770 leaves the RLB numerator un-normalised.
struct Corner {
    double frequencyHz = 0.0;
    double q = 0.0;
};

Corner recoverCorner(const BiquadCoefficients& c, SampleRate rate) {
    const double k = std::sqrt((1.0 + c.a1 + c.a2) / (1.0 - c.a1 + c.a2));
    const double a0 = 4.0 / (1.0 - c.a1 + c.a2);

    Corner corner;
    corner.frequencyHz = rate.hz() * std::atan(k) / std::numbers::pi;
    corner.q = k / (a0 - 1.0 - k * k);
    return corner;
}

KWeightingCoefficients coefficientsOrFail(SampleRate rate) {
    auto result = kWeightingFor(rate);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

/// K gain at `frequency`, both stages together.
double kGainDb(SampleRate rate, double frequency) {
    const auto c = coefficientsOrFail(rate);
    return decibels(magnitude(c.shelf, frequency, rate) * magnitude(c.highPass, frequency, rate));
}

} // namespace

TEST_CASE("The derivation reproduces the coefficients BS.1770-4 prints", "[analysis][kweighting]") {
    // The whole rate-adaptation argument rests on this. If solving the 48 kHz
    // table for an analogue prototype and transforming it back does not land on
    // the published numbers, the prototype is not the one the standard used and
    // every other rate is guesswork.
    const auto c = coefficientsOrFail(kSampleRate48000);

    CHECK(c.shelf.b0 == Approx(kReferenceShelfAt48kHz.b0).margin(1e-12));
    CHECK(c.shelf.b1 == Approx(kReferenceShelfAt48kHz.b1).margin(1e-12));
    CHECK(c.shelf.b2 == Approx(kReferenceShelfAt48kHz.b2).margin(1e-12));
    CHECK(c.shelf.a1 == Approx(kReferenceShelfAt48kHz.a1).margin(1e-12));
    CHECK(c.shelf.a2 == Approx(kReferenceShelfAt48kHz.a2).margin(1e-12));

    CHECK(c.highPass.b0 == kReferenceHighPassAt48kHz.b0);
    CHECK(c.highPass.b1 == kReferenceHighPassAt48kHz.b1);
    CHECK(c.highPass.b2 == kReferenceHighPassAt48kHz.b2);
    CHECK(c.highPass.a1 == Approx(kReferenceHighPassAt48kHz.a1).margin(1e-12));
    CHECK(c.highPass.a2 == Approx(kReferenceHighPassAt48kHz.a2).margin(1e-12));
}

TEST_CASE("Corner frequencies stay put as the sample rate changes", "[analysis][kweighting]") {
    // The failure this guards against is reusing the 48 kHz table everywhere,
    // which drags both corners along with the rate: at 96 kHz the RLB corner
    // would sit at 76 Hz rather than 38 Hz, and at 192 kHz at 153 Hz.
    for (double hz : {8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) {
        const SampleRate rate{hz};
        const auto c = coefficientsOrFail(rate);
        INFO("rate " << hz);

        const Corner highPass = recoverCorner(c.highPass, rate);
        CHECK(highPass.frequencyHz ==
              Approx(kKWeightingPrototype.highPassFrequencyHz).epsilon(1e-9));
        CHECK(highPass.q == Approx(kKWeightingPrototype.highPassQ).epsilon(1e-9));

        const Corner shelf = recoverCorner(c.shelf, rate);
        CHECK(shelf.frequencyHz == Approx(kKWeightingPrototype.shelfFrequencyHz).epsilon(1e-9));
        CHECK(shelf.q == Approx(kKWeightingPrototype.shelfQ).epsilon(1e-9));
    }
}

TEST_CASE("The RLB numerator is left un-normalised, as the standard has it",
          "[analysis][kweighting]") {
    // BS.1770-4 prints the stage 2 numerator as exactly (1, -2, 1) rather than
    // dividing it by a0 as a textbook design would. That leaves the stage with
    // a small passband gain of a0 -- about +0.043 dB at 48 kHz -- which is part
    // of what the -0.691 offset is cancelling. Normalising it "properly" would
    // shift every reading by that much, so it is pinned here.
    for (double hz : {44100.0, 48000.0, 96000.0}) {
        const SampleRate rate{hz};
        const auto c = coefficientsOrFail(rate);
        INFO("rate " << hz);
        CHECK(c.highPass.b0 == 1.0);
        CHECK(c.highPass.b1 == -2.0);
        CHECK(c.highPass.b2 == 1.0);
        // Well above the 38 Hz corner and well below Nyquist: the asymptote.
        const double a0 = 4.0 / (1.0 - c.highPass.a1 + c.highPass.a2);
        CHECK(magnitude(c.highPass, 2000.0, rate) == Approx(a0).epsilon(1e-3));
        CHECK(a0 > 1.0);
    }
}

TEST_CASE("The shelf reaches its +4 dB plateau at every rate", "[analysis][kweighting]") {
    for (double hz : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const SampleRate rate{hz};
        const auto c = coefficientsOrFail(rate);
        INFO("rate " << hz);
        // Well above the corner and well below Nyquist at every rate tested.
        CHECK(decibels(magnitude(c.shelf, 10000.0, rate)) ==
              Approx(kKWeightingPrototype.shelfGainDb).margin(0.05));
        CHECK(decibels(magnitude(c.shelf, 100.0, rate)) == Approx(0.0).margin(0.05));
    }
}

TEST_CASE("The -0.691 offset cancels the K gain at 1 kHz", "[analysis][kweighting]") {
    // This is why a 1 kHz tone reads its own dBFS value in LUFS. The offset and
    // the filter are a matched pair; changing one without the other silently
    // shifts every reading.
    for (double hz : {44100.0, 48000.0, 96000.0}) {
        INFO("rate " << hz);
        CHECK(kGainDb(SampleRate{hz}, 1000.0) == Approx(0.691).margin(0.02));
    }
}

TEST_CASE("Low frequencies are attenuated and highs lifted", "[analysis][kweighting]") {
    const SampleRate rate = kSampleRate48000;
    CHECK(kGainDb(rate, 20.0) < -12.0);
    CHECK(kGainDb(rate, 100.0) < 0.0);
    CHECK(kGainDb(rate, 500.0) == Approx(0.0).margin(0.2));
    CHECK(kGainDb(rate, 10000.0) > 3.5);
    CHECK(kGainDb(rate, 10000.0) < 4.5);
}

TEST_CASE("Both stages are stable at every supported rate", "[analysis][kweighting]") {
    // Jury's criterion for a second-order section: both poles inside the unit
    // circle. A derivation that went wrong at an extreme rate would show up
    // here as a filter that rings forever instead of as a subtly wrong number.
    for (double hz : {8000.0, 11025.0, 44100.0, 48000.0, 96000.0, 192000.0, 768000.0}) {
        const SampleRate rate{hz};
        const auto c = coefficientsOrFail(rate);
        INFO("rate " << hz);
        for (const BiquadCoefficients& stage : {c.shelf, c.highPass}) {
            CHECK(std::abs(stage.a2) < 1.0);
            CHECK(std::abs(stage.a1) < 1.0 + stage.a2);
        }
    }
}

TEST_CASE("Unusable sample rates are rejected", "[analysis][kweighting]") {
    CHECK_FALSE(kWeightingFor(SampleRate{0.0}).hasValue());
    CHECK_FALSE(kWeightingFor(SampleRate{-48000.0}).hasValue());
    CHECK_FALSE(kWeightingFor(SampleRate{1e9}).hasValue());
    // Above zero but below where the shelf corner stops making sense.
    CHECK_FALSE(kWeightingFor(SampleRate{4000.0}).hasValue());
    CHECK_FALSE(kWeightingFor(SampleRate{7999.0}).hasValue());
    CHECK(kWeightingFor(SampleRate{8000.0}).hasValue());
}

TEST_CASE("The difference equation delivers the transfer function's gain",
          "[analysis][kweighting]") {
    // Runs the filter for real and compares the steady-state amplitude with the
    // analytic response, which is what ties BiquadState to the coefficients.
    //
    // Amplitude is read from RMS over a whole number of cycles rather than from
    // the largest sample: at 8 kHz there are only six samples per cycle and
    // none of them need land near a peak.
    const SampleRate rate = kSampleRate48000;
    const auto c = coefficientsOrFail(rate);
    const int settle = 24000;
    const int measured = 24000;

    for (double frequency : {50.0, 1000.0, 8000.0}) {
        BiquadState shelf;
        BiquadState highPass;
        double sumOfSquares = 0.0;

        for (int i = 0; i < settle + measured; ++i) {
            const double x =
                std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate.hz());
            const double y = highPass.process(c.highPass, shelf.process(c.shelf, x));
            if (i >= settle) {
                sumOfSquares += y * y;
            }
        }

        const double observed = std::sqrt(2.0 * sumOfSquares / static_cast<double>(measured));
        const double expected =
            magnitude(c.shelf, frequency, rate) * magnitude(c.highPass, frequency, rate);
        INFO("frequency " << frequency);
        CHECK(observed == Approx(expected).epsilon(1e-6));
    }
}

TEST_CASE("Filter state resets to silence", "[analysis][kweighting]") {
    const auto c = coefficientsOrFail(kSampleRate48000);
    BiquadState state;

    for (int i = 0; i < 100; ++i) {
        static_cast<void>(state.process(c.shelf, 1.0));
    }
    state.reset();
    CHECK(state.process(c.shelf, 0.0) == 0.0);
}
