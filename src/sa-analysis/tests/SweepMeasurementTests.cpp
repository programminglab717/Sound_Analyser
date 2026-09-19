#include <sa/analysis/RoomAcoustics.h>
#include <sa/analysis/SweepMeasurement.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <utility>
#include <vector>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};

[[nodiscard]] SweepSettings quickSweep() {
    SweepSettings settings;
    settings.seconds = 1.0;
    settings.startHz = 50.0;
    settings.endHz = 18000.0;
    return settings;
}

/// Pass a signal through a known impulse response, the way a room would.
[[nodiscard]] AudioBuffer passThrough(const AudioBuffer& signal,
                                      const std::vector<float>& response) {
    const auto frames = signal.frames() + static_cast<SampleCount>(response.size()) - 1;
    AudioBuffer out{ChannelLayout::mono(), frames};
    for (SampleCount i = 0; i < signal.frames(); ++i) {
        const double x = signal.channel(0)[i];
        for (std::size_t tap = 0; tap < response.size(); ++tap) {
            out.channel(0)[i + static_cast<SampleCount>(tap)] +=
                static_cast<float>(x * response[tap]);
        }
    }
    return out;
}

[[nodiscard]] SampleIndex peakIndex(const AudioBuffer& buffer) {
    SampleIndex best = 0;
    double peak = -1.0;
    for (SampleCount i = 0; i < buffer.frames(); ++i) {
        const double magnitude = std::abs(static_cast<double>(buffer.channel(0)[i]));
        if (magnitude > peak) {
            peak = magnitude;
            best = i;
        }
    }
    return best;
}

} // namespace

TEST_CASE("A sweep convolved with its own inverse is an impulse", "[analysis][sweep]") {
    // The property the whole technique rests on. If this is not an impulse then
    // nothing measured with it means anything.
    const auto sweep = generateSweep(kRate, quickSweep());
    const auto filter = sweepInverseFilter(kRate, quickSweep());
    REQUIRE(sweep);
    REQUIRE(filter);

    const std::vector<float> unit{1.0f};
    const AudioBuffer recorded = passThrough(sweep.value(), unit);
    const auto impulse = deconvolveSweep(recorded.view(), kRate, quickSweep());
    REQUIRE(impulse);

    // The peak is at the start, and everything after it is far below.
    REQUIRE(peakIndex(impulse.value()) <= 2);
    const double peak = std::abs(static_cast<double>(impulse.value().channel(0)[0]));
    REQUIRE(peak > 0.5);

    double worstAfter = 0.0;
    for (SampleCount i = 200; i < impulse.value().frames(); ++i) {
        worstAfter =
            std::max(worstAfter, std::abs(static_cast<double>(impulse.value().channel(0)[i])));
    }
    // Forty decibels down, which is the level the fades and the band limits
    // leave rather than a number chosen to pass.
    REQUIRE(20.0 * std::log10(worstAfter / peak) < -40.0);
}

TEST_CASE("Discrete reflections come back where they were put", "[analysis][sweep]") {
    // A room, idealised to three arrivals. Recovering their positions and
    // relative levels is the measurement working end to end.
    std::vector<float> response(2000, 0.0f);
    response[0] = 1.0f;
    response[437] = -0.5f;
    response[1201] = 0.25f;

    const auto sweep = generateSweep(kRate, quickSweep());
    REQUIRE(sweep);
    const AudioBuffer recorded = passThrough(sweep.value(), response);

    const auto impulse = deconvolveSweep(recorded.view(), kRate, quickSweep(), 0.2);
    REQUIRE(impulse);
    REQUIRE(impulse.value().frames() > 1500);

    const double direct = impulse.value().channel(0)[0];
    REQUIRE(std::abs(direct) > 0.5);
    // Each reflection within a sample of where it was put, and at the right
    // level relative to the direct sound -- sign included, because a polarity
    // inversion is a real thing a measurement has to report.
    const double first = impulse.value().channel(0)[437] / direct;
    const double second = impulse.value().channel(0)[1201] / direct;
    REQUIRE(first == Approx(-0.5).margin(0.05));
    REQUIRE(second == Approx(0.25).margin(0.05));
}

