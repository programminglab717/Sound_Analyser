#include <sa/core/RealtimeGuard.h>
#include <sa/dsp/ParametricEq.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

ParametricEq makeEq(SampleRate rate = kSampleRate48000) {
    Result<ParametricEq> result = ParametricEq::create(rate);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

EqBand peak(double frequency, double q, double gainDb) {
    return EqBand{FilterSpec{FilterType::Peaking, frequency, q, gainDb}, true};
}

/// Response measured by pushing an impulse through the EQ itself, then taking a
/// single-frequency DFT of what came out. Independent of the EQ's own
/// magnitudeDbAt(), which is evaluated from the coefficients instead.
double measuredDb(ParametricEq& eq, double frequency, int length = 16384) {
    eq.reset();
    std::complex<double> sum{0.0, 0.0};
    for (int i = 0; i < length; ++i) {
        const float out = eq.processSample(i == 0 ? 1.0f : 0.0f);
        const double angle = -2.0 * std::numbers::pi * frequency * static_cast<double>(i) / kRate;
        sum += static_cast<double>(out) * std::complex<double>{std::cos(angle), std::sin(angle)};
    }
    return 20.0 * std::log10(std::abs(sum));
}

std::vector<float> whiteNoise(std::size_t count, unsigned seed) {
    std::mt19937 rng{seed};
    std::uniform_real_distribution<float> dist{-1.0f, 1.0f};
    std::vector<float> noise(count);
    for (float& sample : noise) {
        sample = dist(rng);
    }
    return noise;
}

} // namespace

TEST_CASE("A parametric EQ validates its sample rate", "[dsp][eq]") {
    CHECK(ParametricEq::create(kSampleRate48000).hasValue());
    CHECK_FALSE(ParametricEq::create(SampleRate{0.0}).hasValue());
    CHECK_FALSE(ParametricEq::create(SampleRate{-48000.0}).hasValue());
    CHECK_FALSE(ParametricEq::create(SampleRate{1e9}).hasValue());
}

TEST_CASE("An EQ with no bands is a wire", "[dsp][eq]") {
    ParametricEq eq = makeEq();
    CHECK(eq.bandCount() == 0);
    CHECK(eq.band(0) == nullptr);

    const std::vector<float> block = whiteNoise(128, 1);
    std::vector<float> output(block.size());
    eq.process(block.data(), output.data(), static_cast<SampleCount>(block.size()));
    for (std::size_t i = 0; i < block.size(); ++i) {
        REQUIRE(output[i] == block[i]);
    }
    CHECK(eq.magnitudeDbAt(1000.0) == Approx(0.0).margin(1e-12));
}

TEST_CASE("Bands land where they were placed", "[dsp][eq]") {
    ParametricEq eq = makeEq();
    REQUIRE(eq.addBand(peak(200.0, 4.0, 6.0)).hasValue());
    REQUIRE(eq.addBand(peak(5000.0, 4.0, -4.0)).hasValue());
    REQUIRE(eq.addBand(EqBand{FilterSpec{FilterType::HighPass, 30.0, kButterworthQ, 0.0}, true})
                .hasValue());
    REQUIRE(eq.bandCount() == 3);

    // Each band's own signature, measured through the whole chain: the boost,
    // the cut and the high-pass corner are all where they were asked for even
    // though they are running in series. The bands are spaced far enough apart
    // that their skirts overlap by under a hundredth of a decibel, so what is
    // measured at each centre is that band and not the sum of three.
    CHECK(measuredDb(eq, 200.0) == Approx(6.0).margin(0.05));
    CHECK(measuredDb(eq, 5000.0) == Approx(-4.0).margin(0.05));
    CHECK(measuredDb(eq, 30.0) == Approx(-3.0103).margin(0.05));
}

TEST_CASE("The drawn curve is the curve being heard", "[dsp][eq]") {
    // magnitudeDbAt() is what the UI paints over the analyser. If it is
    // evaluated from anything other than the coefficients actually loaded into
    // the filters, the display slowly stops describing the audio.
    ParametricEq eq = makeEq();
    REQUIRE(eq.addBand(peak(200.0, 0.8, 8.0)).hasValue());
    REQUIRE(
        eq.addBand(EqBand{FilterSpec{FilterType::HighShelf, 6000.0, kButterworthQ, -10.0}, true})
            .hasValue());
    REQUIRE(
        eq.addBand(EqBand{FilterSpec{FilterType::LowPass, 15000.0, 1.2, 0.0}, true}).hasValue());

    for (double frequency : {30.0, 200.0, 1000.0, 6000.0, 15000.0, 20000.0}) {
        INFO("frequency " << frequency);
        CHECK(measuredDb(eq, frequency) == Approx(eq.magnitudeDbAt(frequency)).margin(0.05));
    }
}

