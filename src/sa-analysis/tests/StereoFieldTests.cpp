#include <sa/analysis/StereoField.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr SampleCount kFrames = 48000;

/// A stereo buffer built from a function of the sample index per channel.
template <typename Left, typename Right>
[[nodiscard]] AudioBuffer stereo(Left left, Right right, SampleCount frames = kFrames) {
    AudioBuffer audio{ChannelLayout::stereo(), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        audio.channel(0)[i] = static_cast<float>(left(i));
        audio.channel(1)[i] = static_cast<float>(right(i));
    }
    return audio;
}

[[nodiscard]] double tone(SampleCount i, double hz = 440.0, double amplitude = 0.5) {
    return amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / 48000.0);
}

} // namespace

TEST_CASE("Two identical channels are mono: correlated, narrow, and lossless summed") {
    const AudioBuffer audio =
        stereo([](SampleCount i) { return tone(i); }, [](SampleCount i) { return tone(i); });
    const auto field = StereoFieldMeter::measure(audio.view());
    REQUIRE(field);

    REQUIRE(field.value().valid);
    REQUIRE(field.value().correlation == Approx(1.0).margin(1e-9));
    REQUIRE(field.value().balanceDb == Approx(0.0).margin(1e-9));
    REQUIRE(field.value().monoLossDb == Approx(0.0).margin(1e-9));
    // No side energy at all, so the width is at the floor rather than a small
    // number that would read as "slightly wide".
    REQUIRE(field.value().widthDb < -100.0);
}

TEST_CASE("Two opposed channels cancel completely in mono") {
    // The fault the meter exists for, at its most extreme.
    const AudioBuffer audio =
        stereo([](SampleCount i) { return tone(i); }, [](SampleCount i) { return -tone(i); });
    const auto field = StereoFieldMeter::measure(audio.view());
    REQUIRE(field);

    REQUIRE(field.value().correlation == Approx(-1.0).margin(1e-9));
    REQUIRE(field.value().monoLossDb < -100.0);
    // All the energy is in the side, none in the mid.
    REQUIRE(field.value().widthDb > 100.0);
    // And it is still perfectly balanced: inverting a channel does not move it.
    REQUIRE(field.value().balanceDb == Approx(0.0).margin(1e-9));
}

TEST_CASE("Uncorrelated channels of equal power lose exactly 3 dB in mono") {
    // Arithmetic rather than damage, and the number to know: this is what a
    // genuinely wide recording reads, and it is not a fault.
    std::mt19937 engine{42};
    std::normal_distribution<double> noise{0.0, 0.2};
    AudioBuffer audio{ChannelLayout::stereo(), 480000};
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        audio.channel(0)[i] = static_cast<float>(noise(engine));
        audio.channel(1)[i] = static_cast<float>(noise(engine));
    }

    const auto field = StereoFieldMeter::measure(audio.view());
    REQUIRE(field);
    REQUIRE(field.value().correlation == Approx(0.0).margin(0.01));
    REQUIRE(field.value().monoLossDb == Approx(-3.0103).margin(0.05));
    // Equal mid and side energy, so the width is 0 dB.
    REQUIRE(field.value().widthDb == Approx(0.0).margin(0.1));
}

TEST_CASE("Balance follows the louder channel, in decibels") {
    // Right at half the amplitude of left is -6.02 dB.
    const AudioBuffer audio =
        stereo([](SampleCount i) { return tone(i); }, [](SampleCount i) { return 0.5 * tone(i); });
    const auto field = StereoFieldMeter::measure(audio.view());
    REQUIRE(field);
    REQUIRE(field.value().balanceDb == Approx(-6.0206).margin(0.01));
    // Still perfectly correlated: a level difference is not a phase problem,
    // and conflating the two is the classic misreading of these meters.
    REQUIRE(field.value().correlation == Approx(1.0).margin(1e-9));
}