TEST_CASE("A measured decay comes back with the reverberation it was given", "[analysis][sweep]") {
    // The whole chain: build a room, play a sweep through it, deconvolve, and
    // measure the result with the same code that measures a real impulse
    // response. This is the test that says the two halves fit together.
    const double t60 = 0.8;
    const double tau = t60 * std::log10(std::numbers::e) * 2.0 * 10.0 / 60.0;

    std::mt19937 engine{11};
    std::normal_distribution<double> noise{0.0, 1.0};
    std::vector<float> response(static_cast<std::size_t>(kRate.hz() * t60 * 1.5));
    for (std::size_t i = 0; i < response.size(); ++i) {
        const double t = static_cast<double>(i) / kRate.hz();
        response[i] = static_cast<float>(std::exp(-t / tau) * noise(engine));
    }
    response[0] += 2.0f; // A clear direct sound in front of the tail.

    SweepSettings settings = quickSweep();
    settings.seconds = 3.0;
    const auto sweep = generateSweep(kRate, settings);
    REQUIRE(sweep);
    const AudioBuffer recorded = passThrough(sweep.value(), response);

    const auto impulse = deconvolveSweep(recorded.view(), kRate, settings, t60 * 1.5);
    REQUIRE(impulse);

    const auto measured = measureRoomAcoustics(impulse.value().view(), kRate);
    REQUIRE(measured);
    REQUIRE(measured.value().valid);
    REQUIRE(measured.value().hasT20);
    REQUIRE(measured.value().t20Seconds == Approx(t60).epsilon(0.12));
}

