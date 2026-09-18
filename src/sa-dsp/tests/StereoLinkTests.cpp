#include <sa/core/RealtimeGuard.h>
#include <sa/dsp/Decibels.h>
#include <sa/dsp/Dynamics.h>
#include <sa/dsp/StereoLink.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr std::size_t kLength = 24000;
constexpr std::size_t kTransientStart = 4800;
constexpr std::size_t kTransientEnd = 5300;

template <typename Processor>
StereoLink<Processor> madeLink(const typename Processor::Settings& settings,
                               const StereoLinkSettings& link) {
    Result<StereoLink<Processor>> result =
        StereoLink<Processor>::create(kSampleRate48000, settings, link);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

double toGain(double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

/// A steady centred tone in both channels, with a loud transient in the left
/// channel only.
///
/// The centred tone is what the ear locates: if the two channels are given
/// different gains it stops being centred, and the difference signal says by how
/// much. The transient is hard panned and short, which is the case a pair of
/// independent processors handles worst.
struct PannedProgramme {
    std::vector<float> left;
    std::vector<float> right;
};

PannedProgramme pannedTransient(double toneDb = -20.0, double transientDb = 0.0) {
    PannedProgramme programme;
    programme.left.assign(kLength, 0.0f);
    programme.right.assign(kLength, 0.0f);

    const double toneGain = toGain(toneDb);
    for (std::size_t i = 0; i < kLength; ++i) {
        const double tone =
            toneGain * std::sin(2.0 * std::numbers::pi * 200.0 * static_cast<double>(i) / kRate);
        programme.left[i] = static_cast<float>(tone);
        programme.right[i] = static_cast<float>(tone);
    }
    for (std::size_t i = kTransientStart; i < kTransientEnd; ++i) {
        const double burst =
            toGain(transientDb) * std::sin(2.0 * std::numbers::pi * 1200.0 *
                                           static_cast<double>(i - kTransientStart) / kRate);
        programme.left[i] += static_cast<float>(burst);
    }
    return programme;
}

/// How far off centre the image has moved, in decibels: the difference between
/// the channels against their sum, over a window in which the input is
/// identical in both channels.
///
/// Measured after the transient has gone, so anything that survives is the
/// processor's own doing rather than the programme's.
double imageShiftDb(const std::vector<float>& left, const std::vector<float>& right,
                    std::size_t from, std::size_t to) {
    double side = 0.0;
    double mid = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        const double difference = static_cast<double>(left[i]) - static_cast<double>(right[i]);
        const double sum = static_cast<double>(left[i]) + static_cast<double>(right[i]);
        side += difference * difference;
        mid += sum * sum;
    }
    if (mid <= 0.0) {
        return 0.0;
    }
    return 10.0 * std::log10(std::max(side, 1e-30) / mid);
}

template <typename Processor>
double largestGainDifferenceDb(StereoLink<Processor>& link, const PannedProgramme& programme) {
    double largest = 0.0;
    for (std::size_t i = 0; i < kLength; ++i) {
        float leftOutput = 0.0f;
        float rightOutput = 0.0f;
        link.processSample(programme.left[i], programme.right[i], leftOutput, rightOutput);
        largest = std::max(
            largest, std::abs(link.left().gainReductionDb() - link.right().gainReductionDb()));
    }
    return largest;
}

} // namespace

TEST_CASE("Stereo link settings are validated", "[dsp][dynamics][stereo]") {
    CHECK(StereoLink<Compressor>::create(kSampleRate48000, CompressorSettings{}).hasValue());
    CHECK(StereoLink<Compressor>::create(kSampleRate48000, CompressorSettings{},
                                         StereoLinkSettings{0.0, StereoLinkMode::Average})
              .hasValue());

    CHECK_FALSE(StereoLink<Compressor>::create(kSampleRate48000, CompressorSettings{},
                                               StereoLinkSettings{-0.1, StereoLinkMode::Maximum})
                    .hasValue());
    CHECK_FALSE(StereoLink<Compressor>::create(kSampleRate48000, CompressorSettings{},
                                               StereoLinkSettings{1.1, StereoLinkMode::Maximum})
                    .hasValue());
    CHECK_FALSE(
        StereoLink<Compressor>::create(
            kSampleRate48000, CompressorSettings{},
            StereoLinkSettings{std::numeric_limits<double>::quiet_NaN(), StereoLinkMode::Maximum})
            .hasValue());
    // The processor's own validation still applies through the wrapper.
    CHECK_FALSE(StereoLink<Compressor>::create(SampleRate{0.0}, CompressorSettings{}).hasValue());
    CHECK_FALSE(StereoLink<Compressor>::create(kSampleRate48000, CompressorSettings{.ratio = 0.5})
                    .hasValue());

    StereoLink<Compressor> link = madeLink<Compressor>(CompressorSettings{}, StereoLinkSettings{});
    CHECK(link.link().amount == Approx(1.0));
    CHECK_FALSE(link.setLink(StereoLinkSettings{2.0, StereoLinkMode::Maximum}).ok());
    CHECK(link.link().amount == Approx(1.0));
    CHECK(link.setLink(StereoLinkSettings{0.5, StereoLinkMode::Average}).ok());
    CHECK(link.link().amount == Approx(0.5));
    CHECK(link.setSettings(CompressorSettings{.thresholdDb = -12.0}).ok());
    CHECK(link.left().settings().thresholdDb == Approx(-12.0));
    CHECK(link.right().settings().thresholdDb == Approx(-12.0));
    CHECK_FALSE(link.setSettings(CompressorSettings{.ratio = 0.0}).ok());
}

