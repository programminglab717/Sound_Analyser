#include <sa/analysis/TruePeakMeter.h>
#include <sa/core/RealtimeGuard.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

/// A sine written at an explicit phase, so a test can place the samples either
/// side of a peak rather than on it.
void fillSine(AudioBuffer& buffer, int channel, double cyclesPerSample, double amplitude,
              double phase) {
    float* samples = buffer.channel(channel);
    for (SampleCount i = 0; i < buffer.frames(); ++i) {
        const double angle =
            2.0 * std::numbers::pi * cyclesPerSample * static_cast<double>(i) + phase;
        samples[i] = static_cast<float>(amplitude * std::sin(angle));
    }
}

/// Raised-cosine taper on both ends of the buffer.
///
/// A signal that starts or stops instantaneously is a step, and a band-limited
/// step really does overshoot -- the meter is right to report it. That is just
/// not what these tests are about, so the ends are tapered and the measurement
/// left to the steady-state middle.
void fadeEdges(AudioBuffer& buffer, SampleCount fade) {
    for (int channel = 0; channel < buffer.channelCount(); ++channel) {
        float* samples = buffer.channel(channel);
        for (SampleCount i = 0; i < fade && i < buffer.frames(); ++i) {
            const double ramp =
                std::numbers::pi * static_cast<double>(i) / static_cast<double>(fade);
            const auto gain = static_cast<float>(0.5 - 0.5 * std::cos(ramp));
            samples[i] *= gain;
            samples[buffer.frames() - 1 - i] *= gain;
        }
    }
}

double samplePeak(const AudioBuffer& buffer) {
    double peak = 0.0;
    for (int channel = 0; channel < buffer.channelCount(); ++channel) {
        const float* samples = buffer.channel(channel);
        for (SampleCount i = 0; i < buffer.frames(); ++i) {
            peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
        }
    }
    return peak;
}

TruePeakMeter meterOrFail(int channels, int oversampling = TruePeakMeter::kDefaultOversampling) {
    auto result = TruePeakMeter::create(channels, oversampling);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

double measureOrFail(const AudioBuffer& buffer,
                     int oversampling = TruePeakMeter::kDefaultOversampling) {
    auto result = TruePeakMeter::measureDbtp(buffer.constView(), oversampling);
    REQUIRE(result.hasValue());
    return result.value();
}

} // namespace

TEST_CASE("A peak falling between samples is found", "[analysis][truepeak]") {
    // The classic inter-sample case: a sine at a quarter of the sample rate,
    // sampled 45 degrees either side of every peak. Every sample reads 0.707,
    // the waveform reaches 1.0 between them, and a sample-peak meter therefore
    // reports 3 dB of headroom that does not exist.
    AudioBuffer buffer{ChannelLayout::mono(), 4096};
    fillSine(buffer, 0, 0.25, 1.0, std::numbers::pi / 4.0);
    fadeEdges(buffer, 256);

    const double sample = samplePeak(buffer);
    const double truePeak = measureOrFail(buffer);

    CHECK(sample == Approx(0.70710678).epsilon(1e-4));
    CHECK(truePeak > 20.0 * std::log10(sample));
    CHECK(truePeak == Approx(0.0).margin(0.01));
}

TEST_CASE("True peak is never below sample peak", "[analysis][truepeak]") {
    // Interpolation can only add peaks, never remove them, and phase 0 of the
    // filter reproduces the input samples exactly. A meter that reported less
    // than the sample peak would be worse than useless as a clipping guard.
    AudioBuffer buffer{ChannelLayout::stereo(), 2048};
    fillSine(buffer, 0, 0.011, 0.8, 0.0);
    fillSine(buffer, 1, 0.37, 0.6, 1.1);

    const double truePeak = std::pow(10.0, measureOrFail(buffer) / 20.0);
    CHECK(truePeak >= samplePeak(buffer));
}

