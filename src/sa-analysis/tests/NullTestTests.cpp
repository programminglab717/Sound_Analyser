#include <sa/analysis/NullTest.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};

/// Two seconds. Long enough that the octave band analysis has several of its
/// 8192-point windows to work with, and short enough to be quick.
constexpr SampleCount kFrames = 96000;

/// Repeatable pseudo-random noise.
///
/// Uniform rather than Gaussian so that no sample can exceed `amplitude`,
/// which the altered-region test below relies on. The mean square of a uniform
/// spread over [-A, A] is A^2/3.
[[nodiscard]] AudioBuffer noise(SampleCount frames, unsigned seed = 7, double amplitude = 0.5) {
    std::mt19937 engine{seed};
    std::uniform_real_distribution<double> spread{-amplitude, amplitude};
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        buffer.channel(0)[i] = static_cast<float>(spread(engine));
    }
    return buffer;
}

[[nodiscard]] AudioBuffer copyOf(const AudioBuffer& source) {
    AudioBuffer out{ChannelLayout::mono(), source.frames()};
    std::copy_n(source.channel(0), source.frames(), out.channel(0));
    return out;
}

/// `source` moved later by `delay` samples, same length, zeros in the gap.
/// A negative delay moves it earlier.
[[nodiscard]] AudioBuffer shifted(const AudioBuffer& source, SampleIndex delay) {
    AudioBuffer out{ChannelLayout::mono(), source.frames()};
    for (SampleCount i = 0; i < source.frames(); ++i) {
        const SampleIndex taken = i - delay;
        if (taken >= 0 && taken < source.frames()) {
            out.channel(0)[i] = source.channel(0)[taken];
        }
    }
    return out;
}

/// `source` times `factor`. Every factor used below is a power of two or its
/// negation, so the multiply is exact and the copies are scaled rather than
/// scaled-and-rounded.
[[nodiscard]] AudioBuffer scaled(const AudioBuffer& source, double factor) {
    AudioBuffer out{ChannelLayout::mono(), source.frames()};
    for (SampleCount i = 0; i < source.frames(); ++i) {
        out.channel(0)[i] = static_cast<float>(factor * static_cast<double>(source.channel(0)[i]));
    }
    return out;
}

[[nodiscard]] double rootMeanSquare(const float* samples, SampleCount frames) {
    double energy = 0.0;
    for (SampleCount i = 0; i < frames; ++i) {
        energy += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }
    return std::sqrt(energy / static_cast<double>(frames));
}

/// The difference between two float buffers, sample by sample.
///
/// Exact in double: both operands are float32 values of similar magnitude, so
/// their difference needs at most 25 significant bits and double has 53. That
/// matters because this is the ground truth the residual is measured against.
[[nodiscard]] double differenceRms(const AudioBuffer& a, const AudioBuffer& b) {
    double energy = 0.0;
    for (SampleCount i = 0; i < a.frames(); ++i) {
        const double gap =
            static_cast<double>(a.channel(0)[i]) - static_cast<double>(b.channel(0)[i]);
        energy += gap * gap;
    }
    return std::sqrt(energy / static_cast<double>(a.frames()));
}

[[nodiscard]] const NullBand& bandAt(const std::vector<NullBand>& bands, double centre) {
    const auto found = std::find_if(bands.begin(), bands.end(), [centre](const NullBand& band) {
        return std::abs(band.centreHz - centre) < 0.51;
    });
    REQUIRE(found != bands.end());
    return *found;
}

} // namespace