TEST_CASE("A hard-panned transient does not move the image when the pair is linked",
          "[dsp][dynamics][stereo]") {
    // The failure this exists to fix. A 0 dBFS burst in the left channel alone,
    // over a centred tone at -20 dBFS, through a compressor that takes 15 dB off
    // whatever crosses its threshold.
    //
    // Measured over the release, after the burst has gone, as the difference
    // between the channels against their sum:
    //   unlinked     -8.0 dB -- the centred tone has moved most of the way over
    //                to the right channel and stays there for the release
    //   half linked  -17.7 dB
    //   linked       -313 dB, which is the difference being exactly zero in
    //                float and the logarithm of nothing
    const PannedProgramme programme = pannedTransient();
    const CompressorSettings settings{.thresholdDb = -20.0,
                                      .ratio = 4.0,
                                      .attackSeconds = 0.005,
                                      .releaseSeconds = 0.200,
                                      .kneeDb = 6.0,
                                      .makeupGainDb = 0.0};

    const auto run = [&](double amount) {
        StereoLink<Compressor> link =
            madeLink<Compressor>(settings, StereoLinkSettings{amount, StereoLinkMode::Maximum});
        std::vector<float> left = programme.left;
        std::vector<float> right = programme.right;
        link.processInPlace(left.data(), right.data(), static_cast<SampleCount>(kLength));
        return imageShiftDb(left, right, kTransientEnd + 200, kTransientEnd + 6000);
    };

    const double unlinked = run(0.0);
    const double linked = run(1.0);
    const double half = run(0.5);

    INFO("unlinked " << unlinked << " dB, half " << half << " dB, linked " << linked << " dB");
    CHECK(unlinked > -12.0);
    CHECK(linked < -100.0);
    // Halfway is between the two, and nearer the unlinked end: half the link
    // still leaves half the difference in the gains.
    CHECK(half < unlinked - 3.0);
    CHECK(half > linked + 40.0);
}

TEST_CASE("At full link every processor applies the same gain to both channels",
          "[dsp][dynamics][stereo]") {
    // Stronger than the image measurement above, and the property the image
    // measurement follows from: the two gain reductions are not merely close,
    // they are the same number.
    const PannedProgramme programme = pannedTransient();
    const StereoLinkSettings linked{1.0, StereoLinkMode::Maximum};

    {
        StereoLink<Compressor> link =
            madeLink<Compressor>(CompressorSettings{.thresholdDb = -24.0, .ratio = 4.0}, linked);
        CHECK(largestGainDifferenceDb(link, programme) == 0.0);
    }
    {
        StereoLink<Limiter> link = madeLink<Limiter>(
            LimiterSettings{.ceilingDb = -1.0, .releaseSeconds = 0.05, .lookAheadSeconds = 0.005},
            linked);
        CHECK(largestGainDifferenceDb(link, programme) == 0.0);
    }
    {
        StereoLink<Gate> link =
            madeLink<Gate>(GateSettings{.thresholdDb = -30.0, .hysteresisDb = 3.0}, linked);
        CHECK(largestGainDifferenceDb(link, programme) == 0.0);
    }
    {
        StereoLink<Expander> link =
            madeLink<Expander>(ExpanderSettings{.thresholdDb = -30.0, .ratio = 2.0}, linked);
        CHECK(largestGainDifferenceDb(link, programme) == 0.0);
    }
}

TEST_CASE("A linked true-peak limiter holds the ceiling on both channels",
          "[dsp][dynamics][stereo][truepeak]") {
    // The clamp at the output is the one part of the limiter that is per-sample
    // rather than per-sidechain, so it is the part a link could still pull off
    // centre. It is made against the level it was handed, which is the linked
    // one -- hence the same gain on both channels, and the ceiling still held.
    LimiterSettings settings{.ceilingDb = -1.0, .releaseSeconds = 0.050, .lookAheadSeconds = 0.002};
    settings.truePeak = true;
    StereoLink<Limiter> link =
        madeLink<Limiter>(settings, StereoLinkSettings{1.0, StereoLinkMode::Maximum});

    const PannedProgramme programme = pannedTransient(-6.0, 12.0);
    std::vector<float> left = programme.left;
    std::vector<float> right = programme.right;
    link.processInPlace(left.data(), right.data(), static_cast<SampleCount>(kLength));

    const double ceiling = toGain(-1.0);
    double leftPeak = 0.0;
    double rightPeak = 0.0;
    for (std::size_t i = 0; i < kLength; ++i) {
        leftPeak = std::max(leftPeak, std::abs(static_cast<double>(left[i])));
        rightPeak = std::max(rightPeak, std::abs(static_cast<double>(right[i])));
    }
    CHECK(leftPeak <= ceiling * (1.0 + 1e-6));
    CHECK(rightPeak <= ceiling * (1.0 + 1e-6));
    CHECK(imageShiftDb(left, right, kTransientEnd + 200, kTransientEnd + 6000) < -100.0);
}

