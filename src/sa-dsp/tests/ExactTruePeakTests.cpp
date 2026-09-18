#include <sa/dsp/ExactTruePeak.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

/// A tone at `ratio` of the sample rate with a chosen starting phase, faded in
/// and out.
///
/// The phase matters because a tone sampled on its crests hides everything an
/// interpolator gets wrong. The fade matters for a subtler reason: a tone that
/// starts abruptly at full level is a step, and the band-limited reconstruction
/// of a step overshoots. That overshoot is real -- a file that begins at full
/// amplitude genuinely has a higher true peak at its first sample than anywhere
/// else -- so a test that wants to measure the tone must not also be measuring
/// its own beginning. There is a test below for the edge behaviour itself.
std::vector<float> tone(SampleCount count, double ratio, double amplitude, double phase,
                        SampleCount fade = 2000) {
    std::vector<float> out(static_cast<std::size_t>(count));
    for (SampleCount i = 0; i < count; ++i) {
        const double envelope =
            fade <= 0 ? 1.0
                      : std::min({1.0, static_cast<double>(i) / static_cast<double>(fade),
                                  static_cast<double>(count - 1 - i) / static_cast<double>(fade)});
        out[static_cast<std::size_t>(i)] = static_cast<float>(
            amplitude * envelope *
            std::sin(2.0 * std::numbers::pi * ratio * static_cast<double>(i) + phase));
    }
    return out;
}

double samplePeak(const std::vector<float>& samples) {
    double peak = 0.0;
    for (const float value : samples) {
        peak = std::max(peak, std::abs(static_cast<double>(value)));
    }
    return peak;
}

} // namespace

TEST_CASE("It finds a peak that falls between samples", "[dsp][truepeak][exact]") {
    // A quarter-rate tone shifted by an eighth of a cycle never lands on its own
    // crest: every sample sits at A/sqrt(2). Anything that reports the sample
    // peak here is not measuring true peak at all.
    const auto samples = tone(65536, 0.25, 0.5, std::numbers::pi / 4.0);

    CHECK(samplePeak(samples) == Approx(0.5 / std::numbers::sqrt2).margin(1e-5));

    auto measured = exactTruePeak(samples.data(), static_cast<SampleCount>(samples.size()));
    REQUIRE(measured.hasValue());
    CHECK(measured.value() == Approx(0.5).margin(1e-4));
}

TEST_CASE("It stays accurate where a polyphase filter droops", "[dsp][truepeak][exact]") {
    // The frequencies where our real-time meter loses up to 0.44 dB. This has
    // no filter, so it should be right across the band and at every phase.
    for (const double ratio : {0.10, 0.25, 0.35, 0.40, 0.45, 0.47}) {
        double worst = 1000.0;
        for (int step = 0; step < 8; ++step) {
            const double phase = 2.0 * std::numbers::pi * step / 8.0;
            const auto samples = tone(32768, ratio, 0.5, phase);
            auto measured = exactTruePeak(samples.data(), static_cast<SampleCount>(samples.size()));
            REQUIRE(measured.hasValue());
            worst = std::min(worst, 20.0 * std::log10(measured.value() / 0.5));
        }
        INFO("ratio " << ratio << " worst error " << worst << " dB");
        CHECK(worst > -0.05);
    }
}

TEST_CASE("Its error is on the safe side of a ceiling", "[dsp][truepeak][exact]") {
    // Block edges ring a little, which biases the reading marginally high.
    // For anything deciding whether a master is under a ceiling that is the
    // direction to be wrong in, and it is small: measured at +0.002 to
    // +0.041 dB across the band.
    for (const double ratio : {0.25, 0.45, 0.47}) {
        const auto samples = tone(32768, ratio, 0.5, 0.3);
        auto measured = exactTruePeak(samples.data(), static_cast<SampleCount>(samples.size()));
        REQUIRE(measured.hasValue());
        const double error = 20.0 * std::log10(measured.value() / 0.5);
        INFO("ratio " << ratio << " error " << error << " dB");
        CHECK(error > -0.01);
        CHECK(error < 0.10);
    }
}

TEST_CASE("A file that starts at full level really does peak higher there",
          "[dsp][truepeak][exact]") {
    // Not a flaw to be smoothed away. A signal that is silent before its first
    // sample and full scale at it is a step, and the band-limited
    // reconstruction of a step overshoots -- so the true peak of that file is
    // genuinely above its sample peak, at its own beginning.
    //
    // This is measured rather than assumed because an earlier version of the
    // implementation kept the outer edges of its first and last analysis blocks
    // and read 4.17 dB high, which looked like exactly this and was not.
    const auto abrupt = tone(32768, 0.45, 0.5, 0.3, /*fade=*/0);
    const auto faded = tone(32768, 0.45, 0.5, 0.3);

    auto hard = exactTruePeak(abrupt.data(), static_cast<SampleCount>(abrupt.size()));
    auto soft = exactTruePeak(faded.data(), static_cast<SampleCount>(faded.size()));
    REQUIRE(hard.hasValue());
    REQUIRE(soft.hasValue());

    const double hardDb = 20.0 * std::log10(hard.value() / 0.5);
    const double softDb = 20.0 * std::log10(soft.value() / 0.5);
    INFO("abrupt " << hardDb << " dB, faded " << softDb << " dB");

    CHECK(softDb < 0.10);         // The tone itself is measured exactly.
    CHECK(hardDb > softDb + 0.5); // And the step is visible above it.
    CHECK(hardDb < 3.0);          // But it is a step, not a bug: measured 1.15 dB.
}

TEST_CASE("Silence and degenerate input are handled rather than crashed on",
          "[dsp][truepeak][exact]") {
    const std::vector<float> silence(16384, 0.0f);
    auto quiet = exactTruePeak(silence.data(), static_cast<SampleCount>(silence.size()));
    REQUIRE(quiet.hasValue());
    CHECK(quiet.value() == Approx(0.0).margin(1e-9));

    auto quietDb = exactTruePeakDbtp(silence.data(), static_cast<SampleCount>(silence.size()));
    REQUIRE(quietDb.hasValue());
    CHECK(quietDb.value() <= -199.0);

    CHECK_FALSE(exactTruePeak(nullptr, 100).hasValue());
    CHECK(exactTruePeak(silence.data(), 0).value() == Approx(0.0));
    CHECK_FALSE(exactTruePeak(silence.data(), 1000, 1).hasValue());
    CHECK_FALSE(exactTruePeak(silence.data(), 1000, 3).hasValue());
    CHECK_FALSE(exactTruePeak(silence.data(), 1000, 128).hasValue());

    // Shorter than one analysis block: the sample peak is the honest answer,
    // because there is not enough signal to reconstruct between.
    const auto brief = tone(500, 0.25, 0.4, 0.0);
    auto short_ = exactTruePeak(brief.data(), static_cast<SampleCount>(brief.size()));
    REQUIRE(short_.hasValue());
    CHECK(short_.value() == Approx(samplePeak(brief)).margin(1e-6));
}

TEST_CASE("A peak in the very last samples is not missed", "[dsp][truepeak][exact]") {
    // The block loop steps by half a block, so a tail shorter than that is not
    // covered by it. A meter that silently ignores the end of a file is worse
    // than no meter.
    std::vector<float> samples(10000, 0.0f);
    samples[9998] = 0.9f;

    auto measured = exactTruePeak(samples.data(), static_cast<SampleCount>(samples.size()));
    REQUIRE(measured.hasValue());
    // 0.9f is not exactly 0.9, so compare against what was actually stored.
    CHECK(measured.value() >= static_cast<double>(0.9f) - 1e-9);
}