TEST_CASE("Two identical recordings null completely", "[analysis][null]") {
    const AudioBuffer audio = noise(kFrames);

    const auto result = nullTest(audio.view(), audio.view(), kRate);
    REQUIRE(result);
    const NullResult& measured = result.value();

    REQUIRE(measured.valid);
    REQUIRE(measured.bitIdentical);
    REQUIRE(measured.verdict == NullVerdict::BitIdentical);
    REQUIRE(measured.delaySamples == 0);
    REQUIRE(measured.comparedFrames == kFrames);
    // <a,a>/<a,a> is exactly 1, and 20*log10(1) is exactly 0.
    REQUIRE(measured.gainDb == Approx(0.0).margin(1e-12));
    REQUIRE_FALSE(measured.polarityInverted);
    // a - 1.0*a is exactly zero in every sample, so the residual is not small
    // but absent, and the floor is what absent reads as.
    REQUIRE(measured.residualDb == kDecibelFloor);
    REQUIRE(measured.peakResidualDb == kDecibelFloor);
    // Nothing in any band either. Stated as a ratio to the reference's own
    // level in each band rather than as an absolute: a band of an all-zero
    // residual reads wherever the spectrum analyser's per-bin silence adds up
    // to, which is somewhere below -160 dBFS and depends on how many bins the
    // band holds. What matters is that every band is further below its own
    // reference than a difference could be.
    REQUIRE_FALSE(measured.bands.empty());
    for (const NullBand& band : measured.bands) {
        REQUIRE(band.relativeDb < kNullFloorDb);
    }
}

TEST_CASE("A delayed copy is found and then nulls", "[analysis][null]") {
    const AudioBuffer reference = noise(kFrames);

    // Both directions: the second recording 137 samples late, and the second
    // recording 137 samples early.
    for (const SampleIndex delay : {SampleIndex{137}, SampleIndex{-137}}) {
        const AudioBuffer other = shifted(reference, delay);
        const auto result = nullTest(reference.view(), other.view(), kRate);
        REQUIRE(result);
        const NullResult& measured = result.value();

        REQUIRE(measured.delaySamples == delay);
        // 96000 frames with 137 of them shifted out of the overlap either way.
        REQUIRE(measured.comparedFrames == kFrames - 137);
        // Over that overlap the two hold the same samples, so the fit is
        // exactly 1 and the subtraction cancels exactly.
        REQUIRE(measured.gainDb == Approx(0.0).margin(1e-12));
        REQUIRE(measured.residualDb == kDecibelFloor);
        REQUIRE(measured.peakResidualDb == kDecibelFloor);
        // Nulls, and is still not the same file.
        REQUIRE_FALSE(measured.bitIdentical);
        REQUIRE(measured.verdict == NullVerdict::WithinFloatFloor);
    }
}

TEST_CASE("A copy at half amplitude gain-matches and nulls", "[analysis][null]") {
    const AudioBuffer reference = noise(kFrames);
    const AudioBuffer other = scaled(reference, 0.5);

    const auto result = nullTest(reference.view(), other.view(), kRate);
    REQUIRE(result);
    const NullResult& measured = result.value();

    REQUIRE(measured.delaySamples == 0);
    // 20*log10(0.5) = -6.020599913279624. Halving a float is exact and the
    // squared terms of two float32 samples are exact in a double accumulator,
    // so <other,reference>/<reference,reference> is exactly 0.5 and the figure
    // is the logarithm's own rounding of that, not an estimate of it.
    REQUIRE(measured.gainDb == Approx(-6.020599913279624).margin(1e-9));
    REQUIRE_FALSE(measured.polarityInverted);
    // The correcting fit is then exactly 2, and reference - 2*other is exactly
    // zero in every sample.
    REQUIRE(measured.residualDb == kDecibelFloor);
    REQUIRE(measured.verdict == NullVerdict::WithinFloatFloor);
}

TEST_CASE("A copy with its polarity flipped nulls, and says so", "[analysis][null]") {
    const AudioBuffer reference = noise(kFrames);
    const AudioBuffer other = scaled(reference, -1.0);

    const auto result = nullTest(reference.view(), other.view(), kRate);
    REQUIRE(result);
    const NullResult& measured = result.value();

    // The correlation peaks negatively at lag zero, and the peak is taken by
    // magnitude, so the alignment is not fooled into hunting for a lag that
    // correlates less badly.
    REQUIRE(measured.delaySamples == 0);
    REQUIRE(measured.polarityInverted);
    // The fit is exactly -1, whose magnitude in decibels is 0.
    REQUIRE(measured.gainDb == Approx(0.0).margin(1e-12));
    REQUIRE(measured.residualDb == kDecibelFloor);

    // Without the gain match it is the worst case rather than the best:
    // reference - (-reference) is twice the reference, 20*log10(2) = +6.0206.
    NullSettings asIs;
    asIs.matchGain = false;
    const auto uncorrected = nullTest(reference.view(), other.view(), kRate, asIs);
    REQUIRE(uncorrected);
    REQUIRE(uncorrected.value().residualDb == Approx(6.020599913279624).margin(1e-9));
    REQUIRE(uncorrected.value().verdict == NullVerdict::Different);
}