TEST_CASE("A sine sampled on its peaks reports no extra", "[analysis][truepeak]") {
    // Same frequency as the inter-sample case, but phased so the samples land
    // on the peaks. There is nothing between them to find, so a meter reading
    // materially above 0 dBTP here would be inventing headroom problems.
    AudioBuffer buffer{ChannelLayout::mono(), 4096};
    fillSine(buffer, 0, 0.25, 1.0, std::numbers::pi / 2.0);
    fadeEdges(buffer, 256);

    CHECK(measureOrFail(buffer) == Approx(0.0).margin(0.02));
}

TEST_CASE("A constant signal does not overshoot itself", "[analysis][truepeak]") {
    // Each polyphase branch is normalised to unity DC gain. Without that a
    // DC-offset recording would read as if it were clipping.
    AudioBuffer buffer{ChannelLayout::mono(), 2048};
    std::fill_n(buffer.channel(0), 2048, 0.5f);
    fadeEdges(buffer, 256);

    const double truePeak = std::pow(10.0, measureOrFail(buffer) / 20.0);
    CHECK(truePeak == Approx(0.5).epsilon(1e-5));
}

TEST_CASE("Silence reads the floor rather than -inf", "[analysis][truepeak]") {
    const AudioBuffer buffer{ChannelLayout::stereo(), 4096};
    const double truePeak = measureOrFail(buffer);
    CHECK(std::isfinite(truePeak));
    CHECK(truePeak == kDecibelFloor);
}

TEST_CASE("Per-channel peaks are reported separately", "[analysis][truepeak]") {
    AudioBuffer buffer{ChannelLayout::stereo(), 2048};
    fillSine(buffer, 0, 0.05, 0.25, 0.0);
    fillSine(buffer, 1, 0.05, 0.5, 0.0);
    fadeEdges(buffer, 256);

    auto meter = meterOrFail(2);
    meter.process(buffer.constView());

    CHECK(meter.channelTruePeak(0) == Approx(0.25).epsilon(1e-4));
    CHECK(meter.channelTruePeak(1) == Approx(0.5).epsilon(1e-4));
    CHECK(meter.truePeak() == meter.channelTruePeak(1));
    // Out-of-range channels answer with silence rather than reading past the end.
    CHECK(meter.channelTruePeak(-1) == 0.0);
    CHECK(meter.channelTruePeak(9) == 0.0);
}

TEST_CASE("4x oversampling is a floor, not an exact answer", "[analysis][truepeak]") {
    // fs/8 phased so that every peak falls an eighth of a sample from a sample
    // -- and therefore exactly midway between two 4x grid points, where 4x
    // cannot see it at all. 16x lands on it. This is the residual error
    // BS.1770-4 is acknowledging when it calls 4x a minimum, and the reason
    // this meter allows more.
    AudioBuffer buffer{ChannelLayout::mono(), 4096};
    fillSine(buffer, 0, 0.125, 1.0, 0.46875 * std::numbers::pi);
    fadeEdges(buffer, 256);

    const double atFour = measureOrFail(buffer, 4);
    const double atSixteen = measureOrFail(buffer, 16);

    CHECK(atFour == Approx(-0.0419).margin(0.01));
    CHECK(atFour == Approx(20.0 * std::log10(samplePeak(buffer))).margin(0.01));
    CHECK(atSixteen > atFour);
    CHECK(atSixteen == Approx(0.0).margin(0.01));
}

TEST_CASE("Streaming block by block matches one-shot exactly", "[analysis][truepeak]") {
    AudioBuffer buffer{ChannelLayout::stereo(), 20011};
    fillSine(buffer, 0, 0.25, 0.9, std::numbers::pi / 4.0);
    fillSine(buffer, 1, 0.031, 0.7, 0.2);

    const double oneShot = measureOrFail(buffer);

    auto meter = meterOrFail(2);
    const SampleCount sizes[] = {1, 1000, 7, 333, 2048, 3};
    SampleCount offset = 0;
    int index = 0;
    while (offset < buffer.frames()) {
        const SampleCount size = std::min(sizes[index % 6], buffer.frames() - offset);
        meter.process(buffer.constView().subRange(offset, size));
        offset += size;
        ++index;
    }

    CHECK(meter.truePeakDbtp() == oneShot);
    CHECK(meter.framesProcessed() == buffer.frames());
}

