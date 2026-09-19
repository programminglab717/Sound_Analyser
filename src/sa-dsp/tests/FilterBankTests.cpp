#include <sa/core/AudioBuffer.h>
#include <sa/dsp/Biquad.h>
#include <sa/dsp/BiquadCascade.h>
#include <sa/dsp/FilterBank.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <iterator>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

/// Where the 1 kHz band sits in the default thirty-one band layout, which
/// starts at band index -17.
constexpr std::size_t kKiloHertzBand = 17;

FilterBank created(Result<FilterBank> result) {
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

std::vector<FilterBand> layoutOf(const FilterBankSettings& settings) {
    Result<std::vector<FilterBand>> result = bandLayout(settings);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

/// Magnitude at `frequency`, measured by driving the cascade with a sine and
/// correlating the input and the output over the same settled window.
///
/// A ratio of two single-frequency DFTs rather than a ratio of RMS values. It
/// ignores everything except the drive frequency, and the leakage from a
/// window holding a fractional number of cycles divides out because both
/// halves of the ratio carry exactly the same amount of it. The cascade
/// arrives by value, so measuring a band does not disturb the bank it came
/// from, and the audio path is the real one: float32 in, float32 out.
///
/// `cycles` of drive are discarded before the window opens. The slowest thing
/// in the bank is a third-octave band, whose slowest section has a pole about
/// 0.33 f/fs inside the unit circle and so a time constant near three cycles
/// of its own centre frequency; forty cycles is a dozen of those.
double measuredDb(BiquadCascade cascade, double frequency, double rate, int cycles = 40) {
    const auto span = static_cast<std::size_t>(
        std::max(4096.0, std::ceil(static_cast<double>(cycles) * rate / frequency)));
    const double step = 2.0 * std::numbers::pi * frequency / rate;

    cascade.reset();
    std::complex<double> driven{0.0, 0.0};
    std::complex<double> filtered{0.0, 0.0};
    for (std::size_t i = 0; i < 2 * span; ++i) {
        const double phase = step * static_cast<double>(i);
        const auto input = static_cast<float>(std::sin(phase));
        const float output = cascade.processSample(input);
        if (i >= span) {
            const std::complex<double> phasor{std::cos(-phase), std::sin(-phase)};
            driven += static_cast<double>(input) * phasor;
            filtered += static_cast<double>(output) * phasor;
        }
    }
    return 20.0 * std::log10(std::abs(filtered) / std::abs(driven));
}

/// Magnitude of an order-`order` Butterworth band-pass, written out from the
/// definition here rather than asked of the library.
///
///     |H(jW)|^2 = 1 / (1 + ((W^2 - w0^2) / (B W))^order)
///
/// with W the pre-warped frequency tan(pi f / fs), w0^2 the product of the
/// pre-warped edges and B their difference. The bilinear transform is an exact
/// change of variable, so this is not an approximation to what the digital
/// filter does -- it is what the digital filter does, given only the order and
/// the two edges. `frequency` must be below Nyquist, where tan() is finite.
double butterworthDb(int order, double lowHz, double highHz, double frequency, double rate) {
    const double lower = std::tan(std::numbers::pi * lowHz / rate);
    const double upper = std::tan(std::numbers::pi * highHz / rate);
    const double here = std::tan(std::numbers::pi * frequency / rate);
    const double detuning = (here * here - lower * upper) / ((upper - lower) * here);
    // Squared first, so the even power is taken of a positive number and the
    // sign of the detuning never reaches std::pow.
    return -10.0 * std::log10(1.0 + std::pow(detuning * detuning, order / 2));
}

/// The band-pass the reverberation-per-band measurement has been built from:
/// two identical cookbook band-pass sections at the band centre, with the Q
/// the band's own edges imply. Rebuilt here rather than called, because
/// sa-analysis sits above this library and cannot be linked from its tests.
BiquadCascade twoSectionBandPass(SampleRate rate, const FilterBand& band) {
    const double q = band.centreHz / std::max(1.0, band.highHz - band.lowHz);
    const Result<BiquadCoefficients> section = BiquadCoefficients::bandPass(rate, band.centreHz, q);
    REQUIRE(section.hasValue());
    BiquadCascade cascade;
    REQUIRE(cascade.append(section.value()).ok());
    REQUIRE(cascade.append(section.value()).ok());
    return cascade;
}

/// Largest pole magnitude in a cascade, from the roots of z^2 + a1 z + a2.
///
/// Solved rather than inferred, and not the library's own stability predicate,
/// so that "the poles are inside the unit circle" is checked against the
/// definition of a pole instead of against the shortcut for it.
double maxPoleRadius(const BiquadCascade& cascade) {
    double worst = 0.0;
    for (int i = 0; i < cascade.sectionCount(); ++i) {
        const BiquadCoefficients* section = cascade.sectionCoefficients(i);
        REQUIRE(section != nullptr);
        const double discriminant = section->a1 * section->a1 - 4.0 * section->a2;
        if (discriminant < 0.0) {
            // A conjugate pair. Their product is a2, so each has radius
            // sqrt(a2).
            worst = std::max(worst, std::sqrt(std::abs(section->a2)));
        } else {
            const double root = std::sqrt(discriminant);
            worst = std::max(worst, std::abs((-section->a1 + root) * 0.5));
            worst = std::max(worst, std::abs((-section->a1 - root) * 0.5));
        }
    }
    return worst;
}

std::vector<float> whiteNoise(std::size_t count, unsigned seed) {
    std::mt19937 engine{seed};
    std::uniform_real_distribution<float> distribution{-0.25f, 0.25f};
    std::vector<float> samples(count);
    for (float& sample : samples) {
        sample = distribution(engine);
    }
    return samples;
}

const std::vector<double>& supportedRates() {
    // Everything a caller is likely to hand over, from telephone bandwidth to
    // the top of what SampleRate::isValid accepts.
    static const std::vector<double> rates{8000.0,   11025.0,  16000.0, 22050.0, 32000.0,
                                           44100.0,  48000.0,  88200.0, 96000.0, 176400.0,
                                           192000.0, 384000.0, 768000.0};
    return rates;
}

} // namespace

TEST_CASE("Centres follow the base-ten definition", "[dsp][filterbank]") {
    // 1000 Hz times 10^(n/10) for third-octaves and 10^(3n/10) for octaves.
    // Written out again here so the check is against the definition rather
    // than against the same expression the library evaluates.
    for (int index = -25; index <= 15; ++index) {
        const double third = 1000.0 * std::pow(10.0, static_cast<double>(index) / 10.0);
        const double octave = 1000.0 * std::pow(10.0, 3.0 * static_cast<double>(index) / 10.0);
        REQUIRE(bandCentreHz(BandSpacing::ThirdOctave, index) == Approx(third).epsilon(1e-12));
        REQUIRE(bandCentreHz(BandSpacing::Octave, index) == Approx(octave).epsilon(1e-12));
    }

    // Index zero is 1 kHz under both spacings, and every third third-octave is
    // an octave band -- that is the same series sampled three times as coarsely.
    REQUIRE(bandCentreHz(BandSpacing::ThirdOctave, 0) == Approx(1000.0));
    REQUIRE(bandCentreHz(BandSpacing::Octave, 0) == Approx(1000.0));
    for (int index = -5; index <= 4; ++index) {
        REQUIRE(bandCentreHz(BandSpacing::Octave, index) ==
                Approx(bandCentreHz(BandSpacing::ThirdOctave, 3 * index)).epsilon(1e-12));
    }

    // And the exact centres are the numbers everybody writes on an axis, to
    // within the rounding in those numbers. The worst case in the whole series
    // is the pair called 160 Hz and 1.6 kHz, centred at 158.489 and 1584.89,
    // which is 0.944% below their names.
    const std::vector<double> nominal{
        20.0,   25.0,   31.5,   40.0,   50.0,   63.0,    80.0,    100.0,   125.0,  160.0,  200.0,
        250.0,  315.0,  400.0,  500.0,  630.0,  800.0,   1000.0,  1250.0,  1600.0, 2000.0, 2500.0,
        3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0, 20000.0};
    double worstError = 0.0;
    for (std::size_t i = 0; i < nominal.size(); ++i) {
        const double exact = bandCentreHz(BandSpacing::ThirdOctave, static_cast<int>(i) - 17);
        worstError = std::max(worstError, std::abs(exact / nominal[i] - 1.0));
    }
    INFO("worst gap between a printed centre and the exact one: " << worstError);
    REQUIRE(worstError < 0.0095);
    REQUIRE(worstError > 0.0094); // 0.944%, and kBandNameTolerance is 2%
}

TEST_CASE("The audible layout is thirty-one third-octaves and ten octaves", "[dsp][filterbank]") {
    // 10 log10(20 / 1000) = -16.99 and 10 log10(20000 / 1000) = +13.01, so the
    // third-octave bands whose centres are named by 20 Hz and 20 kHz are
    // indices -17 and 13: thirty-one of them, centred at 19.9526 and 19952.6.
    const std::vector<FilterBand> thirds = layoutOf({});
    REQUIRE(thirds.size() == 31);
    REQUIRE(thirds.front().index == -17);
    REQUIRE(thirds.back().index == 13);
    REQUIRE(thirds.front().centreHz == Approx(19.952623));
    REQUIRE(thirds.back().centreHz == Approx(19952.623));

    // For octaves the same two limits fall at -5.66 and +4.34, and only whole
    // indices exist, so the series runs from the band called 31.5 Hz to the one
    // called 16 kHz: ten of them. The band below, centred at 15.85 Hz, is more
    // than 2% away from 20 Hz and so is not the band that limit names.
    FilterBankSettings octaves;
    octaves.spacing = BandSpacing::Octave;
    const std::vector<FilterBand> wide = layoutOf(octaves);
    REQUIRE(wide.size() == 10);
    REQUIRE(wide.front().index == -5);
    REQUIRE(wide.back().index == 4);
    REQUIRE(wide.front().centreHz == Approx(31.6228));
    REQUIRE(wide.back().centreHz == Approx(15848.932));

    // Thirty-one and ten are also what the FFT-integrated band display reports
    // at 48 kHz, which is the point: the two ways of measuring a band have to
    // be talking about the same bands.
    const FilterBank bank = created(FilterBank::create(SampleRate{kRate}));
    REQUIRE(bank.bandCount() == 31);
    REQUIRE(bank.unavailable().empty());
    const FilterBank octaveBank = created(FilterBank::create(SampleRate{kRate}, octaves));
    REQUIRE(octaveBank.bandCount() == 10);
    REQUIRE(octaveBank.unavailable().empty());
}

TEST_CASE("Band edges are the octave factors around the centre", "[dsp][filterbank]") {
    for (const FilterBand& band : layoutOf({})) {
        REQUIRE(band.lowHz == Approx(band.centreHz * std::exp2(-1.0 / 6.0)).epsilon(1e-12));
        REQUIRE(band.highHz == Approx(band.centreHz * std::exp2(1.0 / 6.0)).epsilon(1e-12));
        // A third of an octave wide, and the centre is the geometric mean of
        // the edges rather than the arithmetic one.
        REQUIRE(band.highHz / band.lowHz == Approx(std::exp2(1.0 / 3.0)).epsilon(1e-12));
        REQUIRE(std::sqrt(band.lowHz * band.highHz) == Approx(band.centreHz).epsilon(1e-12));
    }

    FilterBankSettings octaves;
    octaves.spacing = BandSpacing::Octave;
    for (const FilterBand& band : layoutOf(octaves)) {
        REQUIRE(band.lowHz == Approx(band.centreHz * std::exp2(-0.5)).epsilon(1e-12));
        REQUIRE(band.highHz == Approx(band.centreHz * std::exp2(0.5)).epsilon(1e-12));
        REQUIRE(band.highHz / band.lowHz == Approx(2.0).epsilon(1e-12));
    }
}

TEST_CASE("A band is the order it was asked for", "[dsp][filterbank]") {
    for (const int order : {2, 4, 6, 12, 32}) {
        FilterBankSettings settings;
        settings.order = order;
        const FilterBank bank = created(FilterBank::create(SampleRate{kRate}, settings));
        REQUIRE(bank.bandCount() == 31);
        for (int i = 0; i < bank.bandCount(); ++i) {
            // One biquad per two poles, which is what "order" counts.
            REQUIRE(bank.filter(i)->sectionCount() == order / 2);
        }
    }
}

TEST_CASE("A sine at a band centre passes at unity", "[dsp][filterbank]") {
    // The peak of a band does not sit exactly on the nominal centre, because
    // the design pre-warps the edges: the peak lands where tan(pi f / fs) is
    // the geometric mean of the edges' tangents, which is a little above the
    // geometric mean of the edges themselves. A Butterworth is maximally flat
    // there, though, so the loss that displacement costs at the nominal centre
    // is tiny and shrinks fast with order. Asserted three ways rather than
    // one: the measurement is close to unity, it is close to what the order
    // and the edges predict, and that prediction is itself close to unity --
    // so a pass says the filter is the design and the design is flat, rather
    // than leaving open which of the two a loose number was hiding.
    //
    // 0.05 dB on the measurement leaves room for the residue of the settling
    // transient still inside the analysis window. 0.02 dB on the design is the
    // worst case over the audible range at these rates: the octave band
    // centred at 15848.9 Hz at 48 kHz, whose upper edge is the closest any
    // band's gets to Nyquist here and so the most stretched by pre-warping.
    constexpr double kMeasured = 0.05;
    constexpr double kDesigned = 0.02;

    for (const double rate : {44100.0, kRate}) {
        for (const BandSpacing spacing : {BandSpacing::ThirdOctave, BandSpacing::Octave}) {
            FilterBankSettings settings;
            settings.spacing = spacing;
            const FilterBank bank = created(FilterBank::create(SampleRate{rate}, settings));
            REQUIRE(bank.bandCount() > 0);

            for (int i = 0; i < bank.bandCount(); ++i) {
                const FilterBand& band = bank.bands()[static_cast<std::size_t>(i)];
                const double gain = measuredDb(*bank.filter(i), band.centreHz, rate);
                const double predicted =
                    butterworthDb(settings.order, band.lowHz, band.highHz, band.centreHz, rate);
                INFO("rate " << rate << " band " << band.index << " at " << band.centreHz
                             << " Hz measured " << gain << " dB against a predicted " << predicted);
                REQUIRE(std::abs(gain) < kMeasured);
                REQUIRE(std::abs(predicted) < kDesigned);
                REQUIRE(gain == Approx(predicted).margin(0.01));
            }
        }
    }
}

TEST_CASE("A band is three decibels down at its edges", "[dsp][filterbank]") {
    // By construction: the detuning term of the Butterworth band-pass is
    // exactly -1 at the lower edge and +1 at the upper one, so the magnitude
    // there is 1/sqrt(2) whatever the order, and 20 log10(1/sqrt(2)) is
    // -3.0103 dB. The edges are pre-warped before the bilinear transform
    // precisely so that this lands on the band edge rather than near it.
    const double expected = 20.0 * std::log10(1.0 / std::sqrt(2.0));
    const FilterBank bank = created(FilterBank::create(SampleRate{kRate}));

    for (int i = 0; i < bank.bandCount(); ++i) {
        const FilterBand& band = bank.bands()[static_cast<std::size_t>(i)];
        INFO("band " << band.index << " edges " << band.lowHz << " .. " << band.highHz);
        REQUIRE(bank.filter(i)->magnitudeDb(SampleRate{kRate}, band.lowHz) ==
                Approx(expected).margin(1e-6));
        REQUIRE(bank.filter(i)->magnitudeDb(SampleRate{kRate}, band.highHz) ==
                Approx(expected).margin(1e-6));
    }

    // And the same thing measured through the audio path, for one band, so
    // that the coefficients and the running filter are known to agree.
    const auto kilohertz = static_cast<int>(kKiloHertzBand);
    const FilterBand& band = bank.bands()[kKiloHertzBand];
    REQUIRE(band.centreHz == Approx(1000.0));
    REQUIRE(measuredDb(*bank.filter(kilohertz), band.lowHz, kRate) ==
            Approx(expected).margin(0.05));
    REQUIRE(measuredDb(*bank.filter(kilohertz), band.highHz, kRate) ==
            Approx(expected).margin(0.05));
}

TEST_CASE("An octave band rejects a tone an octave away by what the order implies",
          "[dsp][filterbank]") {
    // An octave band has w0/B = 1 / (sqrt(2) - 1/sqrt(2)) = sqrt(2). A tone an
    // octave from the centre sits at W/w0 = 2 or 1/2, so the detuning term
    // (W/w0 - w0/W) * w0/B is +/- 1.5 * sqrt(2) = 2.12132, and the attenuation
    // an order-`order` Butterworth band-pass gives there is
    //
    //     10 log10(1 + 2.12132^order)
    //
    // which for order 6 is 10 log10(1 + 91.125) = 19.6438 dB. That is the
    // figure the order implies; nothing here is read off a measurement.
    constexpr int kOrder = 6;
    const double detuning = 1.5 * std::sqrt(2.0);
    const double implied = 10.0 * std::log10(1.0 + std::pow(detuning, kOrder));
    REQUIRE(implied == Approx(19.643775).margin(1e-5));

    FilterBankSettings settings;
    settings.spacing = BandSpacing::Octave;
    settings.order = kOrder;
    const FilterBank bank = created(FilterBank::create(SampleRate{kRate}, settings));
    const double nyquist = kRate * 0.5;

    for (int i = 0; i < bank.bandCount(); ++i) {
        const FilterBand& band = bank.bands()[static_cast<std::size_t>(i)];
        std::vector<double> probes{band.centreHz * 0.5};
        if (band.centreHz * 2.0 < nyquist) {
            probes.push_back(band.centreHz * 2.0);
        }

        for (const double probe : probes) {
            const double measured = measuredDb(*bank.filter(i), probe, kRate);
            const double exact = butterworthDb(kOrder, band.lowHz, band.highHz, probe, kRate);
            INFO("band " << band.index << " at " << band.centreHz << " Hz, probe " << probe
                         << " Hz: measured " << measured << " dB, predicted " << exact << " dB");

            // The filter is what the order and the edges say it is, to a
            // twentieth of a decibel.
            REQUIRE(measured == Approx(exact).margin(0.05));

            // For the bands well clear of Nyquist, that prediction is also the
            // unwarped figure above to within a tenth of a decibel -- asserted
            // rather than assumed, so the allowance below is not a number
            // picked to make the line pass. Above 1 kHz at this rate the
            // pre-warping stretches the top of the band enough to matter and
            // the lower skirt is measurably shallower; the bank's top octave
            // band manages only 13.7 dB, which is worth knowing and is not a
            // failure of the order.
            if (band.centreHz <= 1000.0) {
                REQUIRE(std::abs(-exact - implied) < 0.1);
                REQUIRE(-measured > implied - 0.15);
            }
        }
    }
}

TEST_CASE("The skirt is steeper than the two-section band-pass by the margin the order implies",
          "[dsp][filterbank]") {
    // The point of the exercise. An order-2N Butterworth band-pass has N zeros
    // at the origin against 2N poles, so far from the band its magnitude goes
    // as W^-N: 6N dB per octave, which is 18 for the default order of 6. Two
    // identical cookbook band-pass sections have one zero and two poles each,
    // so they go as W^-2: 12 dB per octave. The margin the two orders imply is
    // therefore 6 dB per octave, and the ratio 1.5.
    constexpr int kOrder = 6;
    const double impliedNew = 6.0 * static_cast<double>(kOrder / 2);
    const double impliedOld = 12.0;
    REQUIRE(impliedNew == 18.0);

    FilterBankSettings settings;
    settings.spacing = BandSpacing::Octave;
    settings.order = kOrder;
    const FilterBank bank = created(FilterBank::create(SampleRate{kRate}, settings));

    // The 125 Hz octave band, so that sixteen times its centre is still only a
    // twelfth of Nyquist and neither filter's roll-off is being helped by the
    // zero every bilinear design has at Nyquist.
    const int index = 2;
    const FilterBand& band = bank.bands()[static_cast<std::size_t>(index)];
    REQUIRE(band.index == -3);
    REQUIRE(band.centreHz == Approx(125.8925));

    const BiquadCascade& fresh = *bank.filter(index);
    const BiquadCascade old = twoSectionBandPass(SampleRate{kRate}, band);

    const double closer = band.centreHz * 8.0;
    const double further = band.centreHz * 16.0;
    const double freshSlope = fresh.magnitudeDb(SampleRate{kRate}, closer) -
                              fresh.magnitudeDb(SampleRate{kRate}, further);
    const double oldSlope =
        old.magnitudeDb(SampleRate{kRate}, closer) - old.magnitudeDb(SampleRate{kRate}, further);
    INFO("slope between " << closer << " and " << further << " Hz: new " << freshSlope
                          << " dB/octave, two-section " << oldSlope << " dB/octave");

    // Each filter is at least as steep as its own asymptote -- both are still
    // approaching it from above at this distance out.
    REQUIRE(freshSlope > impliedNew);
    REQUIRE(oldSlope > impliedOld);
    // Neither has overshot it by much, which is what makes the difference
    // below a comparison of the two orders and not of two arbitrary numbers.
    REQUIRE(freshSlope < impliedNew + 1.0);
    REQUIRE(oldSlope < impliedOld + 1.0);
    // And the margin is the one the orders imply, less a fifth of a decibel
    // for the two filters sitting different distances above their asymptotes.
    REQUIRE(freshSlope - oldSlope > impliedNew - impliedOld - 0.2);
    REQUIRE(freshSlope / oldSlope > 1.45);

    // The same comparison where a measurement through the audio path is still
    // well clear of the noise floor: one octave above the centre, where the
    // order-6 band-pass gives 10 log10(1 + 2.12132^6) = 19.644 dB and two
    // cookbook sections give 20 log10(1 + 2.12132^2) = 14.807 dB, a difference
    // of 4.837 dB.
    const double probe = band.centreHz * 2.0;
    const double detuning = 1.5 * std::sqrt(2.0);
    const double impliedGap = 10.0 * std::log10(1.0 + std::pow(detuning, kOrder)) -
                              20.0 * std::log10(1.0 + detuning * detuning);
    REQUIRE(impliedGap == Approx(4.836521).margin(1e-5));

    const double freshDb = measuredDb(fresh, probe, kRate);
    const double oldDb = measuredDb(old, probe, kRate);
    INFO("one octave above " << band.centreHz << " Hz: new " << freshDb << " dB, two-section "
                             << oldDb << " dB");
    REQUIRE(oldDb - freshDb > impliedGap - 0.1);
}

TEST_CASE("Bands summed across the bank recover the energy that went in", "[dsp][filterbank]") {
    // White noise into every band at once, band energies added up, against the
    // energy of the noise itself. Two things decide what comes back, and both
    // are arithmetic rather than measurement.
    //
    // A band collects a little more than its nominal width. The integral of an
    // order-2N Butterworth magnitude squared across its own band is
    // (pi/2N) / sin(pi/2N) times the distance between its -3 dB points -- the
    // energy the skirts let through outside the band is more than the rounded
    // shoulders lose inside it. For order 6 that factor is 1.047198, which is
    // +0.2003 dB.
    //
    // And the bank stops. The top third-octave band's upper edge is 22396.1 Hz,
    // so 6.68% of the 24 kHz below Nyquist has no band over it at all: that is
    // -0.3004 dB.
    //
    // Net expectation: 10 log10(1.047198 * 22396.1 / 24000) = -0.1001 dB.
    //
    // It is not exact, and the residue is the reason for the tolerance. The
    // two factors above are the analog ones, while the realised bands are
    // pre-warped, which widens the top ones; adjacent bands overlap by 0.08%
    // where base-ten centres meet base-two edges; and each band's skirt reaches
    // a little way under its neighbours, which is counted twice.
    constexpr int kOrder = 6;
    FilterBank bank = created(FilterBank::create(SampleRate{kRate}));
    REQUIRE(bank.bandCount() == 31);

    const double noiseBandwidthFactor =
        (std::numbers::pi / kOrder) / std::sin(std::numbers::pi / kOrder);
    REQUIRE(noiseBandwidthFactor == Approx(1.047198).margin(1e-6));
    const double covered = bank.bands().back().highHz / (kRate * 0.5);
    REQUIRE(covered == Approx(0.9331693).margin(1e-6));
    const double expectedDb = 10.0 * std::log10(noiseBandwidthFactor * covered);
    REQUIRE(expectedDb == Approx(-0.100110).margin(1e-5));

    // Five and a half seconds, of which the first quarter is discarded on both
    // sides of the comparison: the 20 Hz band needs about a second to settle,
    // and its own energy would otherwise be counted short.
    const std::size_t count = 1u << 18u;
    const std::size_t skip = count / 4;
    const std::vector<float> noise = whiteNoise(count, 20250919u);

    double input = 0.0;
    for (std::size_t i = skip; i < count; ++i) {
        input += static_cast<double>(noise[i]) * static_cast<double>(noise[i]);
    }

    double output = 0.0;
    for (int i = 0; i < bank.bandCount(); ++i) {
        BiquadCascade* cascade = bank.filter(i);
        REQUIRE(cascade != nullptr);
        cascade->reset();
        for (std::size_t sample = 0; sample < count; ++sample) {
            const double filtered = static_cast<double>(cascade->processSample(noise[sample]));
            if (sample >= skip) {
                output += filtered * filtered;
            }
        }
    }

    const double recoveredDb = 10.0 * std::log10(output / input);
    INFO("recovered " << recoveredDb << " dB against an expected " << expectedDb << " dB");
    REQUIRE(recoveredDb == Approx(expectedDb).margin(0.05));
}

TEST_CASE("Bands that reach Nyquist are reported unavailable", "[dsp][filterbank]") {
    struct Expectation {
        double rate;
        BandSpacing spacing;
        std::size_t realised;
        std::size_t unavailable;
    };

    // At 8 kHz, Nyquist is 4000 and the highest third-octave band whose upper
    // edge fits is the one called 3150 Hz (index 5, upper edge 3549.5), so
    // twenty-three of thirty-one survive; for octaves it is the band called
    // 2 kHz (index 1, upper edge 2821.7), so seven of ten.
    //
    // At 44.1 kHz, Nyquist is 22050 and exactly one band of each set is lost:
    // the 20 kHz third-octave, whose upper edge is 22396.1, and the 16 kHz
    // octave, whose upper edge is 22413.8.
    const std::vector<Expectation> expectations{
        {8000.0, BandSpacing::ThirdOctave, 23, 8},   {8000.0, BandSpacing::Octave, 7, 3},
        {44100.0, BandSpacing::ThirdOctave, 30, 1},  {44100.0, BandSpacing::Octave, 9, 1},
        {48000.0, BandSpacing::ThirdOctave, 31, 0},  {48000.0, BandSpacing::Octave, 10, 0},
        {192000.0, BandSpacing::ThirdOctave, 31, 0}, {192000.0, BandSpacing::Octave, 10, 0},
    };

    for (const Expectation& expected : expectations) {
        FilterBankSettings settings;
        settings.spacing = expected.spacing;
        const FilterBank bank = created(FilterBank::create(SampleRate{expected.rate}, settings));
        INFO("rate " << expected.rate);
        REQUIRE(bank.bands().size() == expected.realised);
        REQUIRE(bank.unavailable().size() == expected.unavailable);
        for (const UnavailableBand& missing : bank.unavailable()) {
            REQUIRE(missing.reason == BandUnavailability::AboveNyquist);
            REQUIRE(missing.band.highHz >= expected.rate * 0.5);
        }
        for (const FilterBand& band : bank.bands()) {
            REQUIRE(band.highHz < expected.rate * 0.5);
        }
    }

    // The invariant itself, at every rate this library accepts: a band exists
    // exactly when its upper edge is strictly below Nyquist, and every band
    // asked for is accounted for one way or the other.
    for (const double rate : supportedRates()) {
        for (const BandSpacing spacing : {BandSpacing::ThirdOctave, BandSpacing::Octave}) {
            FilterBankSettings settings;
            settings.spacing = spacing;
            const FilterBank bank = created(FilterBank::create(SampleRate{rate}, settings));
            INFO("rate " << rate);
            REQUIRE(bank.bands().size() + bank.unavailable().size() == layoutOf(settings).size());
            for (const FilterBand& band : bank.bands()) {
                REQUIRE(band.highHz < rate * 0.5);
            }
            for (const UnavailableBand& missing : bank.unavailable()) {
                REQUIRE(missing.band.highHz >= rate * 0.5);
            }
        }
    }

    // A band whose upper edge lands exactly on Nyquist is unavailable too, not
    // realised as something a hair narrower. Twice the 1 kHz octave band's
    // upper edge puts it precisely there.
    FilterBankSettings octaves;
    octaves.spacing = BandSpacing::Octave;
    const double edge = 1000.0 * std::exp2(0.5);
    const FilterBank grazing = created(FilterBank::create(SampleRate{2.0 * edge}, octaves));
    REQUIRE(!grazing.unavailable().empty());
    REQUIRE(grazing.unavailable().front().band.centreHz == Approx(1000.0));
    REQUIRE(grazing.unavailable().front().reason == BandUnavailability::AboveNyquist);
    for (const FilterBand& band : grazing.bands()) {
        REQUIRE(band.centreHz < 1000.0);
    }
}

TEST_CASE("Every realised band's poles are inside the unit circle", "[dsp][filterbank]") {
    // The place this is supposed to break is a high-order band-pass on a very
    // low centre at a very high rate, where the whole band sits within a
    // thousandth of a turn of z = 1 and the poles crowd the unit circle. So:
    // every band, every rate the library accepts, and orders from the smallest
    // to the largest a cascade can hold.
    double worstRadius = 0.0;

    for (const double rate : supportedRates()) {
        for (const BandSpacing spacing : {BandSpacing::ThirdOctave, BandSpacing::Octave}) {
            for (const int order : {2, 6, 12, 32}) {
                FilterBankSettings settings;
                settings.spacing = spacing;
                settings.order = order;
                const FilterBank bank = created(FilterBank::create(SampleRate{rate}, settings));

                for (int i = 0; i < bank.bandCount(); ++i) {
                    const FilterBand& band = bank.bands()[static_cast<std::size_t>(i)];
                    const BiquadCascade& cascade = *bank.filter(i);
                    const double radius = maxPoleRadius(cascade);
                    INFO("rate " << rate << " order " << order << " band " << band.index << " at "
                                 << band.centreHz << " Hz has a pole at radius " << radius);
                    REQUIRE(radius < 1.0);
                    REQUIRE(cascade.isStable());

                    worstRadius = std::max(worstRadius, radius);
                }
            }
        }
    }

    // Inside the unit circle is the requirement; how far inside is the thing
    // worth knowing, because a filter bank that only just passes at 768 kHz is
    // one that will stop passing on some other compiler's libm. The narrowest
    // margin anywhere in the sweep is about 1.6e-6, at order 32 and the lowest
    // third-octave band at the highest rate the library accepts. A millionth
    // is nine decimal digits clear of where double precision runs out, so this
    // is a design with room rather than one that happens to land.
    INFO("closest any pole comes to the unit circle: 1 - r = " << 1.0 - worstRadius);
    REQUIRE(1.0 - worstRadius > 1.0e-7);
}

TEST_CASE("A 25 Hz band at 192 kHz is stable and decays", "[dsp][filterbank]") {
    // The specific case that worries: a third-octave band 5.8 Hz wide at a
    // centre of 25.12 Hz, sampled at 192 kHz. The band spans a thirteen
    // thousandth of a radian on the unit circle and the poles end up within
    // 5e-5 of it. It survives -- in double. It would not in float32, which is
    // why Biquad keeps its coefficients and its state in double even though
    // the signal is float32.
    constexpr double kHighRate = 192000.0;
    const FilterBank bank = created(FilterBank::create(SampleRate{kHighRate}));

    const auto found = std::find_if(bank.bands().begin(), bank.bands().end(),
                                    [](const FilterBand& band) { return band.index == -16; });
    REQUIRE(found != bank.bands().end());
    REQUIRE(found->centreHz == Approx(25.118864));
    const auto index = static_cast<int>(std::distance(bank.bands().begin(), found));

    const BiquadCascade& cascade = *bank.filter(index);
    const double radius = maxPoleRadius(cascade);
    INFO("worst pole radius " << radius);
    REQUIRE(radius < 1.0);
    REQUIRE(radius > 0.9999); // it really is that close; the test is not vacuous
    REQUIRE(cascade.isStable());

    // Unity at the centre and 3 dB down at the edges, at this rate as at any
    // other -- the arithmetic has not quietly lost the band.
    REQUIRE(measuredDb(cascade, found->centreHz, kHighRate) == Approx(0.0).margin(0.05));
    REQUIRE(cascade.magnitudeDb(SampleRate{kHighRate}, found->lowHz) ==
            Approx(-3.0103).margin(1e-4));
    REQUIRE(cascade.magnitudeDb(SampleRate{kHighRate}, found->highHz) ==
            Approx(-3.0103).margin(1e-4));

    // And it decays rather than rings: two seconds of a tone at the centre,
    // then two seconds of silence. The slowest section has a pole 4.3e-5
    // inside the unit circle, a time constant of about 23000 samples, so two
    // seconds of silence is some sixteen of those and the tail should be gone.
    BiquadCascade running = cascade;
    running.reset();
    const auto span = static_cast<std::size_t>(2.0 * kHighRate);
    const double step = 2.0 * std::numbers::pi * found->centreHz / kHighRate;

    double driven = 0.0;
    for (std::size_t i = 0; i < span; ++i) {
        const auto input = static_cast<float>(std::sin(step * static_cast<double>(i)));
        const double output = static_cast<double>(running.processSample(input));
        if (i >= span / 2) {
            driven += output * output;
        }
    }
    double tail = 0.0;
    for (std::size_t i = 0; i < span; ++i) {
        const double output = static_cast<double>(running.processSample(0.0f));
        if (i >= span / 2) {
            tail += output * output;
        }
    }
    const double decayDb = 10.0 * std::log10(tail / driven);
    INFO("tail is " << decayDb << " dB below the driven level");
    REQUIRE(decayDb < -80.0);
}

TEST_CASE("Levels come back one per band", "[dsp][filterbank]") {
    FilterBank bank = created(FilterBank::create(SampleRate{kRate}));

    // Four seconds of a full-scale sine at the centre of the 1 kHz band. The
    // RMS of a full-scale sine is 1/sqrt(2), so the band it sits in reads
    // -3.0103 dBFS and not 0.
    const auto count = static_cast<SampleCount>(4.0 * kRate);
    AudioBuffer buffer{ChannelLayout::mono(), count};
    const double step = 2.0 * std::numbers::pi * 1000.0 / kRate;
    for (SampleCount i = 0; i < count; ++i) {
        buffer.channel(0)[i] = static_cast<float>(std::sin(step * static_cast<double>(i)));
    }

    const Result<std::vector<BandLevel>> measured = bank.measure(buffer.constView());
    REQUIRE(measured.hasValue());
    const std::vector<BandLevel>& levels = measured.value();
    REQUIRE(levels.size() == 31);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        REQUIRE(levels[i].band.index == bank.bands()[i].index);
    }

    const BandLevel& centre = levels[kKiloHertzBand];
    REQUIRE(centre.band.centreHz == Approx(1000.0));
    INFO("1 kHz band reads " << centre.levelDb << " dBFS");
    REQUIRE(centre.levelDb == Approx(-3.0103).margin(0.05));

    // The neighbour one step up is centred at 1258.9 Hz, which puts 1000 Hz at
    // a detuning of (1000/1258.9 - 1258.9/1000) * 4.31837 = -2.0062, so an
    // order-6 band-pass is 18.194 dB down there and that band should read
    // about -21.2 dBFS. Three steps up, at 1995.3 Hz, the detuning is -6.4519
    // and the attenuation 48.474 dB.
    //
    // The margin is wider than the twentieth of a decibel used for the band
    // the tone is in, because a band driven off its own centre rings at its
    // centre for the first few milliseconds, and measure() integrates that
    // ringing along with the steady state.
    const double neighbourDb = butterworthDb(6, levels[kKiloHertzBand + 1].band.lowHz,
                                             levels[kKiloHertzBand + 1].band.highHz, 1000.0, kRate);
    REQUIRE(neighbourDb == Approx(-18.194).margin(0.005));
    REQUIRE(levels[kKiloHertzBand + 1].levelDb == Approx(-3.0103 + neighbourDb).margin(0.3));

    const double distantDb = butterworthDb(6, levels[kKiloHertzBand + 3].band.lowHz,
                                           levels[kKiloHertzBand + 3].band.highHz, 1000.0, kRate);
    REQUIRE(distantDb == Approx(-48.474).margin(0.005));
    REQUIRE(levels[kKiloHertzBand + 3].levelDb < -40.0);

    // A buffer with no frames is silence in every band, not an error: there is
    // nothing wrong with asking, and nothing there to hear.
    AudioBuffer empty{ChannelLayout::mono(), 0};
    const Result<std::vector<BandLevel>> nothing = bank.measure(empty.constView());
    REQUIRE(nothing.hasValue());
    REQUIRE(nothing.value().size() == 31);
    for (const BandLevel& level : nothing.value()) {
        REQUIRE(level.levelDb == kSilenceDecibels);
    }

    // A channel that is not there is an error rather than silence, because a
    // caller asking for channel 3 of a mono file has a bug and wants to know.
    REQUIRE(!bank.measure(buffer.constView(), 1).hasValue());
    REQUIRE(!bank.measure(buffer.constView(), -1).hasValue());
    REQUIRE(bank.measure(buffer.constView(), 1).error().code() == ErrorCode::OutOfRange);
}