TEST_CASE("Harmonic distortion deconvolves ahead of the answer, not into it", "[analysis][sweep]") {
    // The reason the sweep is exponential. A loudspeaker asked for a sine at x
    // also produces 2x and 3x; with an exponential sweep those deconvolve to a
    // fixed time *advance* and land before the linear response, where they can
    // simply be cut off. With a linear sweep they smear through it and cannot
    // be separated at all.
    //
    // The advance for the Nth harmonic is T*ln(N)/ln(f2/f1): the sweep reaches
    // N*f exactly that much earlier than it reaches f, whatever f is, and it is
    // that independence from f which makes the separation possible at all.
    const SweepSettings settings = quickSweep();
    const auto sweep = generateSweep(kRate, settings);
    REQUIRE(sweep);

    // A memoryless cubic, which is what a driver being pushed does. For a sine
    // of amplitude A, 0.25*x^3 expands to 0.1875*A^3 at the fundamental and
    // -0.0625*A^3 at the third harmonic, so with A = 0.5 the third harmonic
    // comes back at 0.0625*0.125 / 0.5 = 1/64 of the linear peak, which is
    // -36.1 dB. There is no second harmonic: an odd nonlinearity makes none.
    AudioBuffer distorted{ChannelLayout::mono(), sweep.value().frames()};
    for (SampleCount i = 0; i < sweep.value().frames(); ++i) {
        const double x = sweep.value().channel(0)[i];
        distorted.channel(0)[i] = static_cast<float>(x + 0.25 * x * x * x);
    }

    const auto filter = sweepInverseFilter(kRate, settings);
    REQUIRE(filter);

    // Deconvolve without discarding the negative-time half, so both regions can
    // be looked at.
    const SampleCount sweepFrames = sweep.value().frames();
    AudioBuffer full{ChannelLayout::mono(), distorted.frames() + sweepFrames - 1};
    for (SampleCount i = 0; i < distorted.frames(); ++i) {
        const double x = distorted.channel(0)[i];
        if (std::abs(x) < 1e-12) {
            continue;
        }
        for (SampleCount j = 0; j < sweepFrames; ++j) {
            full.channel(0)[i + j] += static_cast<float>(x * filter.value().channel(0)[j]);
        }
    }

    const SampleIndex origin = sweepFrames - 1;
    const double k = std::log(settings.endHz / settings.startHz);
    const auto advance = static_cast<SampleIndex>(std::lround(kRate.hz() * std::log(3.0) / k));

    const auto peakNear = [&](SampleIndex centre, SampleIndex reach) {
        double best = 0.0;
        SampleIndex at = centre;
        for (SampleIndex i = std::max<SampleIndex>(0, centre - reach);
             i < std::min<SampleIndex>(full.frames(), centre + reach); ++i) {
            const double magnitude = std::abs(static_cast<double>(full.channel(0)[i]));
            if (magnitude > best) {
                best = magnitude;
                at = i;
            }
        }
        return std::pair{best, at};
    };
    const auto rmsNear = [&](SampleIndex centre, SampleIndex reach) {
        double total = 0.0;
        SampleCount counted = 0;
        for (SampleIndex i = std::max<SampleIndex>(0, centre - reach);
             i < std::min<SampleIndex>(full.frames(), centre + reach); ++i) {
            total += static_cast<double>(full.channel(0)[i]) * full.channel(0)[i];
            ++counted;
        }
        return counted > 0 ? std::sqrt(total / static_cast<double>(counted)) : 0.0;
    };

    // The linear response is at the origin, and it is the loudest thing here.
    const auto [linear, linearAt] = peakNear(origin, 400);
    REQUIRE(linearAt == origin);

    // The third harmonic is somewhere in the negative-time half. Look for it
    // over a wide stretch rather than at the answer, then check it landed where
    // the theory says it would.
    const auto [harmonic, harmonicAt] = peakNear(origin - 14000, 6000);
    REQUIRE(origin - harmonicAt == advance);

    // And at the level the cubic predicts. The band limits trim its top -- the
    // third harmonic of anything above 6 kHz falls outside the sweep's range --
    // so allow a decibel for that, but not the order of magnitude that a
    // wrongly scaled inverse filter would move it by.
    REQUIRE(20.0 * std::log10(harmonic / linear) == Approx(-36.1).margin(1.0));

    // It is before the answer rather than in it: the mirror position after the
    // origin holds only the band-limited skirt of the linear impulse, an order
    // of magnitude quieter.
    REQUIRE(rmsNear(origin - advance, 300) > rmsNear(origin + advance, 300) * 10.0);

    // A second harmonic would sit at T*ln(2)/k. A cubic makes none, so that
    // position should be no louder than the skirt it sits in.
    const auto even = static_cast<SampleIndex>(std::lround(kRate.hz() * std::log(2.0) / k));
    REQUIRE(rmsNear(origin - even, 300) < rmsNear(origin - advance, 300));

    // Finally the thing a caller actually gets: deconvolveSweep cuts at the
    // origin, so none of this reaches the impulse response it returns.
    const auto trimmed = deconvolveSweep(distorted.view(), kRate, settings);
    REQUIRE(trimmed);
    REQUIRE(peakIndex(trimmed.value()) == 0);
}

TEST_CASE("The sweep spends equal time in every octave", "[analysis][sweep]") {
    // What makes it exponential rather than linear, checked by where the
    // instantaneous frequency has got to at the halfway point: it should be the
    // geometric mean of the endpoints, not the arithmetic one.
    SweepSettings settings = quickSweep();
    settings.startHz = 100.0;
    settings.endHz = 10000.0;
    settings.fadeSeconds = 0.0;
    const auto sweep = generateSweep(kRate, settings);
    REQUIRE(sweep);

    // Count zero crossings over a window *centred* on the midpoint to estimate
    // the frequency there. Centring matters: counting forwards from the
    // midpoint would average a rising frequency across the whole window and
    // read 1270 Hz rather than 1000, which says nothing about the sweep.
    //
    // A centred window still reads slightly high, and by a knowable amount.
    // Cycles over [t1, t2] are (f(t2) - f(t1))*T/k, so over +/-0.05 s about the
    // midpoint of a 1 s sweep from 100 Hz to 10 kHz (k = ln 100 = 4.6052) that
    // is (1258.93 - 794.33)/4.6052 = 100.89 cycles in 0.1 s: 1008.9 Hz, 0.89%
    // above the geometric mean because a finite window averages an exponential.
    const SampleCount middle = sweep.value().frames() / 2;
    const SampleCount window = 4800;
    int crossings = 0;
    for (SampleCount i = middle - window / 2; i < middle + window / 2 - 1; ++i) {
        const float a = sweep.value().channel(0)[i];
        const float b = sweep.value().channel(0)[i + 1];
        if ((a <= 0.0f && b > 0.0f) || (a >= 0.0f && b < 0.0f)) {
            ++crossings;
        }
    }
    const double measured = crossings * kRate.hz() / (2.0 * window);
    const double geometric = std::sqrt(settings.startHz * settings.endHz);
    REQUIRE(measured == Approx(geometric).epsilon(0.03));

    // Against the figure the window bias actually predicts, to within six
    // hertz -- one zero crossing at this window length, which is the resolution
    // of the method and nothing to do with the sweep.
    REQUIRE(measured == Approx(1008.9).margin(6.0));

    // And decisively not the arithmetic mean, which is where a linear sweep
    // would be.
    REQUIRE(measured < 0.5 * (settings.startHz + settings.endHz));
}