TEST_CASE("The residual equals the noise that was added to it", "[analysis][null]") {
    // The quantitative claim. A copy with independent noise added at a known
    // level has to come back with a residual at that level.
    //
    // The gain match biases it a little and the bias can be written down. With
    // the added noise carrying a fraction e of the reference's energy, the fit
    // is <r, r+d>/<r+d, r+d> = 1/(1+e) to within the accidental correlation
    // between r and d, and the residual it leaves is (1-1/(1+e))*r - d/(1+e),
    // whose energy is e/(1+e) times the reference's. At e = 1e-4 that is
    // 10*log10(1/(1+1e-4)) = -0.0004 dB away from the added noise's own level,
    // which is why the margin below can be a fiftieth of a decibel.
    const AudioBuffer reference = noise(kFrames, 7);

    for (const double level : {-40.0, -80.0}) {
        const double scale = std::pow(10.0, level / 20.0);
        const AudioBuffer added = noise(kFrames, 4321, 0.5 * scale);

        AudioBuffer other = copyOf(reference);
        for (SampleCount i = 0; i < kFrames; ++i) {
            other.channel(0)[i] = static_cast<float>(static_cast<double>(reference.channel(0)[i]) +
                                                     static_cast<double>(added.channel(0)[i]));
        }

        // Ground truth taken from the buffers themselves rather than from the
        // level asked for: rounding the sum back to float32 moves it a little,
        // and the residual has to match what is actually there.
        const double expectedDb = 20.0 * std::log10(differenceRms(other, reference) /
                                                    rootMeanSquare(reference.channel(0), kFrames));

        const auto result = nullTest(reference.view(), other.view(), kRate);
        REQUIRE(result);
        const NullResult& measured = result.value();

        REQUIRE(measured.delaySamples == 0);
        REQUIRE(measured.residualDb == Approx(expectedDb).margin(0.02));
        REQUIRE(measured.verdict == NullVerdict::Different);
        // Sanity on the construction itself: -40 dB of noise must come back as
        // -40 dB and not as some other decade.
        REQUIRE(expectedDb == Approx(level).margin(0.05));
    }
}

TEST_CASE("The worst moment points at the region that differs", "[analysis][null]") {
    const AudioBuffer reference = noise(kFrames, 7, 0.5);
    AudioBuffer other = copyOf(reference);

    // Ten milliseconds replaced at 1.000 s, with noise no sample of which can
    // fall below 0.6 in magnitude -- above anything the reference reaches, so
    // the largest residual sample has to be inside the replacement.
    const SampleIndex at = 48000;
    const SampleCount length = 480;
    std::mt19937 engine{99};
    std::uniform_real_distribution<double> spread{0.6, 0.9};
    for (SampleCount i = 0; i < length; ++i) {
        const double sign = (i % 2 == 0) ? 1.0 : -1.0;
        other.channel(0)[at + i] = static_cast<float>(sign * spread(engine));
    }

    const auto result = nullTest(reference.view(), other.view(), kRate);
    REQUIRE(result);
    const NullResult& measured = result.value();

    REQUIRE(measured.delaySamples == 0);
    // 48000 / 48000 = 1.0 s, and the region runs to 480 / 48000 = 0.010 s
    // later.
    REQUIRE(measured.worstTimeSeconds >= 1.0);
    REQUIRE(measured.worstTimeSeconds < 1.010);
    // Over the replaced region the residual is reference - g*replacement with
    // g near 0.97. Its mean square is 0.5^2/3 + 0.97^2 * E[u^2] where u is
    // uniform on [0.6, 0.9] and E[u^2] = (0.9^3 - 0.6^3)/(3*0.3) = 0.57, so
    // about 0.62 and an RMS of 0.79. A peak is never below the RMS of the
    // samples it is a peak over, so -20 dBFS -- an amplitude of 0.1 -- is a
    // wide margin rather than a close call.
    REQUIRE(measured.peakResidualDb > -20.0);
    REQUIRE(measured.verdict == NullVerdict::Different);

    // Yet overall the residual is a long way down: 480 frames of that 0.62
    // against 96000 frames of the reference's 0.5^2/3 = 0.0833 is
    // 10*log10((480*0.62)/(96000*0.0833)) = -14.2 dB. The moment is obvious
    // and the average is quiet, which is exactly why both are reported.
    REQUIRE(measured.residualDb < -10.0);
    REQUIRE(measured.residualDb > -18.0);
}