TEST_CASE("measure() does not depend on what the bank filtered before", "[dsp][filterbank]") {
    FilterBank bank = created(FilterBank::create(SampleRate{kRate}));
    const auto count = static_cast<SampleCount>(kRate);
    AudioBuffer buffer{ChannelLayout::mono(), count};
    const double step = 2.0 * std::numbers::pi * 1000.0 / kRate;
    for (SampleCount i = 0; i < count; ++i) {
        buffer.channel(0)[i] = static_cast<float>(0.5 * std::sin(step * static_cast<double>(i)));
    }

    const Result<std::vector<BandLevel>> first = bank.measure(buffer.constView());
    REQUIRE(first.hasValue());

    // Shove a full-scale impulse through every band and measure again. The
    // reset inside measure() is what makes the two answers identical.
    for (int i = 0; i < bank.bandCount(); ++i) {
        REQUIRE(bank.filter(i)->processSample(1.0f) != 0.0f);
    }
    const Result<std::vector<BandLevel>> second = bank.measure(buffer.constView());
    REQUIRE(second.hasValue());
    for (std::size_t i = 0; i < first.value().size(); ++i) {
        REQUIRE(second.value()[i].levelDb == first.value()[i].levelDb);
    }

    // reset() clears the bank without measuring anything.
    bank.reset();
    for (int i = 0; i < bank.bandCount(); ++i) {
        REQUIRE(bank.filter(i)->processSample(0.0f) == 0.0f);
    }
}