TEST_CASE("A disabled band contributes nothing", "[dsp][eq]") {
    ParametricEq eq = makeEq();
    const Result<int> index = eq.addBand(peak(1000.0, 2.0, 12.0));
    REQUIRE(index.hasValue());
    CHECK(measuredDb(eq, 1000.0) == Approx(12.0).margin(0.05));

    EqBand disabled = peak(1000.0, 2.0, 12.0);
    disabled.enabled = false;
    REQUIRE(eq.setBand(index.value(), disabled).ok());

    // The band keeps its slot and its parameters -- it just stops doing
    // anything, so switching it back on restores exactly what was there.
    CHECK(eq.bandCount() == 1);
    REQUIRE(eq.band(0) != nullptr);
    CHECK(eq.band(0)->filter.gainDb == Approx(12.0));
    CHECK_FALSE(eq.band(0)->enabled);
    CHECK(measuredDb(eq, 1000.0) == Approx(0.0).margin(1e-4));

    EqBand enabled = disabled;
    enabled.enabled = true;
    REQUIRE(eq.setBand(0, enabled).ok());
    CHECK(measuredDb(eq, 1000.0) == Approx(12.0).margin(0.05));
}

TEST_CASE("Removing a band shifts the rest down", "[dsp][eq]") {
    ParametricEq eq = makeEq();
    REQUIRE(eq.addBand(peak(200.0, 2.0, 6.0)).hasValue());
    REQUIRE(eq.addBand(peak(1000.0, 2.0, -6.0)).hasValue());
    REQUIRE(eq.addBand(peak(5000.0, 2.0, 9.0)).hasValue());

    REQUIRE(eq.removeBand(1).ok());
    REQUIRE(eq.bandCount() == 2);
    REQUIRE(eq.band(0) != nullptr);
    REQUIRE(eq.band(1) != nullptr);
    CHECK(eq.band(0)->filter.frequency == Approx(200.0));
    CHECK(eq.band(1)->filter.frequency == Approx(5000.0));
    CHECK(eq.band(2) == nullptr);

    CHECK(measuredDb(eq, 200.0) == Approx(6.0).margin(0.05));
    CHECK(measuredDb(eq, 5000.0) == Approx(9.0).margin(0.05));
    // The cut is gone: what is left at 1 kHz is only the skirts of the two
    // survivors, which the drawn curve agrees with.
    CHECK(measuredDb(eq, 1000.0) > -1.0);
    CHECK(measuredDb(eq, 1000.0) == Approx(eq.magnitudeDbAt(1000.0)).margin(0.05));

    CHECK_FALSE(eq.removeBand(-1).ok());
    CHECK_FALSE(eq.removeBand(2).ok());

    eq.clearBands();
    CHECK(eq.bandCount() == 0);
    CHECK(eq.magnitudeDbAt(200.0) == Approx(0.0).margin(1e-12));
}

TEST_CASE("Unrealisable bands are rejected and change nothing", "[dsp][eq]") {
    ParametricEq eq = makeEq();
    REQUIRE(eq.addBand(peak(1000.0, 2.0, 6.0)).hasValue());

    // Above Nyquist, zero Q, and a frequency of zero: all impossible rather
    // than merely unusual, so they fail instead of being clamped into
    // something that would quietly not be what was asked for.
    const Result<int> tooHigh = eq.addBand(peak(30000.0, 2.0, 6.0));
    CHECK_FALSE(tooHigh.hasValue());
    CHECK(tooHigh.error().code() == ErrorCode::InvalidArgument);
    CHECK_FALSE(eq.addBand(peak(1000.0, 0.0, 6.0)).hasValue());
    CHECK_FALSE(eq.addBand(peak(0.0, 2.0, 6.0)).hasValue());
    CHECK(eq.bandCount() == 1);

    CHECK_FALSE(eq.setBand(0, peak(40000.0, 2.0, 6.0)).ok());
    REQUIRE(eq.band(0) != nullptr);
    CHECK(eq.band(0)->filter.frequency == Approx(1000.0));
    CHECK_FALSE(eq.setBand(1, peak(1000.0, 2.0, 6.0)).ok());
}

TEST_CASE("The EQ refuses to grow past its capacity", "[dsp][eq]") {
    ParametricEq eq = makeEq();
    for (int i = 0; i < ParametricEq::kMaxBands; ++i) {
        INFO("band " << i);
        REQUIRE(eq.addBand(peak(100.0 + 100.0 * static_cast<double>(i), 2.0, 1.0)).hasValue());
    }
    const Result<int> overflow = eq.addBand(peak(9000.0, 2.0, 1.0));
    CHECK_FALSE(overflow.hasValue());
    CHECK(overflow.error().code() == ErrorCode::OutOfRange);
    CHECK(eq.bandCount() == ParametricEq::kMaxBands);
}