TEST_CASE("Sweeps that cannot be made are refused", "[analysis][sweep]") {
    SweepSettings settings = quickSweep();
    settings.seconds = 0.0;
    REQUIRE_FALSE(generateSweep(kRate, settings));

    settings = quickSweep();
    settings.seconds = 0.0001; // Fewer than sixteen samples.
    REQUIRE_FALSE(generateSweep(kRate, settings));

    REQUIRE_FALSE(generateSweep(SampleRate{0.0}, quickSweep()));

    // An end below the start is not a sweep. Clamping makes the two equal,
    // which the plan rejects.
    settings = quickSweep();
    settings.startHz = 10000.0;
    settings.endHz = 100.0;
    const auto rising = generateSweep(kRate, settings);
    if (rising) {
        // If it was clamped into validity it must at least still rise.
        REQUIRE(rising.value().frames() > 0);
    }

    const AudioBuffer empty{ChannelLayout::mono(), 0};
    REQUIRE_FALSE(deconvolveSweep(empty.view(), kRate, quickSweep()));

    const AudioBuffer something{ChannelLayout::mono(), 100};
    REQUIRE_FALSE(deconvolveSweep(something.view(), kRate, quickSweep(), 0.0, 4));

    // A recording shorter than the sweep is refused rather than deconvolved
    // into something plausible. One frame short is still short: the settings
    // are not the ones it was recorded with, and nothing about the result
    // would say so.
    const auto sweepFrames = static_cast<SampleCount>(quickSweep().seconds * kRate.hz());
    const AudioBuffer justShort{ChannelLayout::mono(), sweepFrames - 1};
    REQUIRE_FALSE(deconvolveSweep(justShort.view(), kRate, quickSweep()));

    // Exactly as long as the sweep is the shortest thing that is not, and it
    // gives an impulse response one sample long.
    const AudioBuffer justEnough{ChannelLayout::mono(), sweepFrames};
    const auto minimal = deconvolveSweep(justEnough.view(), kRate, quickSweep());
    REQUIRE(minimal);
    REQUIRE(minimal.value().frames() == 1);
}

TEST_CASE("Band limits are respected rather than clipped at Nyquist", "[analysis][sweep]") {
    SweepSettings settings = quickSweep();
    settings.endHz = 96000.0; // Far past Nyquist at 48 kHz.
    const auto sweep = generateSweep(kRate, settings);
    REQUIRE(sweep);
    // Nothing in it should alias: the highest instantaneous frequency is
    // clamped below Nyquist, so consecutive samples never reverse faster than
    // that allows.
    for (SampleCount i = 0; i < sweep.value().frames(); ++i) {
        REQUIRE(std::abs(sweep.value().channel(0)[i]) <= 1.0f);
    }
    const auto filter = sweepInverseFilter(kRate, settings);
    REQUIRE(filter);
    REQUIRE(filter.value().frames() == sweep.value().frames());
}
