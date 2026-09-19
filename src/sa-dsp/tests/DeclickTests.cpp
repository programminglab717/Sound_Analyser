#include <sa/dsp/Declick.h>

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

constexpr double kRate = 48000.0;

/// Material with something for a model to get hold of: three partials and a
/// little noise, which is closer to a recording than a bare tone and much
/// closer than an impulse train.
[[nodiscard]] AudioBuffer material(SampleCount frames, int channels = 1, unsigned seed = 5) {
    std::mt19937 engine{seed};
    std::normal_distribution<float> hiss{0.0f, 0.01f};

    AudioBuffer buffer{channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo(), frames};
    for (int channel = 0; channel < channels; ++channel) {
        float* out = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i) / kRate;
            const double phase = static_cast<double>(channel) * 0.7;
            out[i] = static_cast<float>(
                0.35 * std::sin(2.0 * std::numbers::pi * 220.0 * t + phase) +
                0.20 * std::sin(2.0 * std::numbers::pi * 553.0 * t + phase) +
                0.10 * std::sin(2.0 * std::numbers::pi * 1310.0 * t + phase) + hiss(engine));
        }
    }
    return buffer;
}

[[nodiscard]] AudioBuffer copyOf(const AudioBuffer& source) {
    AudioBuffer copy{source.layout(), source.frames()};
    for (int channel = 0; channel < source.channelCount(); ++channel) {
        std::copy_n(source.channel(channel), source.frames(), copy.channel(channel));
    }
    return copy;
}

/// Error energy of `actual` against `wanted`, relative to the signal, in dB.
[[nodiscard]] double errorDb(const AudioBuffer& actual, const AudioBuffer& wanted, int channel) {
    double error = 0.0;
    double signal = 0.0;
    for (SampleCount i = 0; i < wanted.frames(); ++i) {
        const double difference = static_cast<double>(actual.channel(channel)[i]) -
                                  static_cast<double>(wanted.channel(channel)[i]);
        error += difference * difference;
        signal += static_cast<double>(wanted.channel(channel)[i]) *
                  static_cast<double>(wanted.channel(channel)[i]);
    }
    return error > 0.0 ? 10.0 * std::log10(error / signal) : -200.0;
}

[[nodiscard]] double worstDifference(const AudioBuffer& a, const AudioBuffer& b, int channel) {
    double worst = 0.0;
    for (SampleCount i = 0; i < a.frames(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(a.channel(channel)[i]) -
                                         static_cast<double>(b.channel(channel)[i])));
    }
    return worst;
}

} // namespace

TEST_CASE("Clean material is left exactly alone", "[dsp][declick]") {
    // The first thing a restoration tool has to get right. A declicker that
    // finds clicks in undamaged audio is worse than no declicker, because the
    // user cannot hear what it did until the master is out.
    AudioBuffer audio = material(96000);
    const AudioBuffer before = copyOf(audio);

    const auto report = declick(audio.view());
    REQUIRE(report);
    INFO("found " << report.value().clicks << " clicks in clean material");
    CHECK(report.value().clicks == 0);
    CHECK(report.value().samplesRepaired == 0);
    CHECK(worstDifference(audio, before, 0) == 0.0);
}

TEST_CASE("Noise alone is not mistaken for damage", "[dsp][declick]") {
    // The hardest clean case: in noise the model has nothing to predict, so
    // every sample is a surprise and only the *relative* size of the surprise
    // separates a click from the material.
    AudioBuffer audio{ChannelLayout::mono(), 96000};
    std::mt19937 engine{909};
    std::normal_distribution<float> distribution{0.0f, 0.2f};
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        audio.channel(0)[i] = distribution(engine);
    }
    const AudioBuffer before = copyOf(audio);

    const auto report = declick(audio.view());
    REQUIRE(report);
    INFO("found " << report.value().clicks << " clicks in 2 seconds of noise");
    // Gaussian noise does throw the occasional five-sigma sample, and at 96000
    // of them a couple is arithmetic rather than a fault. What matters is that
    // it is a couple and not thousands.
    CHECK(report.value().clicks <= 3);
    CHECK(errorDb(audio, before, 0) < -60.0);
}

TEST_CASE("Clicks are found and taken out", "[dsp][declick]") {
    const AudioBuffer clean = material(96000);
    AudioBuffer damaged = copyOf(clean);

    // Twenty clicks of one to four samples, at known places, well clear of the
    // ends and of each other.
    const std::vector<SampleIndex> places{4000,  8000,  12000, 17000, 21000, 25000, 30000,
                                          34000, 38000, 43000, 47000, 51000, 56000, 60000,
                                          64000, 69000, 73000, 77000, 82000, 86000};
    std::mt19937 engine{31};
    std::uniform_int_distribution<int> lengths{1, 4};
    std::uniform_real_distribution<float> heights{0.4f, 0.9f};
    SampleCount damagedSamples = 0;
    for (const SampleIndex place : places) {
        const int length = lengths(engine);
        const float height = heights(engine) * (engine() % 2 == 0 ? 1.0f : -1.0f);
        for (int i = 0; i < length; ++i) {
            damaged.channel(0)[place + i] += height;
        }
        damagedSamples += length;
    }

    AudioBuffer repaired = copyOf(damaged);
    const auto report = declick(repaired.view());
    REQUIRE(report);

    const double before = errorDb(damaged, clean, 0);
    const double after = errorDb(repaired, clean, 0);
    INFO("found " << report.value().clicks << " of " << places.size() << ", error " << before
                  << " dB -> " << after << " dB, longest repair " << report.value().longestRepair);

    CHECK(report.value().clicks == static_cast<int>(places.size()));
    CHECK(report.value().tooLong == 0);
    CHECK(report.value().unsolved == 0);
    // The damage is gone, not merely reduced.
    CHECK(after < before - 30.0);
    // And the repair is the length of the damage, not the length of the ring
    // it makes in the model -- with a 32-pole model, a detector working off the
    // forward residual alone would repair over thirty samples for a one-sample
    // click.
    CHECK(report.value().longestRepair <= 4 + 2 * 1 + 2);
    CHECK(report.value().samplesRepaired <
          damagedSamples + 6 * static_cast<SampleCount>(places.size()));
}