TEST_CASE("Changing the sample rate keeps the bands in hertz", "[dsp][eq]") {
    ParametricEq eq = makeEq();
    REQUIRE(eq.addBand(peak(1000.0, 2.0, 6.0)).hasValue());
    REQUIRE(eq.setSampleRate(kSampleRate96000).ok());
    CHECK(eq.sampleRate() == kSampleRate96000);
    // Still +6 dB at 1 kHz -- the band moved with the rate rather than staying
    // at the same fraction of it.
    CHECK(eq.magnitudeDbAt(1000.0) == Approx(6.0).margin(0.01));

    // A band that cannot exist at the new rate leaves everything alone.
    REQUIRE(eq.addBand(peak(30000.0, 2.0, 3.0)).hasValue());
    CHECK_FALSE(eq.setSampleRate(kSampleRate48000).ok());
    CHECK(eq.sampleRate() == kSampleRate96000);
    CHECK(eq.magnitudeDbAt(30000.0) == Approx(3.0).margin(0.01));

    CHECK_FALSE(eq.setSampleRate(SampleRate{0.0}).ok());
}

TEST_CASE("reset() clears EQ state", "[dsp][eq]") {
    ParametricEq eq = makeEq();
    REQUIRE(eq.addBand(peak(500.0, 6.0, 12.0)).hasValue());
    REQUIRE(eq.addBand(EqBand{FilterSpec{FilterType::HighPass, 80.0, kButterworthQ, 0.0}, true})
                .hasValue());

    const std::vector<float> block = whiteNoise(512, 13);
    std::vector<float> first(block.size());
    std::vector<float> second(block.size());
    const auto count = static_cast<SampleCount>(block.size());

    eq.process(block.data(), first.data(), count);
    eq.process(block.data(), second.data(), count);
    eq.reset();
    eq.process(block.data(), second.data(), count);

    for (std::size_t i = 0; i < block.size(); ++i) {
        INFO("sample " << i);
        REQUIRE(second[i] == first[i]);
    }
}

TEST_CASE("Adding a band does not restart the others", "[dsp][eq]") {
    // Appending a section leaves the earlier ones' state untouched, so a band
    // added while audio is running does not silence what is already ringing --
    // which is heard as a click on every EQ edit.
    ParametricEq eq = makeEq();
    REQUIRE(eq.addBand(peak(300.0, 8.0, 12.0)).hasValue());

    const std::vector<float> noise = whiteNoise(256, 23);
    for (float sample : noise) {
        static_cast<void>(eq.processSample(sample));
    }

    ParametricEq untouched = eq;
    REQUIRE(eq.addBand(peak(8000.0, 1.0, 1.0)).hasValue());

    double reference = 0.0;
    double difference = 0.0;
    for (int i = 0; i < 64; ++i) {
        const double expected = static_cast<double>(untouched.processSample(0.0f));
        const double actual = static_cast<double>(eq.processSample(0.0f));
        reference = std::max(reference, std::abs(expected));
        difference = std::max(difference, std::abs(actual - expected));
    }

    // The ringing has to be worth measuring before its survival means anything.
    REQUIRE(reference > 0.01);
    CHECK(difference < reference * 0.1);
}

TEST_CASE("Degenerate EQ blocks are no-ops", "[dsp][eq]") {
    ParametricEq eq = makeEq();
    REQUIRE(eq.addBand(peak(1000.0, 2.0, 6.0)).hasValue());
    float sentinel = 0.75f;
    eq.process(&sentinel, &sentinel, 0);
    eq.process(&sentinel, &sentinel, -8);
    CHECK(sentinel == 0.75f);
}

TEST_CASE("EQ processing on the audio thread allocates nothing", "[dsp][eq][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    ParametricEq eq = makeEq();
    for (int i = 0; i < ParametricEq::kMaxBands; ++i) {
        REQUIRE(eq.addBand(peak(100.0 + 500.0 * static_cast<double>(i), 1.4, 3.0)).hasValue());
    }
    std::vector<float> block = whiteNoise(512, 41);
    std::vector<float> output(block.size());

    std::size_t allocations = 0;
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;

        const auto count = static_cast<SampleCount>(block.size());
        eq.process(block.data(), output.data(), count);
        eq.processInPlace(output.data(), count);
        static_cast<void>(eq.processSample(0.2f));
        eq.reset();

        allocations = scope.count();
    }
    CHECK(allocations == 0);
}

TEST_CASE("Band editing allocates nothing either", "[dsp][eq][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    // Not because a parameter change belongs on the audio thread -- it does
    // not -- but because an EQ whose editing allocates cannot be held by value
    // in a pre-allocated parameter object either.
    ParametricEq eq = makeEq();
    REQUIRE(eq.addBand(peak(1000.0, 2.0, 6.0)).hasValue());

    std::size_t allocations = 0;
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;

        static_cast<void>(eq.addBand(peak(4000.0, 1.0, -3.0)).hasValue());
        static_cast<void>(eq.setBand(0, peak(1200.0, 2.0, 5.0)).ok());
        static_cast<void>(eq.removeBand(1).ok());
        eq.clearBands();

        allocations = scope.count();
    }
    CHECK(allocations == 0);
}