TEST_CASE("With gain matching off a scaled copy shows its level instead of nulling",
          "[analysis][null]") {
    const AudioBuffer reference = noise(kFrames);
    const AudioBuffer other = scaled(reference, 0.5);

    NullSettings asIs;
    asIs.matchGain = false;
    const auto result = nullTest(reference.view(), other.view(), kRate, asIs);
    REQUIRE(result);
    const NullResult& measured = result.value();

    // Nothing is applied, so the residual is reference - 0.5*reference, which
    // is exactly half the reference: 20*log10(0.5) = -6.0206 dB below it.
    REQUIRE(measured.residualDb == Approx(-6.020599913279624).margin(1e-9));
    REQUIRE(measured.verdict == NullVerdict::Different);
    // The level difference is still measured. The setting decides whether the
    // subtraction corrects for it, not whether it is looked at.
    REQUIRE(measured.gainDb == Approx(-6.020599913279624).margin(1e-9));

    // The same pair with the setting on, which is what proves the setting is
    // doing the work rather than the material being awkward.
    const auto corrected = nullTest(reference.view(), other.view(), kRate);
    REQUIRE(corrected);
    REQUIRE(corrected.value().residualDb == kDecibelFloor);
}

TEST_CASE("Two unrelated recordings do not null", "[analysis][null]") {
    const AudioBuffer reference = noise(kFrames, 11);
    const AudioBuffer other = noise(kFrames, 29);

    const auto result = nullTest(reference.view(), other.view(), kRate);
    REQUIRE(result);
    const NullResult& measured = result.value();

    // With gain matching on, the residual keeps (1 - rho^2) of the reference's
    // power, where rho is the correlation between the two at whatever lag the
    // search settled on. On independent noise the alignment finds an arbitrary
    // lag and the best rho over the roughly 2*96000 of them is about
    // sqrt(2*ln(2*96000)/96000) = 0.016, so the residual keeps
    // 10*log10(1 - 0.016^2) = -0.001 dB less than all of it. The figure can
    // never exceed 0 dB, because subtracting the best multiple of anything
    // cannot leave more than subtracting nothing.
    REQUIRE(measured.residualDb <= 0.0);
    REQUIRE(measured.residualDb > -0.5);
    REQUIRE(measured.verdict == NullVerdict::Different);
    REQUIRE_FALSE(measured.bitIdentical);
}

TEST_CASE("The band breakdown finds which octave the difference is in", "[analysis][null]") {
    const AudioBuffer reference = noise(kFrames, 7, 0.5);

    // A 1 kHz tone at 0.02 added to broadband noise. The tone sits wholly
    // inside the 1 kHz octave band, which runs from 707 to 1414 Hz.
    AudioBuffer other = copyOf(reference);
    for (SampleCount i = 0; i < kFrames; ++i) {
        const double tone =
            0.02 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate.hz());
        other.channel(0)[i] =
            static_cast<float>(static_cast<double>(reference.channel(0)[i]) + tone);
    }

    const auto result = nullTest(reference.view(), other.view(), kRate);
    REQUIRE(result);
    const NullResult& measured = result.value();

    // Ten octave bands at 48 kHz, 31.5 Hz to 16 kHz.
    REQUIRE(measured.bands.size() == 10);
    const NullBand& home = bandAt(measured.bands, 1000.0);

    // The tone carries 0.02^2/2 = 2.0e-4 of mean square against the noise's
    // 0.5^2/3 = 8.33e-2, so the gain match leaves 1 - 1/(1 + 2.4e-3) = 2.4e-3
    // of the reference behind everywhere, which is -52 dB. In the 1 kHz band
    // the residual is the tone itself, 20*log10(0.02/sqrt(2)) = -37 dBFS
    // against a band level of roughly -26 dBFS, so about -11 dB. The gap is
    // some forty decibels; twenty is asserted, which leaves room for the
    // analysis window's skirts without letting a wrong answer through.
    for (const NullBand& band : measured.bands) {
        if (std::abs(band.centreHz - 1000.0) < 0.51) {
            continue;
        }
        REQUIRE(band.relativeDb < home.relativeDb - 20.0);
    }
    // And the band figures are levels, not a separate opinion: the residual in
    // the 1 kHz band is above the overall residual, which is what "the
    // difference lives here" means.
    REQUIRE(home.relativeDb > measured.residualDb);
}