TEST_CASE("Filter bank refuses settings it cannot honour", "[dsp][filterbank]") {
    SECTION("sample rates that are not rates") {
        for (const double rate : {0.0, -1.0, -48000.0, 1.0e9}) {
            const Result<FilterBank> bank = FilterBank::create(SampleRate{rate});
            INFO("rate " << rate);
            REQUIRE(!bank.hasValue());
            REQUIRE(bank.error().code() == ErrorCode::InvalidArgument);
        }
        REQUIRE(!FilterBank::create(SampleRate{}).hasValue());
    }

    SECTION("orders that are not realisable as biquads") {
        // Odd orders would need a first-order section, which a cascade of
        // second-order sections does not have; zero and negative orders are not
        // filters at all. Rounding any of them up would be answering a question
        // that was not asked.
        for (const int order : {-6, -1, 0, 1, 3, 5, 7, 33}) {
            FilterBankSettings settings;
            settings.order = order;
            INFO("order " << order);
            const Result<FilterBank> bank = FilterBank::create(SampleRate{kRate}, settings);
            REQUIRE(!bank.hasValue());
            REQUIRE(bank.error().code() == ErrorCode::InvalidArgument);
        }

        // Even, but past what sixteen sections can hold.
        FilterBankSettings tooDeep;
        tooDeep.order = FilterBank::kMaxOrder + 2;
        const Result<FilterBank> bank = FilterBank::create(SampleRate{kRate}, tooDeep);
        REQUIRE(!bank.hasValue());
        REQUIRE(bank.error().code() == ErrorCode::OutOfRange);

        // The two ends that do work.
        FilterBankSettings shallow;
        shallow.order = 2;
        REQUIRE(FilterBank::create(SampleRate{kRate}, shallow).hasValue());
        FilterBankSettings deep;
        deep.order = FilterBank::kMaxOrder;
        REQUIRE(FilterBank::create(SampleRate{kRate}, deep).hasValue());
    }

    SECTION("ranges that name no bands") {
        FilterBankSettings inverted;
        inverted.lowestCentreHz = 2000.0;
        inverted.highestCentreHz = 200.0;
        REQUIRE(!bandLayout(inverted).hasValue());
        REQUIRE(!FilterBank::create(SampleRate{kRate}, inverted).hasValue());

        FilterBankSettings subsonic;
        subsonic.lowestCentreHz = 0.01;
        REQUIRE(!bandLayout(subsonic).hasValue());

        FilterBankSettings negative;
        negative.lowestCentreHz = -20.0;
        REQUIRE(!bandLayout(negative).hasValue());

        FilterBankSettings infinite;
        infinite.highestCentreHz = std::numeric_limits<double>::infinity();
        REQUIRE(!bandLayout(infinite).hasValue());

        // A range narrower than one band is empty rather than an error: it is
        // a perfectly sensible question with no bands for an answer.
        FilterBankSettings narrow;
        narrow.lowestCentreHz = 1050.0;
        narrow.highestCentreHz = 1100.0;
        const Result<std::vector<FilterBand>> none = bandLayout(narrow);
        REQUIRE(none.hasValue());
        REQUIRE(none.value().empty());
    }

    SECTION("a band index out of range has no filter") {
        const FilterBank bank = created(FilterBank::create(SampleRate{kRate}));
        REQUIRE(bank.filter(-1) == nullptr);
        REQUIRE(bank.filter(bank.bandCount()) == nullptr);
        REQUIRE(bank.filter(0) != nullptr);
        REQUIRE(bank.filter(bank.bandCount() - 1) != nullptr);
    }
}
