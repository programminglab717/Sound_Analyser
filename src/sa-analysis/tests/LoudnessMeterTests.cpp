#include <sa/analysis/LoudnessMeter.h>
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

/// Analytic constants the invariants below are checked against. They are
/// written out rather than inlined so that a test failure says which law of
/// physics the meter has broken.
constexpr double kDoublingLu = 6.020599913279624;   // 20 * log10(2)
constexpr double kPowerSumLu = 3.010299956639812;   // 10 * log10(2)
constexpr double kSurroundLu = 1.4921911265355774;  // 10 * log10(1.41)

SampleCount framesFor(double seconds, SampleRate rate) {
    return secondsToSamples(seconds, rate);
}

void addSine(AudioBuffer& buffer, int channel, double frequency, double amplitude, SampleRate rate,
             SampleCount start, SampleCount count) {
    float* samples = buffer.channel(channel);
    for (SampleCount i = 0; i < count; ++i) {
        const SampleCount index = start + i;
        if (index < 0 || index >= buffer.frames()) {
            continue;
        }
        const double phase =
            2.0 * std::numbers::pi * frequency * static_cast<double>(index) / rate.hz();
        samples[index] += static_cast<float>(amplitude * std::sin(phase));
    }
}

/// A tone filling the whole of one channel.
void fillSine(AudioBuffer& buffer, int channel, double frequency, double amplitude,
              SampleRate rate) {
    addSine(buffer, channel, frequency, amplitude, rate, 0, buffer.frames());
}