TEST_CASE("The delay search gives one answer in one block or many", "[analysis][null]") {
    const AudioBuffer reference = noise(kFrames);
    const AudioBuffer other = shifted(reference, 137);

    // maxDelaySamples bounds the lag range, and the lag range is what sizes
    // the transform: at 512 the 96000-frame reference is correlated in
    // thirty-odd blocks rather than in one pass, so this is the accumulation
    // across blocks being checked against the single-pass answer above.
    NullSettings bounded;
    bounded.maxDelaySamples = 512;
    const auto found = nullTest(reference.view(), other.view(), kRate, bounded);
    REQUIRE(found);
    REQUIRE(found.value().delaySamples == 137);
    REQUIRE(found.value().residualDb == kDecibelFloor);

    // A bound too small to contain the delay is a constraint and not a hint:
    // the answer stays inside the range it was given, and is wrong, and the
    // residual says so rather than the delay pretending otherwise.
    NullSettings tooTight;
    tooTight.maxDelaySamples = 50;
    const auto clipped = nullTest(reference.view(), other.view(), kRate, tooTight);
    REQUIRE(clipped);
    REQUIRE(std::abs(clipped.value().delaySamples) <= 50);
    REQUIRE(clipped.value().residualDb > -0.5);
}

TEST_CASE("A delay is found under noise that buries the signal", "[analysis][null]") {
    // What summing the blocks is for, rather than taking any one of them. The
    // copy is delayed by 137 and then swamped by noise a hundred times its
    // power, so the correlation at the true lag is only worth having once it
    // has been accumulated over the whole recording.
    //
    // Reference mean square 0.05^2/3 = 8.33e-4, noise mean square 0.5^2/3 =
    // 8.33e-2, a ratio of 100. At the true lag the correlation is N times the
    // reference's mean square: 95863 * 8.33e-4 = 79.9. At a wrong lag it is a
    // random walk whose deviation is sqrt(N * 8.33e-4 * (8.33e-4 + 8.33e-2)) =
    // 2.59, and the largest of the 1025 lags searched is about 3.3 of those,
    // so 8.5. Over the whole recording that is 79.9 against 8.5; over the last
    // 768-frame block alone it would be 0.64 against 0.77, which is to say it
    // would be a coin toss.
    const AudioBuffer reference = noise(kFrames, 7, 0.05);
    const AudioBuffer delayed = shifted(reference, 137);
    const AudioBuffer buried = noise(kFrames, 555, 0.5);

    AudioBuffer other{ChannelLayout::mono(), kFrames};
    for (SampleCount i = 0; i < kFrames; ++i) {
        other.channel(0)[i] = static_cast<float>(static_cast<double>(delayed.channel(0)[i]) +
                                                 static_cast<double>(buried.channel(0)[i]));
    }

    NullSettings bounded;
    bounded.maxDelaySamples = 512; // Thirty-two blocks of 3072 frames.
    const auto result = nullTest(reference.view(), other.view(), kRate, bounded);
    REQUIRE(result);
    REQUIRE(result.value().delaySamples == 137);

    // Nothing nulls here and nothing should. The fit that minimises the
    // residual is <r, r+n>/<r+n, r+n> = 1/101, so the residual keeps
    // 1 - 1/101 of the reference's power: 10*log10(0.9901) = -0.043 dB.
    REQUIRE(result.value().residualDb <= 0.0);
    REQUIRE(result.value().residualDb > -0.2);

    // The level figure reads 0 dB all the same, and is right to: the reference
    // is present in the other recording at unity, and the noise piled on top
    // of it is not the reference. What moves it is the accidental correlation
    // between reference and noise, of order sqrt(100/N) = 0.032, which is a
    // couple of tenths of a decibel either way -- this draw comes out at 0.29.
    REQUIRE(result.value().gainDb == Approx(0.0).margin(0.5));
}