TEST_CASE("Reset clears both the peak and the filter history", "[analysis][truepeak]") {
    AudioBuffer loud{ChannelLayout::mono(), 2048};
    fillSine(loud, 0, 0.25, 1.0, std::numbers::pi / 4.0);

    auto meter = meterOrFail(1);
    meter.process(loud.constView());
    REQUIRE(meter.truePeak() > 0.9);

    meter.reset();
    CHECK(meter.truePeak() == 0.0);
    CHECK(meter.framesProcessed() == 0);

    // If the delay line had survived, the loud tail would leak into this.
    const AudioBuffer silence{ChannelLayout::mono(), 2048};
    meter.process(silence.constView());
    CHECK(meter.truePeak() == 0.0);
}

TEST_CASE("Degenerate input is refused or ignored, never crashed on", "[analysis][truepeak]") {
    SECTION("an empty view has no channels to meter") {
        CHECK_FALSE(TruePeakMeter::measureDbtp(ConstAudioBufferView{}).hasValue());
    }

    SECTION("oversampling below the standard's floor is refused") {
        CHECK_FALSE(TruePeakMeter::create(1, 0).hasValue());
        CHECK_FALSE(TruePeakMeter::create(1, 1).hasValue());
        CHECK_FALSE(TruePeakMeter::create(1, 3).hasValue());
        CHECK_FALSE(TruePeakMeter::create(1, 17).hasValue());
        CHECK(TruePeakMeter::create(1, 4).hasValue());
        CHECK(TruePeakMeter::create(1, 16).hasValue());
    }

    SECTION("a bad channel count is refused") {
        CHECK_FALSE(TruePeakMeter::create(0).hasValue());
        CHECK_FALSE(TruePeakMeter::create(-2).hasValue());
        CHECK_FALSE(TruePeakMeter::create(kMaxChannels + 1).hasValue());
    }

    SECTION("a single sample is metered without reaching past the buffer") {
        AudioBuffer buffer{ChannelLayout::mono(), 1};
        buffer.channel(0)[0] = -0.75f;
        // Only the raw-sample path can see it -- the filter has not yet filled.
        CHECK(measureOrFail(buffer) == Approx(20.0 * std::log10(0.75)).margin(0.01));
    }

    SECTION("a zero-length buffer measures nothing") {
        AudioBuffer buffer{ChannelLayout::mono(), 0};
        CHECK(measureOrFail(buffer) == kDecibelFloor);
    }

    SECTION("a block with the wrong channel count is ignored") {
        AudioBuffer wrong{ChannelLayout::stereo(), 1024};
        fillSine(wrong, 0, 0.1, 1.0, 0.0);
        auto meter = meterOrFail(1);
        meter.process(wrong.constView());
        CHECK(meter.framesProcessed() == 0);
        CHECK(meter.truePeak() == 0.0);
    }
}

TEST_CASE("process allocates nothing", "[analysis][truepeak][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    AudioBuffer buffer{ChannelLayout::stereo(), 24576};
    fillSine(buffer, 0, 0.25, 0.9, std::numbers::pi / 4.0);
    fillSine(buffer, 1, 0.13, 0.5, 0.0);

    auto meter = meterOrFail(2);
    const SampleCount block = 512;

    const rt::ScopedAudioThread guard;
    const rt::AllocationScope scope;

    for (SampleCount offset = 0; offset + block <= buffer.frames(); offset += block) {
        meter.process(buffer.constView().subRange(offset, block));
    }
    static_cast<void>(meter.truePeakDbtp());

    CHECK(scope.count() == 0);
}
