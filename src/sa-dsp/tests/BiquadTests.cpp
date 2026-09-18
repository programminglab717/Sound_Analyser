#include <sa/core/RealtimeGuard.h>
#include <sa/dsp/Biquad.h>
#include <sa/dsp/BiquadCascade.h>
#include <sa/dsp/Fft.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

BiquadCoefficients designed(Result<BiquadCoefficients> result) {
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

/// Impulse response, taken by running an impulse through the real processing
/// path -- float32 in, float32 out, the same arithmetic audio would see.
std::vector<float> impulseResponse(const BiquadCoefficients& coefficients, int length) {
    Biquad filter{coefficients};
    std::vector<float> response(static_cast<std::size_t>(length), 0.0f);
    response[0] = filter.processSample(1.0f);
    for (int i = 1; i < length; ++i) {
        response[static_cast<std::size_t>(i)] = filter.processSample(0.0f);
    }
    return response;
}

/// Single-frequency DFT of a measured impulse response, accumulated in double.
///
/// This is the measurement half of every response check below: it reports what
/// the filter actually did, with no reference to how its coefficients were
/// derived. One frequency at a time rather than a full FFT, so the test can ask
/// about exactly 1000.0 Hz instead of the nearest bin centre -- which matters,
/// because "-3 dB at the cutoff" is a claim about the cutoff and not about a
/// frequency 0.7 Hz away from it.
std::complex<double> measuredResponse(const std::vector<float>& response, double frequency,
                                      double rate) {
    std::complex<double> sum{0.0, 0.0};
    for (std::size_t i = 0; i < response.size(); ++i) {
        const double angle =
            -2.0 * std::numbers::pi * frequency * static_cast<double>(i) / rate;
        sum += static_cast<double>(response[i]) *
               std::complex<double>{std::cos(angle), std::sin(angle)};
    }
    return sum;
}

/// H(z) on the unit circle, written out from the definition here rather than
/// called from the library, so that the library's own response() is one of the
/// things under test rather than the yardstick.
std::complex<double> transferFunction(const BiquadCoefficients& c, double frequency, double rate) {
    const double angle = -2.0 * std::numbers::pi * frequency / rate;
    const std::complex<double> z1{std::cos(angle), std::sin(angle)};
    const std::complex<double> z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}

double decibels(std::complex<double> value) {
    return 20.0 * std::log10(std::abs(value));
}

/// Decibels at `frequency`, measured through the filter.
double measuredDb(const BiquadCoefficients& coefficients, double frequency, int length = 16384,
                  double rate = kRate) {
    return decibels(measuredResponse(impulseResponse(coefficients, length), frequency, rate));
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

TEST_CASE("Designers reject parameters that would not produce a filter", "[dsp][biquad]") {
    CHECK(BiquadCoefficients::lowPass(kSampleRate48000, 1000.0).hasValue());

    CHECK_FALSE(BiquadCoefficients::lowPass(SampleRate{0.0}, 1000.0).hasValue());
    CHECK_FALSE(BiquadCoefficients::lowPass(kSampleRate48000, 0.0).hasValue());
    CHECK_FALSE(BiquadCoefficients::lowPass(kSampleRate48000, -100.0).hasValue());
    // Nyquist itself, not just beyond it: the cookbook formulas degenerate there.
    CHECK_FALSE(BiquadCoefficients::lowPass(kSampleRate48000, 24000.0).hasValue());
    CHECK_FALSE(BiquadCoefficients::lowPass(kSampleRate48000, 30000.0).hasValue());
    CHECK_FALSE(BiquadCoefficients::lowPass(kSampleRate48000, 1000.0, 0.0).hasValue());
    CHECK_FALSE(BiquadCoefficients::lowPass(kSampleRate48000, 1000.0, -1.0).hasValue());
    CHECK_FALSE(
        BiquadCoefficients::peaking(kSampleRate48000, 1000.0, 1.0,
                                    std::numeric_limits<double>::quiet_NaN())
            .hasValue());

    const auto error = BiquadCoefficients::lowPass(kSampleRate48000, 30000.0);
    REQUIRE_FALSE(error.hasValue());
    CHECK(error.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("The measured response matches the transfer function", "[dsp][biquad]") {
    // The check the whole suite rests on. An impulse goes through the filter,
    // the module's own FFT transforms the result, and the spectrum is compared
    // against H(z) evaluated on the unit circle. The two share no code: one is
    // the difference equation actually running, the other is arithmetic on the
    // coefficients. Agreement means the recursion realises the transfer
    // function the designer intended, which no amount of self-comparison shows.
    const int size = 16384;
    const RealFft fft{size};

    const FilterSpec specs[] = {
        FilterSpec{FilterType::LowPass, 1000.0, kButterworthQ, 0.0},
        FilterSpec{FilterType::HighPass, 200.0, kButterworthQ, 0.0},
        FilterSpec{FilterType::BandPass, 2000.0, 2.0, 0.0},
        FilterSpec{FilterType::Notch, 1000.0, 4.0, 0.0},
        FilterSpec{FilterType::Peaking, 1000.0, 1.0, 6.0},
        FilterSpec{FilterType::Peaking, 4000.0, 3.0, -12.0},
        FilterSpec{FilterType::LowShelf, 300.0, kButterworthQ, 9.0},
        FilterSpec{FilterType::HighShelf, 6000.0, kButterworthQ, -6.0},
        FilterSpec{FilterType::AllPass, 1500.0, 0.5, 0.0},
    };

    for (const FilterSpec& spec : specs) {
        const BiquadCoefficients coefficients =
            designed(BiquadCoefficients::design(kSampleRate48000, spec));
        const std::vector<float> response = impulseResponse(coefficients, size);

        std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(fft.binCount()));
        fft.forward(response.data(), spectrum.data());

        double worst = 0.0;
        int worstBin = 0;
        for (int k = 0; k < fft.binCount(); ++k) {
            const double frequency = kRate * static_cast<double>(k) / static_cast<double>(size);
            const double expected = std::abs(transferFunction(coefficients, frequency, kRate));
            const double actual = static_cast<double>(std::abs(spectrum[static_cast<std::size_t>(k)]));

            // Absolute and relative terms together: the float32 FFT carries a
            // small absolute noise floor that a purely relative tolerance would
            // trip over inside a notch, and a purely absolute one would let a
            // real error hide in a boosted band.
            const double allowed = 2e-4 + 0.002 * expected;
            const double error = std::abs(actual - expected) / allowed;
            if (error > worst) {
                worst = error;
                worstBin = k;
            }
        }

        INFO("filter type " << static_cast<int>(spec.type) << " at " << spec.frequency
                            << " Hz, worst bin " << worstBin << " ("
                            << kRate * static_cast<double>(worstBin) / static_cast<double>(size)
                            << " Hz), error " << worst << " x tolerance");
        CHECK(worst < 1.0);
    }
}

TEST_CASE("The library's own response() agrees with the definition", "[dsp][biquad]") {
    // response() is what the EQ curve overlay is drawn from, so it has to mean
    // the same thing as the measurement above rather than merely look similar.
    const BiquadCoefficients coefficients =
        designed(BiquadCoefficients::peaking(kSampleRate48000, 1000.0, 2.0, 8.0));

    for (double frequency : {20.0, 100.0, 500.0, 1000.0, 2000.0, 8000.0, 20000.0}) {
        const std::complex<double> expected = transferFunction(coefficients, frequency, kRate);
        const std::complex<double> actual = coefficients.response(frequency / kRate);
        INFO("frequency " << frequency);
        CHECK(actual.real() == Approx(expected.real()).margin(1e-12));
        CHECK(actual.imag() == Approx(expected.imag()).margin(1e-12));
        CHECK(coefficients.magnitudeDb(kSampleRate48000, frequency) ==
              Approx(decibels(expected)).margin(1e-9));
    }
}

TEST_CASE("A low-pass is 3 dB down at its cutoff", "[dsp][biquad]") {
    // -3.0103 dB, not "about -3": a Butterworth section's magnitude at its own
    // cutoff is exactly Q, and the bilinear transform RBJ uses is exact at the
    // prewarp frequency. Anything looser would not notice a cutoff off by 5%.
    for (double cutoff : {100.0, 1000.0, 5000.0}) {
        const BiquadCoefficients coefficients =
            designed(BiquadCoefficients::lowPass(kSampleRate48000, cutoff));
        INFO("cutoff " << cutoff);
        CHECK(measuredDb(coefficients, cutoff) == Approx(-3.0103).margin(0.01));
    }

    const BiquadCoefficients highPass =
        designed(BiquadCoefficients::highPass(kSampleRate48000, 1000.0));
    CHECK(measuredDb(highPass, 1000.0) == Approx(-3.0103).margin(0.01));
}

TEST_CASE("A 20 Hz high-pass at 96 kHz is accurate at 20 Hz", "[dsp][biquad]") {
    // The case direct form I with float32 state gets wrong: both poles sit
    // within 0.0013 of z = 1, where coefficient and state rounding move the
    // cutoff and raise the noise floor. A 65536-sample response is long enough
    // for the tail to be negligible at a pole radius of 0.99908.
    const BiquadCoefficients coefficients =
        designed(BiquadCoefficients::highPass(kSampleRate96000, 20.0));
    CHECK(measuredDb(coefficients, 20.0, 65536, 96000.0) == Approx(-3.0103).margin(0.02));
    // Well into the passband it must be flat, not merely present.
    CHECK(measuredDb(coefficients, 1000.0, 65536, 96000.0) == Approx(0.0).margin(0.01));
}

TEST_CASE("A peaking EQ puts its gain where it was asked to", "[dsp][biquad]") {
    const BiquadCoefficients boost =
        designed(BiquadCoefficients::peaking(kSampleRate48000, 1000.0, 2.0, 6.0));

    CHECK(measuredDb(boost, 1000.0) == Approx(6.0).margin(0.01));
    // Far from the centre a peaking filter must be out of the way entirely --
    // this is what separates it from a shelf or a badly normalised design.
    CHECK(measuredDb(boost, 50.0) == Approx(0.0).margin(0.05));
    CHECK(measuredDb(boost, 20000.0) == Approx(0.0).margin(0.05));

    const BiquadCoefficients cut =
        designed(BiquadCoefficients::peaking(kSampleRate48000, 1000.0, 2.0, -6.0));
    CHECK(measuredDb(cut, 1000.0) == Approx(-6.0).margin(0.01));
    CHECK(measuredDb(cut, 50.0) == Approx(0.0).margin(0.05));

    // A cut is the exact inverse of the matching boost: cascade them and the
    // pair is transparent. A designer that got the 1/A factor wrong passes the
    // centre-frequency check above and fails this one.
    for (double frequency : {100.0, 500.0, 1000.0, 3000.0, 15000.0}) {
        INFO("frequency " << frequency);
        CHECK(measuredDb(boost, frequency) + measuredDb(cut, frequency) ==
              Approx(0.0).margin(0.02));
    }
}

TEST_CASE("Shelves reach their target gain in the shelf band", "[dsp][biquad]") {
    const double gainDb = 9.0;
    const BiquadCoefficients low =
        designed(BiquadCoefficients::lowShelf(kSampleRate48000, 1000.0, kButterworthQ, gainDb));

    // At DC the shelf is at its full gain, at Nyquist it is out of the way, and
    // at the corner frequency it is at exactly half the gain in decibels. All
    // three are exact properties of the cookbook shelf, not approximations.
    CHECK(measuredDb(low, 0.0) == Approx(gainDb).margin(0.02));
    CHECK(measuredDb(low, 1000.0) == Approx(gainDb / 2.0).margin(0.02));
    CHECK(measuredDb(low, 24000.0) == Approx(0.0).margin(0.02));

    const BiquadCoefficients high =
        designed(BiquadCoefficients::highShelf(kSampleRate48000, 1000.0, kButterworthQ, gainDb));
    CHECK(measuredDb(high, 24000.0) == Approx(gainDb).margin(0.02));
    CHECK(measuredDb(high, 1000.0) == Approx(gainDb / 2.0).margin(0.02));
    CHECK(measuredDb(high, 0.0) == Approx(0.0).margin(0.02));

    const BiquadCoefficients cut =
        designed(BiquadCoefficients::lowShelf(kSampleRate48000, 1000.0, kButterworthQ, -gainDb));
    CHECK(measuredDb(cut, 0.0) == Approx(-gainDb).margin(0.02));
    CHECK(measuredDb(cut, 24000.0) == Approx(0.0).margin(0.02));
}

TEST_CASE("A notch nulls at its centre", "[dsp][biquad]") {
    const BiquadCoefficients coefficients =
        designed(BiquadCoefficients::notch(kSampleRate48000, 1000.0, 4.0));

    // The numerator has a zero pair exactly on the unit circle at the centre
    // frequency, so the null is arithmetically total; what is left is the
    // float32 rounding of the impulse response being measured.
    CHECK(measuredDb(coefficients, 1000.0) < -80.0);

    // And it must be a notch, not an attenuator: two octaves out it is back.
    CHECK(measuredDb(coefficients, 250.0) == Approx(0.0).margin(0.1));
    CHECK(measuredDb(coefficients, 4000.0) == Approx(0.0).margin(0.1));

    const BiquadCoefficients narrow =
        designed(BiquadCoefficients::notch(kSampleRate48000, 50.0, 30.0));
    CHECK(measuredDb(narrow, 50.0, 65536) < -60.0);
}

TEST_CASE("A band-pass is unity at its centre and falls away", "[dsp][biquad]") {
    const BiquadCoefficients coefficients =
        designed(BiquadCoefficients::bandPass(kSampleRate48000, 1000.0, 1.0));

    // Unity, not Q: this is the constant-peak-gain band-pass, so the band level
    // does not change when the Q control is moved.
    CHECK(measuredDb(coefficients, 1000.0) == Approx(0.0).margin(0.01));
    CHECK(measuredDb(coefficients, 100.0) < -18.0);
    CHECK(measuredDb(coefficients, 10000.0) < -18.0);

    const BiquadCoefficients wide =
        designed(BiquadCoefficients::bandPass(kSampleRate48000, 1000.0, 0.25));
    CHECK(measuredDb(wide, 1000.0) == Approx(0.0).margin(0.01));
}

TEST_CASE("An all-pass is flat and only moves phase", "[dsp][biquad]") {
    const BiquadCoefficients coefficients =
        designed(BiquadCoefficients::allPass(kSampleRate48000, 1000.0, 0.7));
    const std::vector<float> response = impulseResponse(coefficients, 16384);

    for (double frequency : {20.0, 200.0, 1000.0, 5000.0, 20000.0}) {
        INFO("frequency " << frequency);
        CHECK(decibels(measuredResponse(response, frequency, kRate)) == Approx(0.0).margin(0.01));
    }

    // Flat magnitude with no phase shift would be a wire, not an all-pass. At
    // the centre frequency the phase is exactly half a turn.
    const double phase = std::arg(measuredResponse(response, 1000.0, kRate));
    CHECK(std::abs(phase) == Approx(std::numbers::pi).margin(0.01));
}

TEST_CASE("reset() clears the filter state", "[dsp][biquad]") {
    // Without this, the second half of a seek inherits the first half's ringing
    // and a rendered region does not match the same region played back.
    const BiquadCoefficients coefficients =
        designed(BiquadCoefficients::lowPass(kSampleRate48000, 800.0, 4.0));
    const std::vector<float> block = whiteNoise(512, 3);

    Biquad filter{coefficients};
    std::vector<float> first(block.size());
    filter.process(block.data(), first.data(), static_cast<SampleCount>(block.size()));

    // Run something else through it, so the state is definitely not zero.
    std::vector<float> scratch(block.size());
    filter.process(block.data(), scratch.data(), static_cast<SampleCount>(block.size()));

    filter.reset();
    std::vector<float> second(block.size());
    filter.process(block.data(), second.data(), static_cast<SampleCount>(block.size()));

    for (std::size_t i = 0; i < block.size(); ++i) {
        INFO("sample " << i);
        REQUIRE(second[i] == first[i]);
    }
}

TEST_CASE("Changing coefficients does not disturb the state", "[dsp][biquad]") {
    // A parameter change is a change of filter, not a restart: the output has
    // to stay continuous or every automated EQ move is a click.
    const BiquadCoefficients first =
        designed(BiquadCoefficients::lowPass(kSampleRate48000, 500.0));
    const BiquadCoefficients second =
        designed(BiquadCoefficients::lowPass(kSampleRate48000, 520.0));

    Biquad filter{first};
    const std::vector<float> noise = whiteNoise(256, 9);
    for (float sample : noise) {
        static_cast<void>(filter.processSample(sample));
    }

    const float before = filter.processSample(0.5f);
    filter.setCoefficients(second);
    const float after = filter.processSample(0.5f);
    CHECK(std::abs(after - before) < 0.02f);
}

TEST_CASE("Filters stay bounded at extreme settings", "[dsp][biquad]") {
    // Where naive implementations diverge: poles a rounding error away from
    // z = 1 because the cutoff is a ten-thousandth of the sample rate, and
    // poles jammed against the unit circle because the Q is enormous.
    //
    // Two things are asserted. Bounded and finite over seconds of noise rules
    // out a slow divergence. Then silence goes in, and the ringing must have
    // died away after ten time constants -- where the time constant is derived
    // from the pole radius, which for a conjugate pair is sqrt(a2). A filter
    // that rings on past its own poles is not obeying its transfer function.
    struct Case {
        const char* name;
        SampleRate rate;
        FilterSpec spec;
        double bound;
    };

    const Case cases[] = {
        {"20 Hz high-pass at 96 kHz", kSampleRate96000,
         FilterSpec{FilterType::HighPass, 20.0, kButterworthQ, 0.0}, 4.0},
        {"10 Hz high-pass at 192 kHz", SampleRate{192000.0},
         FilterSpec{FilterType::HighPass, 10.0, kButterworthQ, 0.0}, 4.0},
        {"Q 100 band-pass at 30 Hz", kSampleRate96000,
         FilterSpec{FilterType::BandPass, 30.0, 100.0, 0.0}, 40.0},
        {"+24 dB peak at Q 50", kSampleRate96000,
         FilterSpec{FilterType::Peaking, 60.0, 50.0, 24.0}, 40.0},
        {"low-pass just under Nyquist", kSampleRate48000,
         FilterSpec{FilterType::LowPass, 23900.0, 10.0, 0.0}, 20.0},
    };

    for (const Case& test : cases) {
        const Result<BiquadCoefficients> result = BiquadCoefficients::design(test.rate, test.spec);
        REQUIRE(result.hasValue());
        const BiquadCoefficients coefficients = result.value();

        INFO(test.name);
        CHECK(coefficients.isStable());

        Biquad filter{coefficients};
        const auto length = static_cast<std::size_t>(test.rate.hz() * 4.0);
        const std::vector<float> noise = whiteNoise(length, 17);

        double peak = 0.0;
        bool finite = true;
        for (float sample : noise) {
            const float out = filter.processSample(sample);
            finite = finite && std::isfinite(out);
            peak = std::max(peak, static_cast<double>(std::abs(out)));
        }
        CHECK(finite);
        CHECK(peak < test.bound);

        const double poleRadius = std::sqrt(std::abs(coefficients.a2));
        const auto decaySamples =
            static_cast<std::size_t>(10.0 / -std::log(poleRadius)) + 1024;

        double tail = 0.0;
        for (std::size_t i = 0; i < decaySamples; ++i) {
            const float out = filter.processSample(0.0f);
            finite = finite && std::isfinite(out);
            // Only the last stretch is the tail; before that the filter is
            // still legitimately ringing down.
            if (i + 512 >= decaySamples) {
                tail = std::max(tail, static_cast<double>(std::abs(out)));
            }
        }
        CHECK(finite);
        CHECK(tail < peak * 1e-3);
    }
}

TEST_CASE("A cascade is the product of its sections", "[dsp][biquad][cascade]") {
    BiquadCascade cascade;
    const BiquadCoefficients first =
        designed(BiquadCoefficients::peaking(kSampleRate48000, 200.0, 1.0, 6.0));
    const BiquadCoefficients second =
        designed(BiquadCoefficients::peaking(kSampleRate48000, 3000.0, 2.0, -9.0));
    const BiquadCoefficients third =
        designed(BiquadCoefficients::highPass(kSampleRate48000, 40.0));

    REQUIRE(cascade.append(first).ok());
    REQUIRE(cascade.append(second).ok());
    REQUIRE(cascade.append(third).ok());
    REQUIRE(cascade.sectionCount() == 3);
    CHECK(cascade.isStable());

    // Measure the chain end to end and compare against the product of the three
    // transfer functions worked out independently.
    std::vector<float> response(16384, 0.0f);
    response[0] = cascade.processSample(1.0f);
    for (std::size_t i = 1; i < response.size(); ++i) {
        response[i] = cascade.processSample(0.0f);
    }

    for (double frequency : {20.0, 100.0, 200.0, 1000.0, 3000.0, 12000.0, 20000.0}) {
        const std::complex<double> expected = transferFunction(first, frequency, kRate) *
                                              transferFunction(second, frequency, kRate) *
                                              transferFunction(third, frequency, kRate);
        INFO("frequency " << frequency);
        CHECK(decibels(measuredResponse(response, frequency, kRate)) ==
              Approx(decibels(expected)).margin(0.02));
        CHECK(cascade.magnitudeDb(kSampleRate48000, frequency) ==
              Approx(decibels(expected)).margin(1e-9));
    }
}

TEST_CASE("A Butterworth cascade matches the Butterworth magnitude response",
          "[dsp][biquad][cascade]") {
    // The reference is the textbook Butterworth magnitude,
    //
    //     |H| = 1 / sqrt(1 + (W/W0)^(2N)),
    //
    // with W = tan(pi f / fs) -- the bilinear transform's frequency warping,
    // which is exactly what the cookbook designs prewarp for. Nothing in that
    // formula comes from this module, and it is sharp enough to catch the usual
    // mistake of cascading identical 1/sqrt(2) sections: that filter reads
    // -3 dB per section at the cutoff instead of -3 dB overall.
    const auto warped = [](double frequency) { return std::tan(std::numbers::pi * frequency / kRate); };

    const double cutoff = 1000.0;
    for (int order : {2, 4, 6, 8}) {
        const Result<BiquadCascade> result =
            BiquadCascade::butterworth(FilterType::LowPass, order, kSampleRate48000, cutoff);
        REQUIRE(result.hasValue());
        REQUIRE(result.value().sectionCount() == order / 2);

        BiquadCascade cascade = result.value();
        std::vector<float> response(16384, 0.0f);
        response[0] = cascade.processSample(1.0f);
        for (std::size_t i = 1; i < response.size(); ++i) {
            response[i] = cascade.processSample(0.0f);
        }

        for (double frequency : {100.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0}) {
            const double ratio = warped(frequency) / warped(cutoff);
            const double expected =
                -10.0 * std::log10(1.0 + std::pow(ratio, 2.0 * static_cast<double>(order)));
            INFO("order " << order << " at " << frequency << " Hz");
            CHECK(decibels(measuredResponse(response, frequency, kRate)) ==
                  Approx(expected).margin(0.05));
        }
    }

    CHECK_FALSE(BiquadCascade::butterworth(FilterType::LowPass, 3, kSampleRate48000, 1000.0)
                    .hasValue());
    CHECK_FALSE(BiquadCascade::butterworth(FilterType::LowPass, 0, kSampleRate48000, 1000.0)
                    .hasValue());
    CHECK_FALSE(BiquadCascade::butterworth(FilterType::Peaking, 4, kSampleRate48000, 1000.0)
                    .hasValue());
    CHECK_FALSE(BiquadCascade::butterworth(FilterType::LowPass, 40, kSampleRate48000, 1000.0)
                    .hasValue());
}

TEST_CASE("A cascade refuses to grow past its capacity", "[dsp][biquad][cascade]") {
    BiquadCascade cascade;
    const BiquadCoefficients section =
        designed(BiquadCoefficients::lowPass(kSampleRate48000, 1000.0));

    for (int i = 0; i < BiquadCascade::kMaxSections; ++i) {
        INFO("section " << i);
        REQUIRE(cascade.append(section).ok());
    }
    const Status overflow = cascade.append(section);
    CHECK_FALSE(overflow.ok());
    CHECK(overflow.error().code() == ErrorCode::OutOfRange);
    CHECK(cascade.sectionCount() == BiquadCascade::kMaxSections);

    CHECK(cascade.sectionCoefficients(0) != nullptr);
    CHECK(cascade.sectionCoefficients(-1) == nullptr);
    CHECK(cascade.sectionCoefficients(BiquadCascade::kMaxSections) == nullptr);
    CHECK_FALSE(cascade.setSection(-1, section).ok());
    CHECK_FALSE(cascade.setSection(BiquadCascade::kMaxSections, section).ok());

    cascade.clear();
    CHECK(cascade.isEmpty());
    // An empty cascade is a wire, not a mute.
    CHECK(cascade.processSample(0.25f) == 0.25f);
}

TEST_CASE("Cascade reset clears every section", "[dsp][biquad][cascade]") {
    const Result<BiquadCascade> result =
        BiquadCascade::butterworth(FilterType::HighPass, 6, kSampleRate48000, 120.0);
    REQUIRE(result.hasValue());

    BiquadCascade cascade = result.value();
    const std::vector<float> block = whiteNoise(512, 31);
    std::vector<float> first(block.size());
    std::vector<float> second(block.size());

    cascade.process(block.data(), first.data(), static_cast<SampleCount>(block.size()));
    cascade.process(block.data(), second.data(), static_cast<SampleCount>(block.size()));
    cascade.reset();
    cascade.process(block.data(), second.data(), static_cast<SampleCount>(block.size()));

    for (std::size_t i = 0; i < block.size(); ++i) {
        INFO("sample " << i);
        REQUIRE(second[i] == first[i]);
    }
}

TEST_CASE("Degenerate blocks are no-ops", "[dsp][biquad]") {
    Biquad filter{designed(BiquadCoefficients::lowPass(kSampleRate48000, 1000.0))};
    float sentinel = 1.0f;
    filter.process(&sentinel, &sentinel, 0);
    filter.process(&sentinel, &sentinel, -16);
    CHECK(sentinel == 1.0f);

    BiquadCascade cascade;
    REQUIRE(cascade.append(filter.coefficients()).ok());
    cascade.process(&sentinel, &sentinel, 0);
    CHECK(sentinel == 1.0f);
}

TEST_CASE("In-place processing matches out-of-place", "[dsp][biquad]") {
    const BiquadCoefficients coefficients =
        designed(BiquadCoefficients::peaking(kSampleRate48000, 900.0, 1.5, 4.0));
    const std::vector<float> block = whiteNoise(256, 77);

    std::vector<float> separate(block.size());
    Biquad{coefficients}.process(block.data(), separate.data(),
                                 static_cast<SampleCount>(block.size()));

    std::vector<float> inPlace = block;
    Biquad filter{coefficients};
    filter.processInPlace(inPlace.data(), static_cast<SampleCount>(inPlace.size()));

    for (std::size_t i = 0; i < block.size(); ++i) {
        REQUIRE(inPlace[i] == separate[i]);
    }
}

TEST_CASE("Filtering on the audio thread allocates nothing", "[dsp][biquad][rt]") {
    if (!rt::checksEnabled()) {
        SUCCEED("SA_RT_SAFETY_CHECKS is off in this build");
        return;
    }

    // Everything that allocates happens here, off the audio thread, as real
    // code must: designing, building the cascade, sizing the buffers.
    const BiquadCoefficients coefficients =
        designed(BiquadCoefficients::lowShelf(kSampleRate48000, 120.0, kButterworthQ, 3.0));
    const Result<BiquadCascade> built =
        BiquadCascade::butterworth(FilterType::HighPass, 8, kSampleRate48000, 30.0);
    REQUIRE(built.hasValue());

    Biquad filter{coefficients};
    BiquadCascade cascade = built.value();
    std::vector<float> block = whiteNoise(512, 5);
    std::vector<float> output(block.size());

    std::size_t allocations = 0;
    {
        const rt::ScopedAudioThread guard;
        const rt::AllocationScope scope;

        const auto count = static_cast<SampleCount>(block.size());
        filter.process(block.data(), output.data(), count);
        cascade.process(output.data(), output.data(), count);
        filter.processInPlace(output.data(), count);
        filter.reset();
        cascade.reset();
        static_cast<void>(filter.processSample(0.1f));

        allocations = scope.count();
    }
    CHECK(allocations == 0);
}
