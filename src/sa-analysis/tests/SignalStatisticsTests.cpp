#include <sa/analysis/ProgrammeAnalysis.h>
#include <sa/analysis/SignalStatistics.h>
#include <sa/core/RealtimeGuard.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

void fillSine(AudioBuffer& buffer, int channel, double cyclesPerSample, double amplitude,
              double offset = 0.0) {
    float* samples = buffer.channel(channel);
    for (SampleCount i = 0; i < buffer.frames(); ++i) {
        const double angle = 2.0 * std::numbers::pi * cyclesPerSample * static_cast<double>(i);
        samples[i] = static_cast<float>(amplitude * std::sin(angle) + offset);
    }
}

SignalStatistics measureOrFail(const AudioBuffer& buffer,
                               double truePeakDbtp = kDecibelFloor,
                               double integratedLufs = kDecibelFloor) {
    auto result = SignalStatisticsMeter::measure(buffer.constView(), truePeakDbtp, integratedLufs);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

SignalStatisticsMeter meterOrFail(int channels) {
    auto result = SignalStatisticsMeter::create(channels);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

} // namespace

TEST_CASE("A full-scale sine has the textbook peak, RMS and crest",
          "[analysis][statistics]") {
    // An exact number of cycles, so the RMS is exactly amplitude / sqrt(2) and
    // the crest factor exactly 3.01 dB, with no windowing excuse available.
    AudioBuffer buffer{ChannelLayout::mono(), 4800};
    fillSine(buffer, 0, 100.0 / 4800.0, 1.0);

    const auto statistics = measureOrFail(buffer);
    CHECK(statistics.samplePeak == Approx(1.0).epsilon(1e-4));
    CHECK(statistics.rms == Approx(1.0 / std::sqrt(2.0)).epsilon(1e-4));
    CHECK(statistics.samplePeakDbfs == Approx(0.0).margin(0.01));
    CHECK(statistics.rmsDbfs == Approx(-3.0103).margin(0.01));
    CHECK(statistics.crestFactorDb == Approx(3.0103).margin(0.01));
    CHECK(statistics.dcOffset == Approx(0.0).margin(1e-6));
    CHECK(statistics.frames == 4800);
    CHECK(statistics.channels == 1);
}

TEST_CASE("DC offset is found on the worst channel, not averaged away",
          "[analysis][statistics]") {
    // Equal and opposite offsets on left and right average to a clean zero.
    // That average is exactly the reading that would let a real fault ship.
    AudioBuffer buffer{ChannelLayout::stereo(), 4800};
    fillSine(buffer, 0, 100.0 / 4800.0, 0.5, 0.1);
    fillSine(buffer, 1, 100.0 / 4800.0, 0.5, -0.1);

    const auto statistics = measureOrFail(buffer);
    CHECK(std::abs(statistics.dcOffset) == Approx(0.1).epsilon(1e-4));
}

TEST_CASE("DC offset keeps its sign", "[analysis][statistics]") {
    AudioBuffer buffer{ChannelLayout::mono(), 4800};
    fillSine(buffer, 0, 100.0 / 4800.0, 0.5, -0.25);
    CHECK(measureOrFail(buffer).dcOffset == Approx(-0.25).epsilon(1e-4));
}

TEST_CASE("RMS pools every channel", "[analysis][statistics]") {
    // One channel at full scale and one silent is 0.7071 RMS overall, not 1.0
    // and not 0.5. Getting this wrong is the same sum-of-squares mistake that
    // misdraws waveforms.
    AudioBuffer buffer{ChannelLayout::stereo(), 4800};
    std::fill_n(buffer.channel(0), 4800, 1.0f);

    const auto statistics = measureOrFail(buffer);
    CHECK(statistics.rms == Approx(std::sqrt(0.5)).epsilon(1e-6));
    CHECK(statistics.samplePeak == 1.0);
}

TEST_CASE("A square wave has no crest at all", "[analysis][statistics]") {
    AudioBuffer buffer{ChannelLayout::mono(), 4800};
    for (SampleCount i = 0; i < 4800; ++i) {
        buffer.channel(0)[i] = (i / 100) % 2 == 0 ? 0.5f : -0.5f;
    }

    const auto statistics = measureOrFail(buffer);
    CHECK(statistics.rms == Approx(0.5).epsilon(1e-6));
    CHECK(statistics.crestFactorDb == Approx(0.0).margin(1e-6));
}

TEST_CASE("Silence reads the floor and a finite crest", "[analysis][statistics]") {
    const AudioBuffer buffer{ChannelLayout::stereo(), 4800};
    const auto statistics = measureOrFail(buffer);

    CHECK(statistics.samplePeak == 0.0);
    CHECK(statistics.rms == 0.0);
    CHECK(statistics.samplePeakDbfs == kDecibelFloor);
    CHECK(statistics.rmsDbfs == kDecibelFloor);
    CHECK(statistics.dcOffset == 0.0);
    // 0/0 has no crest; 0 dB is what any constant signal reads and keeps the
    // field finite for whatever averages it downstream.
    CHECK(std::isfinite(statistics.crestFactorDb));
    CHECK(statistics.crestFactorDb == 0.0);
}

TEST_CASE("Peak to loudness ratio is true peak over gated loudness",
          "[analysis][statistics]") {
    CHECK(peakToLoudnessRatioDb(-1.0, -14.0) == Approx(13.0));
    CHECK(peakToLoudnessRatioDb(-0.3, -23.0) == Approx(22.7));

    // Either input missing means there is no ratio to report, rather than a
    // number built out of a floor.
    CHECK(peakToLoudnessRatioDb(kDecibelFloor, -23.0) == kDecibelFloor);
    CHECK(peakToLoudnessRatioDb(-1.0, kDecibelFloor) == kDecibelFloor);

    AudioBuffer buffer{ChannelLayout::mono(), 4800};
    fillSine(buffer, 0, 100.0 / 4800.0, 0.5);
    CHECK(measureOrFail(buffer).peakToLoudnessRatioDb == kDecibelFloor);
    CHECK(measureOrFail(buffer, -1.0, -16.0).peakToLoudnessRatioDb == Approx(15.0));
}

TEST_CASE("Streaming block by block matches one-shot exactly", "[analysis][statistics]") {
    AudioBuffer buffer{ChannelLayout::stereo(), 20011};
    fillSine(buffer, 0, 0.013, 0.8, 0.02);
    fillSine(buffer, 1, 0.071, 0.4, -0.05);

    const auto oneShot = measureOrFail(buffer);

    auto meter = meterOrFail(2);
    const SampleCount sizes[] = {1, 4096, 17, 999, 2};
    SampleCount offset = 0;
    int index = 0;
    while (offset < buffer.frames()) {
        const SampleCount size = std::min(sizes[index % 5], buffer.frames() - offset);
        meter.process(buffer.constView().subRange(offset, size));
        offset += size;
        ++index;
    }
    const auto streamed = meter.statistics();

    CHECK(streamed.samplePeak == oneShot.samplePeak);
    CHECK(streamed.rms == oneShot.rms);
    CHECK(streamed.dcOffset == oneShot.dcOffset);
    CHECK(streamed.crestFactorDb == oneShot.crestFactorDb);
    CHECK(streamed.frames == oneShot.frames);
}

TEST_CASE("Degenerate input is refused or ignored, never crashed on",
          "[analysis][statistics]") {
    SECTION("an empty view has no channels") {
        CHECK_FALSE(SignalStatisticsMeter::measure(ConstAudioBufferView{}).hasValue());
    }

    SECTION("a bad channel count is refused") {
        CHECK_FALSE(SignalStatisticsMeter::create(0).hasValue());
        CHECK_FALSE(SignalStatisticsMeter::create(-1).hasValue());
        CHECK_FALSE(SignalStatisticsMeter::create(kMaxChannels + 1).hasValue());
    }

    SECTION("a zero-length buffer measures nothing") {
        AudioBuffer buffer{ChannelLayout::mono(), 0};
        const auto statistics = measureOrFail(buffer);
        CHECK(statistics.frames == 0);
        CHECK(statistics.rms == 0.0);
        CHECK(statistics.samplePeakDbfs == kDecibelFloor);
    }

    SECTION("a single sample is a peak, an RMS and a DC offset all at once") {
        AudioBuffer buffer{ChannelLayout::mono(), 1};
        buffer.channel(0)[0] = -0.4f;
        const auto statistics = measureOrFail(buffer);
        CHECK(statistics.samplePeak == Approx(0.4).epsilon(1e-6));
        CHECK(statistics.rms == Approx(0.4).epsilon(1e-6));
        CHECK(statistics.dcOffset == Approx(-0.4).epsilon(1e-6));
        CHECK(statistics.crestFactorDb == Approx(0.0).margin(1e-6));
    }

    SECTION("a block with the wrong channel count is ignored") {
        AudioBuffer wrong{ChannelLayout::stereo(), 1024};
        auto meter = meterOrFail(1);
        meter.process(wrong.constView());
        CHECK(meter.framesProcessed() == 0);
    }
}

TEST_CASE("process allocates nothing", "[analysis][statistics][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    AudioBuffer buffer{ChannelLayout::stereo(), 24576};
    fillSine(buffer, 0, 0.01, 0.9);
    fillSine(buffer, 1, 0.02, 0.5);

    auto meter = meterOrFail(2);
    const SampleCount block = 512;

    const rt::ScopedAudioThread guard;
    const rt::AllocationScope scope;

    for (SampleCount offset = 0; offset + block <= buffer.frames(); offset += block) {
        meter.process(buffer.constView().subRange(offset, block));
    }
    static_cast<void>(meter.statistics());

    CHECK(scope.count() == 0);
}

TEST_CASE("The one-shot programme analysis ties the three meters together",
          "[analysis][statistics]") {
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::stereo();

    AudioBuffer buffer{layout, secondsToSamples(5.0, rate)};
    fillSine(buffer, 0, 1000.0 / rate.hz(), 0.5);
    fillSine(buffer, 1, 1000.0 / rate.hz(), 0.5);

    auto result = analyseProgramme(buffer.constView(), rate, layout);
    REQUIRE(result.hasValue());
    const ProgrammeAnalysis& analysis = result.value();

    CHECK(analysis.loudness.integratedLufs == Approx(-6.01).margin(0.1));
    CHECK(analysis.truePeakDbtp == Approx(-6.0206).margin(0.05));
    CHECK(analysis.statistics.samplePeak == Approx(0.5).epsilon(1e-3));
    // PLR is the one figure that needs two meters at once, which is the reason
    // this entry point exists at all.
    CHECK(analysis.statistics.peakToLoudnessRatioDb ==
          Approx(analysis.truePeakDbtp - analysis.loudness.integratedLufs).margin(1e-9));

    CHECK_FALSE(analyseProgramme(ConstAudioBufferView{}, rate, layout).hasValue());
}