TEST_CASE("Alignment can be turned off, and then a shift is the difference", "[analysis][null]") {
    const AudioBuffer reference = noise(kFrames);
    const AudioBuffer other = shifted(reference, 137);

    NullSettings asIs;
    asIs.alignDelay = false;
    const auto result = nullTest(reference.view(), other.view(), kRate, asIs);
    REQUIRE(result);

    REQUIRE(result.value().delaySamples == 0);
    REQUIRE(result.value().comparedFrames == kFrames);
    // Noise against a shifted copy of itself is noise against unrelated noise,
    // so the residual is the whole reference, as in the unrelated case above.
    REQUIRE(result.value().residualDb <= 0.0);
    REQUIRE(result.value().residualDb > -0.5);
    REQUIRE(result.value().verdict == NullVerdict::Different);
}

TEST_CASE("A null test refuses what it cannot compare", "[analysis][null]") {
    const AudioBuffer mono = noise(4800);
    const AudioBuffer stereo{ChannelLayout::stereo(), 4800};

    // A channel index neither recording has, and one only the first has.
    const auto missing = nullTest(mono.view(), mono.view(), kRate, NullSettings{}, 1);
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().code() == ErrorCode::OutOfRange);
    REQUIRE_FALSE(nullTest(stereo.view(), mono.view(), kRate, NullSettings{}, 1));
    REQUIRE_FALSE(nullTest(mono.view(), mono.view(), kRate, NullSettings{}, -1));

    // Every time in the result is a count of samples divided by the rate.
    const auto noRate = nullTest(mono.view(), mono.view(), SampleRate{0.0});
    REQUIRE_FALSE(noRate);
    REQUIRE(noRate.error().code() == ErrorCode::InvalidArgument);

    // Nothing to compare, from either side.
    const AudioBuffer empty{ChannelLayout::mono(), 0};
    REQUIRE_FALSE(nullTest(empty.view(), mono.view(), kRate));
    REQUIRE_FALSE(nullTest(mono.view(), empty.view(), kRate));

    // Two recordings with nothing in common. Stated plainly: the delay search
    // is clamped to lags at which the two share at least one sample, so the
    // overlap can never come out empty for recordings that have samples, and a
    // view of no frames is the only way to get there. That reaches the same
    // refusal as the empty buffers above rather than a different one -- the
    // guard on the overlap itself is defensive and is not exercised here.
    const auto disjoint = nullTest(mono.view().subRange(0, 0), mono.view(), kRate);
    REQUIRE_FALSE(disjoint);
    REQUIRE(disjoint.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("Silence is compared without inventing a ratio to it", "[analysis][null]") {
    const AudioBuffer silence{ChannelLayout::mono(), kFrames};
    const AudioBuffer audio = noise(kFrames);

    // Two silences are a perfect null and a real answer.
    const auto both = nullTest(silence.view(), silence.view(), kRate);
    REQUIRE(both);
    REQUIRE(both.value().valid);
    REQUIRE(both.value().bitIdentical);
    REQUIRE(both.value().delaySamples == 0); // Every lag ties at zero; the smallest wins.
    REQUIRE(both.value().residualDb == kDecibelFloor);

    // A silent reference against something is a ratio with nothing underneath
    // it, so the figures are withdrawn rather than floored or infinite.
    const auto onesided = nullTest(silence.view(), audio.view(), kRate);
    REQUIRE(onesided);
    REQUIRE_FALSE(onesided.value().valid);
    REQUIRE(onesided.value().verdict == NullVerdict::Different);

    // The other way round the reference has energy, so the residual is the
    // whole of it: nothing in the silent copy can cancel any of it.
    const auto silentCopy = nullTest(audio.view(), silence.view(), kRate);
    REQUIRE(silentCopy);
    REQUIRE(silentCopy.value().valid);
    REQUIRE(silentCopy.value().residualDb == Approx(0.0).margin(1e-12));
    // None of the reference is present in a silent copy, and the gain figure
    // is at the floor rather than at some level.
    REQUIRE(silentCopy.value().gainDb == kDecibelFloor);
}