TEST_CASE("A silent channel is not correlated with anything") {
    const AudioBuffer audio =
        stereo([](SampleCount i) { return tone(i); }, [](SampleCount) { return 0.0; });
    const auto field = StereoFieldMeter::measure(audio.view());
    REQUIRE(field);
    REQUIRE(field.value().valid);
    REQUIRE(field.value().correlation == Approx(0.0).margin(1e-9));
    REQUIRE(field.value().balanceDb < -100.0);
    // Half the energy goes to mid and half to side, and summing to mono costs
    // 6 dB on the one channel that had anything -- which averages to -3 over
    // the pair.
    REQUIRE(field.value().widthDb == Approx(0.0).margin(1e-6));
    REQUIRE(field.value().monoLossDb == Approx(-3.0103).margin(0.01));
}

TEST_CASE("Silence has no stereo field, and does not claim one") {
    const AudioBuffer audio =
        stereo([](SampleCount) { return 0.0; }, [](SampleCount) { return 0.0; });
    const auto field = StereoFieldMeter::measure(audio.view());
    REQUIRE(field);
    // Saying "perfectly correlated" about silence is exactly the lie a meter
    // is there to prevent.
    REQUIRE_FALSE(field.value().valid);
    REQUIRE(field.value().frames == kFrames);
}

TEST_CASE("Block size cannot be seen in the answer") {
    const AudioBuffer audio = stereo([](SampleCount i) { return tone(i, 440.0); },
                                     [](SampleCount i) { return tone(i, 443.0, 0.3); });

    const auto whole = StereoFieldMeter::measure(audio.view());
    REQUIRE(whole);

    auto piecemeal = StereoFieldMeter::create(2);
    REQUIRE(piecemeal);
    for (SampleCount at = 0; at < audio.frames();) {
        const SampleCount take = std::min<SampleCount>(37, audio.frames() - at);
        piecemeal.value().process(audio.view().subRange(at, take));
        at += take;
    }
    const StereoField pieces = piecemeal.value().field();

    REQUIRE(pieces.correlation == Approx(whole.value().correlation).margin(1e-12));
    REQUIRE(pieces.widthDb == Approx(whole.value().widthDb).margin(1e-9));
    REQUIRE(pieces.balanceDb == Approx(whole.value().balanceDb).margin(1e-9));
    REQUIRE(pieces.monoLossDb == Approx(whole.value().monoLossDb).margin(1e-9));
    REQUIRE(pieces.frames == audio.frames());
}

TEST_CASE("Anything that is not a pair is refused rather than measured") {
    for (const int channels : {1, 3, 6}) {
        REQUIRE_FALSE(StereoFieldMeter::create(channels));
        AudioBuffer audio{ChannelLayout::discrete(channels), 128};
        REQUIRE_FALSE(StereoFieldMeter::measure(audio.view()));
    }
}

TEST_CASE("Reset puts the meter back to empty") {
    auto meter = StereoFieldMeter::create(2);
    REQUIRE(meter);
    const AudioBuffer audio =
        stereo([](SampleCount i) { return tone(i); }, [](SampleCount i) { return -tone(i); });
    meter.value().process(audio.view());
    REQUIRE(meter.value().field().correlation == Approx(-1.0).margin(1e-9));

    meter.value().reset();
    REQUIRE(meter.value().framesProcessed() == 0);
    REQUIRE_FALSE(meter.value().field().valid);
}

TEST_CASE("A partly out-of-phase mix reads between the extremes") {
    // What a real problem looks like: the two channels mostly agree, but a low
    // component is inverted on one of them. In stereo it is inaudible; summed,
    // that component goes.
    const AudioBuffer audio =
        stereo([](SampleCount i) { return tone(i, 1000.0, 0.4) + tone(i, 60.0, 0.4); },
               [](SampleCount i) { return tone(i, 1000.0, 0.4) - tone(i, 60.0, 0.4); });

    const auto field = StereoFieldMeter::measure(audio.view());
    REQUIRE(field);
    // Half the energy agrees and half opposes, so the correlation sits at zero.
    REQUIRE(field.value().correlation == Approx(0.0).margin(0.02));
    // And summing costs 3 dB, because exactly half of it survives.
    REQUIRE(field.value().monoLossDb == Approx(-3.0103).margin(0.05));
}