TEST_CASE("A repair continues the signal rather than drawing across it", "[dsp][declick]") {
    // What separates interpolation through a model from linear interpolation:
    // the repaired samples should be what the material was doing, phase
    // included, not a straight line between the edges.
    const AudioBuffer clean = material(48000);
    AudioBuffer damaged = copyOf(clean);
    constexpr SampleIndex kAt = 24000;
    for (int i = 0; i < 6; ++i) {
        damaged.channel(0)[kAt + i] = 0.95f;
    }

    const auto report = declick(damaged.view());
    REQUIRE(report);
    REQUIRE(report.value().clicks == 1);

    double worst = 0.0;
    for (SampleIndex i = kAt - 2; i < kAt + 8; ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(damaged.channel(0)[i]) -
                                         static_cast<double>(clean.channel(0)[i])));
    }
    INFO("worst difference across the repair " << worst);
    // The signal peaks around 0.6 here, so this is the repair landing within a
    // few percent of what was actually there.
    CHECK(worst < 0.05);
}

TEST_CASE("Damage too long to be a click is counted, not invented over", "[dsp][declick]") {
    // A 300-sample burst is a dropout. Reconstructing it from a 32-pole model
    // would be composition, not repair, so it is reported and left.
    const AudioBuffer clean = material(48000);
    AudioBuffer damaged = copyOf(clean);
    std::mt19937 engine{17};
    std::uniform_real_distribution<float> distribution{-0.9f, 0.9f};
    for (int i = 0; i < 300; ++i) {
        damaged.channel(0)[20000 + i] = distribution(engine);
    }
    const AudioBuffer before = copyOf(damaged);

    DeclickSettings settings;
    settings.maximumGap = 64;
    const auto report = declick(damaged.view(), settings);
    REQUIRE(report);
    INFO("repaired " << report.value().clicks << ", left " << report.value().tooLong);
    CHECK(report.value().tooLong >= 1);

    // Whatever it did elsewhere, it did not touch the burst.
    double worst = 0.0;
    for (SampleIndex i = 20000; i < 20300; ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(damaged.channel(0)[i]) -
                                         static_cast<double>(before.channel(0)[i])));
    }
    CHECK(worst == 0.0);
}

TEST_CASE("Channels are repaired separately", "[dsp][declick]") {
    // A click on a transfer is usually on one channel. Repairing both would
    // damage the good one, and repairing neither would leave the bad one.
    const AudioBuffer clean = material(48000, 2);
    AudioBuffer damaged = copyOf(clean);
    damaged.channel(0)[24000] += 0.9f;
    damaged.channel(0)[24001] += 0.8f;

    const auto report = declick(damaged.view());
    REQUIRE(report);
    CHECK(report.value().clicks == 1);
    CHECK(worstDifference(damaged, clean, 1) == 0.0);
    CHECK(worstDifference(damaged, clean, 0) < 0.05);
}

TEST_CASE("Silence has no model and is passed over", "[dsp][declick]") {
    AudioBuffer audio{ChannelLayout::mono(), 48000};
    const auto report = declick(audio.view());
    REQUIRE(report);
    CHECK(report.value().clicks == 0);
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        CHECK(audio.channel(0)[i] == 0.0f);
    }
}

TEST_CASE("Declicking refuses what it cannot do", "[dsp][declick]") {
    AudioBuffer audio = material(48000);
    const AudioBufferView view = audio.view();

    CHECK_FALSE(declick(AudioBufferView{}).hasValue());

    DeclickSettings settings;
    settings.order = 0;
    CHECK_FALSE(declick(view, settings).hasValue());
    settings.order = 500;
    CHECK_FALSE(declick(view, settings).hasValue());

    settings = DeclickSettings{};
    settings.blockSize = 64; // Less than four times the order.
    CHECK_FALSE(declick(view, settings).hasValue());

    settings = DeclickSettings{};
    settings.maximumGap = 0;
    CHECK_FALSE(declick(view, settings).hasValue());
    settings.maximumGap = kMaximumDeclickGap + 1;
    CHECK_FALSE(declick(view, settings).hasValue());

    settings = DeclickSettings{};
    settings.guard = -1;
    CHECK_FALSE(declick(view, settings).hasValue());

    settings = DeclickSettings{};
    settings.threshold = 0.0;
    CHECK_FALSE(declick(view, settings).hasValue());
}