LoudnessMeter meterOrFail(SampleRate rate, const ChannelLayout& layout) {
    auto result = LoudnessMeter::create(rate, layout);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

LoudnessMeasurement measureOrFail(const AudioBuffer& buffer, SampleRate rate,
                                  const ChannelLayout& layout) {
    auto result = LoudnessMeter::measure(buffer.constView(), rate, layout);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

/// Integrated loudness of a mono tone of `amplitude`, `seconds` long.
double monoToneLufs(double amplitude, double seconds = 5.0,
                    SampleRate rate = kSampleRate48000) {
    AudioBuffer buffer{ChannelLayout::mono(), framesFor(seconds, rate)};
    fillSine(buffer, 0, 1000.0, amplitude, rate);
    return measureOrFail(buffer, rate, ChannelLayout::mono()).integratedLufs;
}

} // namespace

// --- Absolute calibration ---------------------------------------------------

TEST_CASE("A 1 kHz stereo sine at -23 dBFS reads -23 LUFS", "[analysis][loudness]") {
    // EBU Tech 3341 compliance case 1, rebuilt from its description because the
    // official WAV set is not in the repository. Passing this does not make the
    // meter conformant -- it makes exactly one point of the curve right -- but
    // failing it would mean the offset, the filter or the channel summation is
    // wrong, and no other test would tell us which.
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::stereo();
    const double amplitude = std::pow(10.0, -23.0 / 20.0);

    AudioBuffer buffer{layout, framesFor(10.0, rate)};
    fillSine(buffer, 0, 1000.0, amplitude, rate);
    fillSine(buffer, 1, 1000.0, amplitude, rate);

    const auto measurement = measureOrFail(buffer, rate, layout);
    CHECK(measurement.integratedLufs == Approx(-23.0).margin(0.1));
    CHECK(measurement.momentaryLufs == Approx(-23.0).margin(0.1));
    CHECK(measurement.shortTermLufs == Approx(-23.0).margin(0.1));
}

TEST_CASE("The reading does not depend on the sample rate", "[analysis][loudness]") {
    // The payoff of deriving K-weighting coefficients per rate instead of
    // reusing the 48 kHz table. With the table reused, this same signal drifts
    // 2.4 LU between 44.1 kHz and 192 kHz; derived properly it stays inside
    // 0.05 LU. The tones straddle all three regions of the K curve, which a
    // single 1 kHz tone would not.
    std::vector<double> readings;
    for (double hz : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
        const SampleRate rate{hz};
        AudioBuffer buffer{ChannelLayout::mono(), framesFor(5.0, rate)};
        for (double frequency : {100.0, 1000.0, 6000.0}) {
            addSine(buffer, 0, frequency, 0.1, rate, 0, buffer.frames());
        }
        readings.push_back(measureOrFail(buffer, rate, ChannelLayout::mono()).integratedLufs);
    }

    const auto [low, high] = std::minmax_element(readings.begin(), readings.end());
    INFO("spread " << (*high - *low) << " LU");
    CHECK(*high - *low < 0.1);
}

// --- Summation and weighting laws -------------------------------------------

TEST_CASE("Doubling the amplitude raises loudness by 6.02 LU", "[analysis][loudness]") {
    const double quiet = monoToneLufs(0.1);
    const double loud = monoToneLufs(0.2);
    // Exact, not approximate: the meter is linear, the gating picks the same
    // blocks either way, and every block energy scales by exactly four.
    CHECK(loud - quiet == Approx(kDoublingLu).margin(1e-6));
}

TEST_CASE("Two correlated channels read 3.01 LU above one", "[analysis][loudness]") {
    // Loudness sums power across channels, so identical content in two
    // channels is twice the power, not twice the amplitude. Getting this wrong
    // -- by averaging channels, say -- is a 3 LU error on every stereo file.
    const SampleRate rate = kSampleRate48000;
    const auto stereo = ChannelLayout::stereo();

    AudioBuffer dual{stereo, framesFor(5.0, rate)};
    fillSine(dual, 0, 1000.0, 0.1, rate);
    fillSine(dual, 1, 1000.0, 0.1, rate);

    const double stereoLufs = measureOrFail(dual, rate, stereo).integratedLufs;
    CHECK(stereoLufs - monoToneLufs(0.1) == Approx(kPowerSumLu).margin(1e-6));
}

TEST_CASE("Surround channels carry the 1.41 weight", "[analysis][loudness]") {
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::fiveOne();
    const SampleCount frames = framesFor(5.0, rate);

    AudioBuffer front{layout, frames};
    fillSine(front, layout.indexOf(Speaker::Left), 1000.0, 0.1, rate);

    AudioBuffer surround{layout, frames};
    fillSine(surround, layout.indexOf(Speaker::LeftSurround), 1000.0, 0.1, rate);

    const double frontLufs = measureOrFail(front, rate, layout).integratedLufs;
    const double surroundLufs = measureOrFail(surround, rate, layout).integratedLufs;
    CHECK(surroundLufs - frontLufs == Approx(kSurroundLu).margin(1e-6));
}

TEST_CASE("LFE is excluded from the measurement entirely", "[analysis][loudness]") {
    // Not attenuated -- excluded. A film mix can have 10 dB more energy in the
    // LFE than anywhere else, and including it at any weight would make every
    // action sequence measure far louder than it sounds.
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::fiveOne();
    const SampleCount frames = framesFor(5.0, rate);
    const int lfe = layout.indexOf(Speaker::Lfe);
    REQUIRE(lfe >= 0);

    AudioBuffer without{layout, frames};
    fillSine(without, layout.indexOf(Speaker::Left), 1000.0, 0.1, rate);
    fillSine(without, layout.indexOf(Speaker::Right), 1000.0, 0.1, rate);

    AudioBuffer with{layout, frames};
    fillSine(with, layout.indexOf(Speaker::Left), 1000.0, 0.1, rate);
    fillSine(with, layout.indexOf(Speaker::Right), 1000.0, 0.1, rate);
    fillSine(with, lfe, 60.0, 0.9, rate);

    CHECK(measureOrFail(with, rate, layout).integratedLufs ==
          measureOrFail(without, rate, layout).integratedLufs);
}

TEST_CASE("Channel weights follow the BS.1770-4 table", "[analysis][loudness]") {
    CHECK(LoudnessMeter::weightFor(Speaker::Left) == 1.0);
    CHECK(LoudnessMeter::weightFor(Speaker::Right) == 1.0);
    CHECK(LoudnessMeter::weightFor(Speaker::Centre) == 1.0);
    CHECK(LoudnessMeter::weightFor(Speaker::Mono) == 1.0);
    CHECK(LoudnessMeter::weightFor(Speaker::LeftSurround) == LoudnessMeter::kSurroundWeight);
    CHECK(LoudnessMeter::weightFor(Speaker::RightSurround) == LoudnessMeter::kSurroundWeight);
    CHECK(LoudnessMeter::weightFor(Speaker::LeftSurroundRear) == LoudnessMeter::kSurroundWeight);
    CHECK(LoudnessMeter::weightFor(Speaker::RightSurroundRear) == LoudnessMeter::kSurroundWeight);
    CHECK(LoudnessMeter::weightFor(Speaker::Lfe) == 0.0);
    // Discrete multichannel material has no speaker assignment; full weight is
    // the only assumption that cannot silently discard signal.
    CHECK(LoudnessMeter::weightFor(Speaker::Unknown) == 1.0);
}

TEST_CASE("A layout with nothing but LFE is rejected", "[analysis][loudness]") {
    const auto lfeOnly = ChannelLayout::discrete(0).withSpeakers({Speaker::Lfe});
    CHECK_FALSE(LoudnessMeter::create(kSampleRate48000, lfeOnly).hasValue());
}

// --- Gating -----------------------------------------------------------------

TEST_CASE("The absolute gate drops silent blocks", "[analysis][loudness]") {
    // Ten seconds of tone followed by ten of digital silence. Ungated, the
    // answer would be 3 dB low; correctly gated, the silence is simply not
    // programme material and does not count.
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::mono();
    const SampleCount half = framesFor(10.0, rate);

    AudioBuffer loudOnly{layout, half};
    fillSine(loudOnly, 0, 1000.0, 0.1, rate);

    AudioBuffer withSilence{layout, half * 2};
    addSine(withSilence, 0, 1000.0, 0.1, rate, 0, half);

    const auto reference = measureOrFail(loudOnly, rate, layout);
    const auto gated = measureOrFail(withSilence, rate, layout);

    // The three blocks that straddle the boundary are part loud and part
    // silent, and they legitimately pull the answer down a fraction.
    CHECK(gated.integratedLufs == Approx(reference.integratedLufs).margin(0.15));
    // The ungated average would land near 3 LU lower, so this margin is what
    // distinguishes a working gate from no gate at all.
    CHECK(gated.integratedLufs > reference.integratedLufs - kPowerSumLu + 1.0);
    // Only the handful of blocks touching the boundary survive from the silent
    // half -- the filter rings for a few milliseconds past the cut, so this is
    // a small bound rather than an exact count.
    INFO("gated blocks " << gated.gatedBlockCount << " vs reference "
                         << reference.gatedBlockCount);
    CHECK(gated.gatedBlockCount <= reference.gatedBlockCount + 6);
}

TEST_CASE("The relative gate drops a quiet passage that clears -70", "[analysis][loudness]") {
    // A -40 dB tail sits at about -60 LUFS: comfortably above the absolute
    // gate, so only the -10 LU relative gate can remove it. Without that second
    // stage the reading would drop by about 3 LU.
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::mono();
    const SampleCount half = framesFor(10.0, rate);

    AudioBuffer loudOnly{layout, half};
    fillSine(loudOnly, 0, 1000.0, 0.1, rate);

    AudioBuffer withTail{layout, half * 2};
    addSine(withTail, 0, 1000.0, 0.1, rate, 0, half);
    addSine(withTail, 0, 1000.0, 0.001, rate, half, half);

    const auto reference = measureOrFail(loudOnly, rate, layout);
    const auto gated = measureOrFail(withTail, rate, layout);

    CHECK(gated.integratedLufs == Approx(reference.integratedLufs).margin(0.15));
    // Every block of the tail clears -70 LUFS, so the absolute gate lets all of
    // them through and only the relative gate can have removed them. If this
    // count dropped, the fixture would be testing the wrong gate.
    INFO("gated blocks " << gated.gatedBlockCount << " vs reference "
                         << reference.gatedBlockCount);
    CHECK(gated.gatedBlockCount > reference.gatedBlockCount * 2);
}

TEST_CASE("Loudness range spans the quiet and loud halves", "[analysis][loudness]") {
    // Twenty seconds at one level then twenty 10 dB lower. LRA is the 10th to
    // 95th percentile of the short-term values, so the answer should be the
    // 10 dB step itself, not the average of the two halves.
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::mono();
    const SampleCount half = framesFor(20.0, rate);

    AudioBuffer buffer{layout, half * 2};
    addSine(buffer, 0, 1000.0, 0.2, rate, 0, half);
    addSine(buffer, 0, 1000.0, 0.2 / std::sqrt(10.0), rate, half, half);

    const auto measurement = measureOrFail(buffer, rate, layout);
    INFO("LRA " << measurement.loudnessRangeLu << " from " << measurement.shortTermBlockCount
                << " short-term blocks");
    CHECK(measurement.shortTermBlockCount > 30);
    CHECK(measurement.loudnessRangeLu == Approx(10.0).margin(0.5));
}

TEST_CASE("A steady tone has essentially no loudness range", "[analysis][loudness]") {
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::mono();

    AudioBuffer buffer{layout, framesFor(20.0, rate)};
    fillSine(buffer, 0, 1000.0, 0.2, rate);

    CHECK(measureOrFail(buffer, rate, layout).loudnessRangeLu < 0.3);
}

// --- Floors and degenerate input --------------------------------------------

TEST_CASE("Silence reads the floor rather than NaN or -inf", "[analysis][loudness]") {
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::stereo();
    const AudioBuffer buffer{layout, framesFor(5.0, rate)};

    const auto measurement = measureOrFail(buffer, rate, layout);
    for (double value : {measurement.integratedLufs, measurement.momentaryLufs,
                         measurement.shortTermLufs, measurement.maximumMomentaryLufs}) {
        CHECK(std::isfinite(value));
        CHECK(value == kDecibelFloor);
    }
    CHECK(measurement.loudnessRangeLu == 0.0);
    // Every block was below the absolute gate, so nothing entered the pool.
    CHECK(measurement.gatedBlockCount == 0);
    CHECK(measurement.framesProcessed == buffer.frames());
}

TEST_CASE("Material shorter than one block reports nothing rather than guessing",
          "[analysis][loudness]") {
    // BS.1770 defines no gating block below 400 ms, so there is no honest
    // number to report. The frame count still moves, which is how a caller
    // tells "too short" from "nothing was fed".
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::mono();

    AudioBuffer buffer{layout, framesFor(0.2, rate)};
    fillSine(buffer, 0, 1000.0, 0.5, rate);

    const auto measurement = measureOrFail(buffer, rate, layout);
    CHECK(measurement.integratedLufs == kDecibelFloor);
    CHECK(measurement.momentaryLufs == kDecibelFloor);
    CHECK(measurement.gatedBlockCount == 0);
    CHECK(measurement.framesProcessed == buffer.frames());
}

TEST_CASE("Momentary and short-term appear as soon as they are defined",
          "[analysis][loudness]") {
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::mono();

    AudioBuffer buffer{layout, framesFor(5.0, rate)};
    fillSine(buffer, 0, 1000.0, 0.1, rate);

    auto meter = meterOrFail(rate, layout);
    const SampleCount step = framesFor(0.1, rate);

    meter.process(buffer.constView().subRange(0, step * 3));
    CHECK(meter.momentaryLufs() == kDecibelFloor);

    meter.process(buffer.constView().subRange(step * 3, step));
    CHECK(meter.momentaryLufs() > -30.0);
    CHECK(meter.shortTermLufs() == kDecibelFloor);

    meter.process(buffer.constView().subRange(step * 4, step * 26));
    CHECK(meter.shortTermLufs() > -30.0);
}

TEST_CASE("Degenerate input is refused or ignored, never crashed on",
          "[analysis][loudness]") {
    const SampleRate rate = kSampleRate48000;
    const auto mono = ChannelLayout::mono();

    SECTION("an empty view has no channels to match a layout") {
        const ConstAudioBufferView empty;
        CHECK_FALSE(LoudnessMeter::measure(empty, rate, mono).hasValue());
    }

    SECTION("a layout with no channels is refused") {
        CHECK_FALSE(LoudnessMeter::create(rate, ChannelLayout::discrete(0)).hasValue());
    }

    SECTION("an invalid rate is refused") {
        AudioBuffer buffer{mono, 1024};
        CHECK_FALSE(LoudnessMeter::measure(buffer.constView(), SampleRate{0.0}, mono).hasValue());
        CHECK_FALSE(LoudnessMeter::measure(buffer.constView(), SampleRate{-1.0}, mono).hasValue());
        CHECK_FALSE(LoudnessMeter::measure(buffer.constView(), SampleRate{100.0}, mono).hasValue());
    }

    SECTION("a single sample is processed and reports the floor") {
        AudioBuffer buffer{mono, 1};
        buffer.channel(0)[0] = 0.5f;
        const auto measurement = measureOrFail(buffer, rate, mono);
        CHECK(measurement.framesProcessed == 1);
        CHECK(measurement.integratedLufs == kDecibelFloor);
    }

    SECTION("a zero-length buffer is accepted and measures nothing") {
        AudioBuffer buffer{mono, 0};
        const auto measurement = measureOrFail(buffer, rate, mono);
        CHECK(measurement.framesProcessed == 0);
        CHECK(measurement.integratedLufs == kDecibelFloor);
    }

    SECTION("a block with the wrong channel count is ignored") {
        AudioBuffer wrong{ChannelLayout::stereo(), 48000};
        auto meter = meterOrFail(rate, mono);
        meter.process(wrong.constView());
        CHECK(meter.framesProcessed() == 0);
    }
}

// --- Streaming --------------------------------------------------------------

TEST_CASE("Streaming block by block matches one-shot exactly", "[analysis][loudness]") {
    // Bit-identical, not merely close. Every accumulator advances in sample
    // order regardless of how the input is chopped, so any difference here
    // would mean state is leaking across block boundaries.
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::stereo();

    AudioBuffer buffer{layout, framesFor(12.0, rate)};
    fillSine(buffer, 0, 997.0, 0.25, rate);
    fillSine(buffer, 1, 1310.0, 0.18, rate);

    const auto oneShot = measureOrFail(buffer, rate, layout);

    auto meter = meterOrFail(rate, layout);
    // Deliberately awkward sizes: prime, and never a factor of the 4800-sample
    // sub-block, so blocks straddle every boundary the meter has.
    const SampleCount sizes[] = {1, 4799, 13, 4801, 7919, 2, 331};
    SampleCount offset = 0;
    int index = 0;
    while (offset < buffer.frames()) {
        const SampleCount size = std::min(sizes[index % 7], buffer.frames() - offset);
        meter.process(buffer.constView().subRange(offset, size));
        offset += size;
        ++index;
    }
    const auto streamed = meter.measurement();

    CHECK(streamed.integratedLufs == oneShot.integratedLufs);
    CHECK(streamed.momentaryLufs == oneShot.momentaryLufs);
    CHECK(streamed.shortTermLufs == oneShot.shortTermLufs);
    CHECK(streamed.maximumShortTermLufs == oneShot.maximumShortTermLufs);
    CHECK(streamed.loudnessRangeLu == oneShot.loudnessRangeLu);
    CHECK(streamed.framesProcessed == oneShot.framesProcessed);
    CHECK(streamed.gatedBlockCount == oneShot.gatedBlockCount);
}

TEST_CASE("Reset returns the meter to its initial state", "[analysis][loudness]") {
    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::mono();

    AudioBuffer buffer{layout, framesFor(5.0, rate)};
    fillSine(buffer, 0, 1000.0, 0.3, rate);

    auto meter = meterOrFail(rate, layout);
    meter.process(buffer.constView());
    const double first = meter.integratedLufs();

    meter.reset();
    CHECK(meter.integratedLufs() == kDecibelFloor);
    CHECK(meter.framesProcessed() == 0);

    meter.process(buffer.constView());
    CHECK(meter.integratedLufs() == first);
}

TEST_CASE("process allocates nothing", "[analysis][loudness][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    const SampleRate rate = kSampleRate48000;
    const auto layout = ChannelLayout::fiveOne();
    AudioBuffer buffer{layout, framesFor(4.0, rate)};
    for (int channel = 0; channel < layout.count(); ++channel) {
        fillSine(buffer, channel, 440.0 + 10.0 * static_cast<double>(channel), 0.2, rate);
    }

    auto meter = meterOrFail(rate, layout);
    const SampleCount block = 512;

    const rt::ScopedAudioThread guard;
    const rt::AllocationScope scope;

    for (SampleCount offset = 0; offset + block <= buffer.frames(); offset += block) {
        meter.process(buffer.constView().subRange(offset, block));
    }
    // The gated read-outs run over the histogram, so a UI polling them from the
    // audio thread must not allocate either.
    static_cast<void>(meter.integratedLufs());
    static_cast<void>(meter.loudnessRangeLu());
    static_cast<void>(meter.measurement());

    CHECK(scope.count() == 0);
}