TEST_CASE("Zero link is exactly two independent processors", "[dsp][dynamics][stereo]") {
    const PannedProgramme programme = pannedTransient();
    const CompressorSettings settings{.thresholdDb = -20.0, .ratio = 4.0};

    StereoLink<Compressor> link =
        madeLink<Compressor>(settings, StereoLinkSettings{0.0, StereoLinkMode::Maximum});
    std::vector<float> left = programme.left;
    std::vector<float> right = programme.right;
    link.processInPlace(left.data(), right.data(), static_cast<SampleCount>(kLength));

    Result<Compressor> alone = Compressor::create(kSampleRate48000, settings);
    REQUIRE(alone.hasValue());
    Compressor single = std::move(alone).value();
    std::vector<float> reference = programme.left;
    single.processInPlace(reference.data(), static_cast<SampleCount>(kLength));

    for (std::size_t i = 0; i < kLength; ++i) {
        REQUIRE(left[i] == reference[i]);
    }
}

TEST_CASE("Maximum and average sidechains differ by what the quiet channel is worth",
          "[dsp][dynamics][stereo]") {
    // One channel loud, the other silent. The maximum sidechain acts on the loud
    // channel's level, so both channels take the full reduction. The average
    // sidechain sees half that level -- 6 dB less -- and reduces by less.
    //
    // Measured settled reduction: 17.98 dB on maximum against 13.47 dB on
    // average, a difference of 4.52 dB -- which is the 6.02 dB the average
    // loses from the silent channel, put through a ratio of 4.
    std::vector<float> loud(kLength);
    std::vector<float> silent(kLength, 0.0f);
    for (std::size_t i = 0; i < kLength; ++i) {
        loud[i] = static_cast<float>(
            0.5 * ((i / 60) % 2 == 0 ? 1.0 : -1.0)); // square wave: one constant magnitude
    }

    const CompressorSettings settings{.thresholdDb = -30.0,
                                      .ratio = 4.0,
                                      .attackSeconds = 0.001,
                                      .releaseSeconds = 0.010,
                                      .kneeDb = 0.0};

    const auto settled = [&](StereoLinkMode mode) {
        StereoLink<Compressor> link = madeLink<Compressor>(settings, StereoLinkSettings{1.0, mode});
        std::vector<float> left = loud;
        std::vector<float> right = silent;
        link.processInPlace(left.data(), right.data(), static_cast<SampleCount>(kLength));
        return link.left().gainReductionDb();
    };

    const double maximum = settled(StereoLinkMode::Maximum);
    const double average = settled(StereoLinkMode::Average);
    INFO("maximum " << maximum << " dB, average " << average << " dB");

    // -6.02 dB on the input at a ratio of 4 is 4.52 dB less reduction.
    CHECK(maximum - average == Approx(6.02 * (1.0 - 1.0 / 4.0)).margin(0.05));
    CHECK(maximum > average);
}

TEST_CASE("Degenerate stereo blocks are no-ops", "[dsp][dynamics][stereo]") {
    StereoLink<Compressor> link = madeLink<Compressor>(CompressorSettings{}, StereoLinkSettings{});
    float left = 0.5f;
    float right = -0.25f;
    link.process(&left, &right, &left, &right, 0);
    link.process(&left, &right, &left, &right, -8);
    CHECK(left == 0.5f);
    CHECK(right == -0.25f);
    link.reset();
    CHECK(link.left().gainReductionDb() == Approx(0.0));
}

TEST_CASE("Linked dynamics on the audio thread allocate nothing", "[dsp][dynamics][stereo][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    StereoLink<Compressor> compressor =
        madeLink<Compressor>(CompressorSettings{}, StereoLinkSettings{});
    LimiterSettings limiterSettings{};
    limiterSettings.truePeak = true;
    StereoLink<Limiter> limiter = madeLink<Limiter>(limiterSettings, StereoLinkSettings{});
    StereoLink<Gate> gate = madeLink<Gate>(GateSettings{}, StereoLinkSettings{});
    StereoLink<Expander> expander = madeLink<Expander>(ExpanderSettings{}, StereoLinkSettings{});

    std::vector<float> left(512, 0.3f);
    std::vector<float> right(512, -0.2f);

    std::size_t allocations = 0;
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;

        const auto count = static_cast<SampleCount>(left.size());
        compressor.processInPlace(left.data(), right.data(), count);
        limiter.processInPlace(left.data(), right.data(), count);
        gate.processInPlace(left.data(), right.data(), count);
        expander.processInPlace(left.data(), right.data(), count);
        compressor.reset();
        limiter.reset();
        gate.reset();
        expander.reset();
        static_cast<void>(compressor.setLink(StereoLinkSettings{0.5, StereoLinkMode::Average}));

        allocations = scope.count();
    }
    CHECK(allocations == 0);
}
