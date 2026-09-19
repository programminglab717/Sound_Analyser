#include <sa/dsp/Fft.h>
#include <sa/dsp/MultibandCompressor.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};

/// Deterministic white noise, so a failure is reproducible from the seed.
[[nodiscard]] AudioBuffer noise(SampleCount frames, int channels, double amplitude, unsigned seed) {
    AudioBuffer audio{ChannelLayout::discrete(channels), frames};
    unsigned state = seed;
    for (int channel = 0; channel < channels; ++channel) {
        for (SampleCount i = 0; i < frames; ++i) {
            state = 1103515245u * state + 12345u;
            audio.channel(channel)[i] = static_cast<float>(
                amplitude * (static_cast<double>(state % 65536u) / 32768.0 - 1.0));
        }
    }
    return audio;
}

/// A sum of tones, faded in over `fade` frames.
///
/// The fade is not decoration. A sine switched on at full amplitude is a step
/// in the first derivative, the crossover spreads that step across every band
/// at once, and a band that is supposed to be empty then has a transient in it
/// large enough for its compressor to act on -- which would make "no other band
/// touched it" a statement about the onset rather than about the tone.
[[nodiscard]] AudioBuffer tones(SampleCount frames,
                                const std::vector<std::pair<double, double>>& parts,
                                int channels = 1, SampleCount fade = 4800) {
    AudioBuffer audio{ChannelLayout::discrete(channels), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        const double ramp = i >= fade
                                ? 1.0
                                : 0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(i) /
                                                        static_cast<double>(fade)));
        double value = 0.0;
        for (const auto& [hz, amplitude] : parts) {
            value += ramp * amplitude *
                     std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate.hz());
        }
        for (int channel = 0; channel < channels; ++channel) {
            audio.channel(channel)[i] = static_cast<float>(value);
        }
    }
    return audio;
}

[[nodiscard]] AudioBuffer copyOf(const AudioBuffer& source) {
    AudioBuffer audio{ChannelLayout::discrete(source.channelCount()), source.frames()};
    for (int channel = 0; channel < source.channelCount(); ++channel) {
        std::copy_n(source.channel(channel), source.frames(), audio.channel(channel));
    }
    return audio;
}

[[nodiscard]] std::vector<MultibandBandSettings> bandsOf(std::size_t count, bool bypass = false) {
    std::vector<MultibandBandSettings> bands(count);
    for (MultibandBandSettings& band : bands) {
        band.bypass = bypass;
    }
    return bands;
}

/// The all-pass a Linkwitz-Riley crossover of `order` sums to.
///
/// Derived rather than taken from the implementation. With N = order/2 and D(s)
/// the Butterworth denominator of order N, the two halves are 1/D(s)^2 and
/// s^2N/D(s)^2; D(s) D(-s) = 1 + s^2N is the Butterworth magnitude relation, so
/// the halves sum to D(-s)/D(s). That is the all-pass sitting at the crossover
/// with the Butterworth section Qs, and it is built here out of
/// BiquadCoefficients::allPass -- a designer the crossover itself never calls,
/// so the two agreeing is evidence rather than a restatement.
[[nodiscard]] BiquadCascade referenceAllPass(int order, double frequency) {
    BiquadCascade cascade;
    const int butterworthOrder = order / 2;
    for (int k = 0; k < butterworthOrder / 2; ++k) {
        const double angle = std::numbers::pi * static_cast<double>(2 * k + 1) /
                             (2.0 * static_cast<double>(butterworthOrder));
        const Result<BiquadCoefficients> section =
            BiquadCoefficients::allPass(kRate, frequency, 0.5 / std::cos(angle));
        REQUIRE(section);
        REQUIRE(cascade.append(section.value()));
    }
    return cascade;
}

void applyReferenceAllPasses(AudioBufferView audio, const std::vector<double>& crossoverHz,
                             int order) {
    for (const double frequency : crossoverHz) {
        const BiquadCascade prototype = referenceAllPass(order, frequency);
        for (int channel = 0; channel < audio.channelCount(); ++channel) {
            BiquadCascade filter = prototype;
            filter.processInPlace(audio.channel(channel), audio.frames());
        }
    }
}

[[nodiscard]] double worstDifference(const AudioBuffer& a, const AudioBuffer& b) {
    double worst = 0.0;
    for (int channel = 0; channel < a.channelCount(); ++channel) {
        for (SampleCount i = 0; i < a.frames(); ++i) {
            worst = std::max(worst, std::abs(static_cast<double>(a.channel(channel)[i]) -
                                             static_cast<double>(b.channel(channel)[i])));
        }
    }
    return worst;
}

[[nodiscard]] bool identical(const AudioBuffer& a, const AudioBuffer& b) {
    if (a.channelCount() != b.channelCount() || a.frames() != b.frames()) {
        return false;
    }
    for (int channel = 0; channel < a.channelCount(); ++channel) {
        for (SampleCount i = 0; i < a.frames(); ++i) {
            if (a.channel(channel)[i] != b.channel(channel)[i]) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] double rmsOf(const AudioBuffer& audio, SampleIndex from, SampleCount count,
                           int channel = 0) {
    double sum = 0.0;
    for (SampleCount i = 0; i < count; ++i) {
        const double sample = static_cast<double>(audio.channel(channel)[from + i]);
        sum += sample * sample;
    }
    return std::sqrt(sum / static_cast<double>(count));
}

[[nodiscard]] double decibels(double linear) {
    return 20.0 * std::log10(std::max(linear, 1e-15));
}

/// Amplitude of one FFT bin over the last `size` frames, in decibels.
///
/// Rectangular window on purpose: every tone measured through this sits exactly
/// on a bin, so it completes a whole number of cycles in the window and there
/// is nothing for a window function to suppress.
[[nodiscard]] double binLevelDb(const AudioBuffer& audio, int size, int bin, int channel = 0) {
    const RealFft fft{size};
    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(fft.binCount()));
    fft.forward(audio.channel(channel) + (audio.frames() - size), spectrum.data());
    // A sine of amplitude A over N samples puts A N / 2 into its own bin.
    const double magnitude = static_cast<double>(std::abs(spectrum[static_cast<std::size_t>(bin)]));
    return decibels(magnitude * 2.0 / static_cast<double>(size));
}

} // namespace

TEST_CASE("Bypassed bands sum back to the input through one all-pass per crossover") {
    // The property the whole design rests on, stated as what it really is. The
    // output is not the input: it is the input through the all-pass each
    // crossover sums to, one per crossover, and that chain is built here from a
    // different designer than the crossover uses.
    //
    // The residual is not zero because the two chains are different arithmetic
    // on the same transfer function -- six or more biquads' worth of float32
    // rounding either way, and each biquad returns a float. One unit in the
    // last place of a float near 0.5 is 5.96e-8, and what these cases measure
    // is one, two or three of them: 6e-8 through 1.8e-7, the same figures on
    // debug, release and asan. 1e-6 is that worst case with room, and still
    // five orders of magnitude under anything a design error could hide in.
    struct Case {
        std::vector<double> crossovers;
        int order;
    };

    const std::vector<Case> cases = {
        {{1000.0}, 4},
        {{1000.0}, 8},
        {{500.0}, 12},
        {{200.0, 2000.0, 8000.0}, 4},
        {{200.0, 2000.0, 8000.0}, 8},
        {{80.0, 400.0, 2500.0, 10000.0}, 4},
    };

    for (std::size_t index = 0; index < cases.size(); ++index) {
        const Case& one = cases[index];
        CAPTURE(index, one.order, one.crossovers.size());

        AudioBuffer audio = noise(48000, 2, 0.5, 2024u);
        AudioBuffer reference = copyOf(audio);
        applyReferenceAllPasses(reference.view(), one.crossovers, one.order);

        MultibandSettings settings;
        settings.crossoverHz = one.crossovers;
        settings.order = one.order;
        settings.bands = bandsOf(one.crossovers.size() + 1, true);

        const Result<MultibandResult> result = compressMultiband(audio.view(), kRate, settings);
        REQUIRE(result);
        REQUIRE(result.value().gainReductionDb.size() == one.crossovers.size() + 1);

        const double residual = worstDifference(audio, reference);
        CAPTURE(residual);
        REQUIRE(residual < 1e-6);
    }
}

TEST_CASE("Bypassed bands recombine flat in magnitude") {
    // The same property measured the other way, as a response rather than as a
    // residual: an impulse in, the summed bands out, and the magnitude of the
    // transform of that is what a signal passing through unprocessed is
    // multiplied by at each frequency.
    //
    // 16384 points at 48 kHz is 341 ms, and the slowest thing here is the
    // 200 Hz crossover, whose poles sit at a radius of 0.982 and are two
    // thousand samples from irrelevance -- so the response is inside the window
    // and there is nothing for truncation to smear.
    constexpr int kSize = 16384;
    constexpr double kBinHz = 48000.0 / static_cast<double>(kSize);

    AudioBuffer audio{ChannelLayout::mono(), kSize};
    audio.channel(0)[0] = 1.0f;

    MultibandSettings settings;
    settings.bands = bandsOf(settings.crossoverHz.size() + 1, true);
    REQUIRE(compressMultiband(audio.view(), kRate, settings));

    const RealFft fft{kSize};
    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(fft.binCount()));
    fft.forward(audio.channel(0), spectrum.data());

    const int lowest = static_cast<int>(std::ceil(20.0 / kBinHz));
    const int highest = static_cast<int>(std::floor(20000.0 / kBinHz));
    double worstDb = 0.0;
    for (int bin = lowest; bin <= highest; ++bin) {
        const double magnitude =
            static_cast<double>(std::abs(spectrum[static_cast<std::size_t>(bin)]));
        worstDb = std::max(worstDb, std::abs(decibels(magnitude)));
    }

    // Measured 0.0000026 dB on debug, release and asan alike. What is left is
    // the float32 rounding of six biquads and of the transform itself, not the
    // design: a magnitude wrong in the last float bit, 6e-8 of itself, is
    // 0.0000005 dB out, so a handful of those is the whole of this figure.
    //
    // The bound is four times the measurement rather than a hundred times it,
    // because what this has to catch is a design error and the smallest of
    // those is large. Dropping the all-pass compensation -- one loop, and the
    // mistake this design exists to avoid -- takes the same figure to 0.625 dB.
    CAPTURE(worstDb);
    REQUIRE(worstDb < 1e-5);
}

TEST_CASE("A tone inside one band is compressed by that band's settings and no other's") {
    // Crossovers at 50 Hz and 10 kHz, and a tone at their geometric centre. An
    // LR4 half's magnitude is 1/(1+(f/fc)^4) below the crossover and
    // (f/fc)^4/(1+(f/fc)^4) above it, so at 707.107 Hz -- 14.142 times 50 and
    // 1/14.142 of 10000, and 14.142^4 is 40000 -- each skirt is one part in
    // 40001 down, which is 0.0002 dB. The middle band therefore carries the
    // tone at the level it arrived at, and the expected reduction follows from
    // the input amplitude alone.
    //
    // The same two ratios put the tone 92 dB down in each outer band, which is
    // -98 dBFS against a -20 dB threshold: those two compressors have nothing
    // to do and must do exactly nothing.
    constexpr double kToneHz = 707.10678; // sqrt(50 * 10000)
    constexpr double kAmplitude = 0.5;

    AudioBuffer audio = tones(96000, {{kToneHz, kAmplitude}});
    const AudioBuffer original = copyOf(audio);

    CompressorSettings band;
    band.thresholdDb = -20.0;
    band.ratio = 4.0;
    band.kneeDb = 0.0;
    band.attackSeconds = 0.0002;
    // Far longer than the tone's 1.4 ms period, deliberately. A compressor
    // detecting on sample magnitude sees a sine sweep to zero and back twice a
    // cycle; with a release this slow the envelope cannot follow it down, so
    // the gain settles at the static curve's answer for the *peak* rather than
    // at some average of the sweep. OfflineDynamicsTests makes the same point
    // with a square wave, which is not an option inside one band.
    band.releaseSeconds = 5.0;

    MultibandSettings settings;
    settings.crossoverHz = {50.0, 10000.0};
    settings.bands = bandsOf(3);
    for (MultibandBandSettings& one : settings.bands) {
        one.compressor = band;
    }

    const Result<MultibandResult> result = compressMultiband(audio.view(), kRate, settings);
    REQUIRE(result);

    // -6.02 dBFS in, 13.98 dB over the threshold, and a 4:1 ratio gives back a
    // quarter of that: 3.49 dB over, so 10.48 dB of reduction.
    const double expectedDb = -compressorGainDb(band, decibels(kAmplitude));
    REQUIRE(expectedDb == Approx(10.4846).margin(0.001));

    CAPTURE(result.value().gainReductionDb[1]);
    REQUIRE(result.value().gainReductionDb[1] == Approx(expectedDb).margin(0.02));
    // Exactly zero, not nearly: below the threshold with a hard knee the gain
    // computer returns 0 dB, which is a gain of exactly 1, so those bands come
    // out of the compressor as the same floats that went in.
    REQUIRE(result.value().gainReductionDb[0] == 0.0);
    REQUIRE(result.value().gainReductionDb[2] == 0.0);

    // And the level really moved by that much. The last half second, by which
    // point the envelope has long settled; RMS rather than peak because an
    // all-pass chain has moved the crests off the sample grid.
    const double before = decibels(rmsOf(original, 72000, 24000));
    const double after = decibels(rmsOf(audio, 72000, 24000));
    REQUIRE(before - after == Approx(result.value().gainReductionDb[1]).margin(0.02));
}

TEST_CASE("Raising one band's threshold moves that band and leaves the others where they were") {
    // Two tones, one in the bottom band and one in the top, each sitting
    // exactly on an FFT bin so that a rectangular window reads its amplitude
    // with nothing to leak. Crossovers at 300 and 2000 Hz put the 29.3 Hz tone
    // 80.8 dB down in the middle band and the 7500 Hz tone 112 dB down in the
    // bottom one, so neither compressor can reach the other's tone: a 10 dB
    // change of gain in one band moves the other band's tone by under a
    // thousandth of a decibel.
    constexpr int kSize = 16384;
    constexpr int kLowBin = 10;    // 29.296875 Hz
    constexpr int kHighBin = 2560; // 7500 Hz exactly
    constexpr double kBinHz = 48000.0 / static_cast<double>(kSize);
    const double kAmplitude = 0.4;

    const auto run = [&](double lowThresholdDb) {
        AudioBuffer audio =
            tones(65536, {{kLowBin * kBinHz, kAmplitude}, {kHighBin * kBinHz, kAmplitude}});

        CompressorSettings compressor;
        compressor.thresholdDb = -20.0;
        compressor.ratio = 4.0;
        compressor.kneeDb = 0.0;
        compressor.attackSeconds = 0.0002;
        compressor.releaseSeconds = 5.0;

        MultibandSettings settings;
        settings.crossoverHz = {300.0, 2000.0};
        settings.bands = bandsOf(3);
        for (MultibandBandSettings& band : settings.bands) {
            band.compressor = compressor;
        }
        settings.bands[0].compressor.thresholdDb = lowThresholdDb;

        Result<MultibandResult> result = compressMultiband(audio.view(), kRate, settings);
        REQUIRE(result);
        return std::pair{std::move(audio), std::move(result).value()};
    };

    const auto [compressed, compressedReport] = run(-20.0);
    const auto [released, releasedReport] = run(0.0);

    // -7.96 dBFS against a -20 dB threshold is 12.04 dB over, and a quarter of
    // that is kept: 9.03 dB of reduction. Against a 0 dB threshold the tone is
    // under it and nothing happens at all.
    REQUIRE(compressedReport.gainReductionDb[0] == Approx(9.031).margin(0.02));
    REQUIRE(releasedReport.gainReductionDb[0] == 0.0);

    const double lowMoved =
        binLevelDb(released, kSize, kLowBin) - binLevelDb(compressed, kSize, kLowBin);
    const double highMoved =
        binLevelDb(released, kSize, kHighBin) - binLevelDb(compressed, kSize, kHighBin);
    CAPTURE(lowMoved, highMoved);
    REQUIRE(lowMoved == Approx(9.031).margin(0.05));
    REQUIRE(highMoved == Approx(0.0).margin(0.005));

    // The top band's compressor saw the same audio under the same settings in
    // both runs, so its report is not merely close -- it is the same number.
    REQUIRE(compressedReport.gainReductionDb[2] == releasedReport.gainReductionDb[2]);
    REQUIRE(compressedReport.gainReductionDb[2] > 1.0);
}

TEST_CASE("Solo takes the bands it names and drops the rest") {
    // Soloing decides what is heard, not what each band does, so the three
    // solo-one-band runs must add up to the run with nothing soloed --
    // exactly, because the bands are summed in the same order in both and
    // adding zero to a float changes nothing.
    const AudioBuffer source = noise(48000, 1, 0.5, 77u);

    CompressorSettings compressor;
    compressor.thresholdDb = -30.0;
    compressor.ratio = 6.0;
    compressor.attackSeconds = 0.005;
    compressor.releaseSeconds = 0.150;

    MultibandSettings settings;
    settings.crossoverHz = {300.0, 3000.0};
    settings.bands = bandsOf(3);
    for (MultibandBandSettings& band : settings.bands) {
        band.compressor = compressor;
    }

    AudioBuffer whole = copyOf(source);
    const Result<MultibandResult> wholeResult = compressMultiband(whole.view(), kRate, settings);
    REQUIRE(wholeResult);

    AudioBuffer rebuilt{ChannelLayout::mono(), source.frames()};
    for (std::size_t index = 0; index < 3; ++index) {
        MultibandSettings soloed = settings;
        soloed.bands[index].solo = true;

        AudioBuffer part = copyOf(source);
        const Result<MultibandResult> partResult = compressMultiband(part.view(), kRate, soloed);
        REQUIRE(partResult);

        // A soloed band still does exactly what it did in the full mix.
        REQUIRE(partResult.value().gainReductionDb[index] ==
                wholeResult.value().gainReductionDb[index]);
        // The bands nobody is listening to are not compressed, so they report
        // nothing rather than a figure about audio that was thrown away.
        for (std::size_t other = 0; other < 3; ++other) {
            if (other != index) {
                REQUIRE(partResult.value().gainReductionDb[other] == 0.0);
            }
        }
        // A soloed band is a part of the signal, not the whole of it.
        REQUIRE_FALSE(identical(part, whole));

        for (SampleCount i = 0; i < source.frames(); ++i) {
            rebuilt.channel(0)[i] += part.channel(0)[i];
        }
    }

    REQUIRE(identical(rebuilt, whole));
}

TEST_CASE("Bypass takes a band's compressor out and leaves the band itself in") {
    const AudioBuffer source = noise(48000, 1, 0.5, 4242u);

    CompressorSettings crushing;
    crushing.thresholdDb = -60.0;
    crushing.ratio = 20.0;
    crushing.attackSeconds = 0.001;
    crushing.releaseSeconds = 0.050;
    // On the bypass run this must not be applied either: bypass means the band
    // was not touched, not that it was compressed by nothing and then lifted.
    crushing.makeupGainDb = 6.0;

    MultibandSettings settings;
    settings.crossoverHz = {300.0, 3000.0};
    settings.bands = bandsOf(3, true);
    settings.bands[1].compressor = crushing;

    AudioBuffer active = copyOf(source);
    settings.bands[1].bypass = false;
    const Result<MultibandResult> activeResult = compressMultiband(active.view(), kRate, settings);
    REQUIRE(activeResult);
    REQUIRE(activeResult.value().gainReductionDb[1] > 5.0);

    AudioBuffer bypassed = copyOf(source);
    settings.bands[1].bypass = true;
    const Result<MultibandResult> bypassedResult =
        compressMultiband(bypassed.view(), kRate, settings);
    REQUIRE(bypassedResult);
    REQUIRE(bypassedResult.value().gainReductionDb[1] == 0.0);

    // With every band bypassed the output is the all-pass chain and nothing
    // else. This run has a 20:1 compressor and 6 dB of makeup configured on the
    // middle band and is still exactly that, so the bypass took out the makeup
    // as well -- 6 dB is six hundred times the residual it would have to hide
    // inside.
    AudioBuffer reference = copyOf(source);
    applyReferenceAllPasses(reference.view(), settings.crossoverHz, settings.order);
    REQUIRE(worstDifference(bypassed, reference) < 1e-6);
    REQUIRE_FALSE(identical(active, bypassed));
}

TEST_CASE("A signal that never crosses a threshold comes out as though bypassed") {
    // -40 dBFS against a -10 dB threshold with a 6 dB knee, whose lower edge is
    // at -13: nothing reaches the bend. The gain computer then returns 0 dB,
    // decibelsToGain(0) is exactly 1, and multiplying a float by exactly 1
    // returns that float -- so this is not "close to untouched", it is the same
    // samples as the bypassed run, bit for bit.
    const AudioBuffer source = tones(48000, {{120.0, 0.01}, {1200.0, 0.01}, {9000.0, 0.01}});

    CompressorSettings compressor;
    compressor.thresholdDb = -10.0;
    compressor.ratio = 8.0;
    compressor.kneeDb = 6.0;

    MultibandSettings settings;
    settings.bands = bandsOf(settings.crossoverHz.size() + 1);
    for (MultibandBandSettings& band : settings.bands) {
        band.compressor = compressor;
    }

    AudioBuffer quiet = copyOf(source);
    const Result<MultibandResult> result = compressMultiband(quiet.view(), kRate, settings);
    REQUIRE(result);
    for (const double reduction : result.value().gainReductionDb) {
        REQUIRE(reduction == 0.0);
    }

    MultibandSettings bypassed = settings;
    for (MultibandBandSettings& band : bypassed.bands) {
        band.bypass = true;
    }
    AudioBuffer untouched = copyOf(source);
    REQUIRE(compressMultiband(untouched.view(), kRate, bypassed));

    REQUIRE(identical(quiet, untouched));
}

TEST_CASE("Makeup gain lifts its own band and nothing else") {
    // The band's gain, not the mix's: two bands, makeup on one of them, and the
    // other band's tone must sit where it was.
    constexpr int kSize = 16384;
    constexpr int kLowBin = 20;    // 58.59375 Hz
    constexpr int kHighBin = 2048; // 6000 Hz exactly
    constexpr double kBinHz = 48000.0 / static_cast<double>(kSize);

    const auto run = [&](double makeupGainDb) {
        AudioBuffer audio = tones(65536, {{kLowBin * kBinHz, 0.05}, {kHighBin * kBinHz, 0.05}});
        MultibandSettings settings;
        settings.crossoverHz = {600.0};
        settings.bands = bandsOf(2);
        // Well below anything here, so the only thing moving is the makeup.
        for (MultibandBandSettings& band : settings.bands) {
            band.compressor.thresholdDb = 0.0;
            band.compressor.kneeDb = 0.0;
        }
        settings.bands[0].compressor.makeupGainDb = makeupGainDb;
        REQUIRE(compressMultiband(audio.view(), kRate, settings));
        return audio;
    };

    const AudioBuffer flat = run(0.0);
    const AudioBuffer lifted = run(6.0);

    REQUIRE(binLevelDb(lifted, kSize, kLowBin) - binLevelDb(flat, kSize, kLowBin) ==
            Approx(6.0).margin(0.01));
    REQUIRE(binLevelDb(lifted, kSize, kHighBin) - binLevelDb(flat, kSize, kHighBin) ==
            Approx(0.0).margin(0.01));
}

TEST_CASE("A stereo pair is split and compressed without the image moving") {
    // The linked compressor already has its own tests; what is checked here is
    // that a band of a linked pair is still one band -- that both channels come
    // out with the same gain applied, so nothing has been split per channel
    // along the way.
    constexpr SampleCount kFrames = 96000;
    AudioBuffer audio{ChannelLayout::stereo(), kFrames};
    for (SampleCount i = 0; i < kFrames; ++i) {
        const double ramp = i < 4800 ? static_cast<double>(i) / 4800.0 : 1.0;
        const auto value = static_cast<float>(
            ramp * 0.5 *
            std::sin(2.0 * std::numbers::pi * 707.10678 * static_cast<double>(i) / kRate.hz()));
        audio.channel(0)[i] = value;
        // Half the amplitude on the right: a fixed 6 dB of image offset, which
        // a linked pair must preserve exactly.
        audio.channel(1)[i] = 0.5f * value;
    }

    MultibandSettings settings;
    settings.crossoverHz = {50.0, 10000.0};
    settings.bands = bandsOf(3);
    for (MultibandBandSettings& band : settings.bands) {
        band.compressor.thresholdDb = -20.0;
        band.compressor.ratio = 4.0;
        band.compressor.kneeDb = 0.0;
        band.compressor.attackSeconds = 0.0002;
        band.compressor.releaseSeconds = 5.0;
    }

    const Result<MultibandResult> result = compressMultiband(audio.view(), kRate, settings);
    REQUIRE(result);
    // Something has to have happened, or the offset would be preserved by a
    // processor that did nothing at all.
    REQUIRE(result.value().gainReductionDb[1] > 5.0);

    const double left = decibels(rmsOf(audio, 72000, 24000, 0));
    const double right = decibels(rmsOf(audio, 72000, 24000, 1));
    REQUIRE(left - right == Approx(decibels(2.0)).margin(0.01));
}

TEST_CASE("A crossover list that is not strictly increasing is refused, not repaired") {
    AudioBuffer audio = noise(4800, 1, 0.5, 1u);

    const auto refuse = [&](const std::vector<double>& crossovers) {
        MultibandSettings settings;
        settings.crossoverHz = crossovers;
        settings.bands = bandsOf(crossovers.size() + 1, true);
        const Result<MultibandResult> result = compressMultiband(audio.view(), kRate, settings);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code() == ErrorCode::InvalidArgument);
    };

    refuse({2000.0, 200.0});         // Out of order.
    refuse({200.0, 200.0});          // Duplicated.
    refuse({200.0, 2000.0, 1000.0}); // Out of order further along.
    refuse({24000.0});               // Exactly Nyquist.
    refuse({30000.0});               // Past Nyquist.
    refuse({-200.0});                // Negative.
    refuse({0.0});                   // Zero.
    refuse({200.0, 2000.0, -50.0});  // Negative further along.
    refuse({std::nan("")});          // Not a number.
    refuse({std::numeric_limits<double>::infinity()});
    refuse({}); // No split at all.
}

TEST_CASE("A band count that does not match the crossover count is refused") {
    AudioBuffer audio = noise(4800, 1, 0.5, 1u);

    MultibandSettings settings;
    settings.crossoverHz = {200.0, 2000.0};

    settings.bands = bandsOf(2, true);
    REQUIRE_FALSE(compressMultiband(audio.view(), kRate, settings));

    settings.bands = bandsOf(4, true);
    REQUIRE_FALSE(compressMultiband(audio.view(), kRate, settings));

    settings.bands.clear();
    REQUIRE_FALSE(compressMultiband(audio.view(), kRate, settings));

    settings.bands = bandsOf(3, true);
    REQUIRE(compressMultiband(audio.view(), kRate, settings));
}

TEST_CASE("An order that is not a multiple of four is refused rather than rounded") {
    AudioBuffer audio = noise(4800, 1, 0.5, 1u);

    const auto attempt = [&](int order) {
        MultibandSettings settings;
        settings.crossoverHz = {1000.0};
        settings.bands = bandsOf(2, true);
        settings.order = order;
        return compressMultiband(audio.view(), kRate, settings);
    };

    // 2 and 6 are real Linkwitz-Riley orders and are still refused: both are a
    // Butterworth of odd order squared, which needs a first-order section the
    // cookbook set does not produce, and both sum with a polarity flip that the
    // rest of this design does not carry.
    REQUIRE_FALSE(attempt(2));
    REQUIRE_FALSE(attempt(6));
    REQUIRE_FALSE(attempt(0));
    REQUIRE_FALSE(attempt(-4));
    REQUIRE_FALSE(attempt(3));
    REQUIRE_FALSE(attempt(kMaxCrossoverOrder + 4));
    REQUIRE(attempt(4));
    REQUIRE(attempt(8));
    REQUIRE(attempt(kMaxCrossoverOrder));
}

TEST_CASE("Bad compressor settings are refused even on a band that is bypassed") {
    AudioBuffer audio = noise(4800, 1, 0.5, 1u);

    MultibandSettings settings;
    settings.crossoverHz = {1000.0};
    settings.bands = bandsOf(2, true);
    settings.bands[1].compressor.ratio = 0.5; // Below 1: an expander, not a ratio.

    REQUIRE_FALSE(compressMultiband(audio.view(), kRate, settings));

    settings.bands[1].compressor.ratio = 4.0;
    settings.bands[0].compressor.attackSeconds = -1.0;
    REQUIRE_FALSE(compressMultiband(audio.view(), kRate, settings));
}

TEST_CASE("A run-up past the end is refused, and an over-long blend is clamped") {
    AudioBuffer audio = noise(4800, 1, 0.5, 1u);

    MultibandSettings settings;
    settings.crossoverHz = {1000.0};
    settings.bands = bandsOf(2, true);

    settings.runUp = 9600;
    REQUIRE_FALSE(compressMultiband(audio.view(), kRate, settings));

    settings.runUp = -1;
    REQUIRE_FALSE(compressMultiband(audio.view(), kRate, settings));

    settings.runUp = 0;
    settings.blend = -1;
    REQUIRE_FALSE(compressMultiband(audio.view(), kRate, settings));

    // A blend longer than the region it has to fit in is the one of the three
    // that is clamped rather than refused, because that is what compressOffline
    // does with the same number and two answers to one question would be worse
    // than either.
    settings.blend = 100000;
    REQUIRE(compressMultiband(audio.view(), kRate, settings));
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        REQUIRE(std::isfinite(audio.channel(0)[i]));
    }
}

TEST_CASE("An unusable sample rate is refused") {
    AudioBuffer audio = noise(4800, 1, 0.5, 1u);
    MultibandSettings settings;
    settings.crossoverHz = {1000.0};
    settings.bands = bandsOf(2, true);

    REQUIRE_FALSE(compressMultiband(audio.view(), SampleRate{0.0}, settings));
    REQUIRE_FALSE(compressMultiband(audio.view(), SampleRate{-48000.0}, settings));
}

TEST_CASE("An empty buffer reports a figure per band and does nothing") {
    AudioBuffer audio{ChannelLayout::stereo(), 0};
    MultibandSettings settings;
    const Result<MultibandResult> result = compressMultiband(audio.view(), kRate, settings);
    REQUIRE(result);
    REQUIRE(result.value().gainReductionDb.size() == 4);
    for (const double reduction : result.value().gainReductionDb) {
        REQUIRE(reduction == 0.0);
    }
}

TEST_CASE("A run-up leaves the compressor already working at the first sample kept") {
    // The reason OfflineDynamics has a run-up, checked through the band split:
    // without one the passage opens with whatever the uncompressed band was
    // doing, for the length of the attack.
    constexpr SampleCount kRunUp = 24000;
    constexpr SampleCount kBody = 24000;
    constexpr double kToneHz = 707.10678;

    CompressorSettings compressor;
    compressor.thresholdDb = -20.0;
    compressor.ratio = 4.0;
    compressor.kneeDb = 0.0;
    compressor.attackSeconds = 0.050;
    compressor.releaseSeconds = 0.200;

    // A one-millisecond fade rather than the usual hundred: the point of this
    // test is what the compressor is doing ten milliseconds in, and a fade
    // longer than the attack would have settled the envelope before the
    // measurement began.
    const auto run = [&](SampleCount runUp, SampleCount frames) {
        AudioBuffer audio = tones(frames, {{kToneHz, 0.5}}, 1, 48);
        MultibandSettings settings;
        settings.crossoverHz = {50.0, 10000.0};
        settings.bands = bandsOf(3);
        for (MultibandBandSettings& band : settings.bands) {
            band.compressor = compressor;
        }
        settings.runUp = runUp;
        REQUIRE(compressMultiband(audio.view(), kRate, settings));
        return audio;
    };

    const AudioBuffer warmed = run(kRunUp, kRunUp + kBody);
    const AudioBuffer cold = run(0, kBody);

    // Over ten milliseconds against a 50 ms attack a cold compressor has
    // reached about a fifth of its travel, so it is still passing some 8 dB
    // more of the band than a warmed one holding the full 10.5 dB down.
    const double coldDb = decibels(rmsOf(cold, 48, 480));
    const double warmedDb = decibels(rmsOf(warmed, kRunUp, 480));
    CAPTURE(coldDb, warmedDb);
    REQUIRE(coldDb > warmedDb + 3.0);
}

TEST_CASE("The edges of a multiband selection are blended, so the sum does not step") {
    // Applied per band with one curve, which by linearity is the same signal as
    // blending the sum, so the seam is measured on the sum.
    //
    // What it is measured *against* needs saying. The blend crossfades each
    // band towards that band before its compressor, so the sum crossfades
    // towards the sum of the uncompressed bands -- which is the input through
    // the all-pass chain, not the input. So the untouched side of this seam is
    // a fully bypassed run, and a caller splicing a compressed selection into
    // genuinely untouched audio still meets the all-pass phase step at the
    // join. That step is inherent to splitting and no blend inside the
    // processor can remove it.
    constexpr SampleCount kRunUp = 24000 + 17;
    constexpr SampleCount kBody = 24000;
    constexpr SampleCount kBlend = 2400;
    const double kToneHz = 707.10678;
    const std::vector<double> kCrossovers = {50.0, 10000.0};

    CompressorSettings compressor;
    compressor.thresholdDb = -30.0;
    compressor.ratio = 8.0;
    compressor.kneeDb = 0.0;
    compressor.attackSeconds = 0.001;
    compressor.releaseSeconds = 0.010;

    const auto process = [&](SampleCount blend, bool bypass) {
        AudioBuffer audio = tones(kRunUp + kBody, {{kToneHz, 0.5}});
        MultibandSettings settings;
        settings.crossoverHz = kCrossovers;
        settings.bands = bandsOf(3, bypass);
        for (MultibandBandSettings& band : settings.bands) {
            band.compressor = compressor;
        }
        settings.runUp = kRunUp;
        settings.blend = blend;
        REQUIRE(compressMultiband(audio.view(), kRate, settings));
        return audio;
    };

    const AudioBuffer untouched = process(0, true);

    const auto seamStep = [&](SampleCount blend) {
        const AudioBuffer audio = process(blend, false);
        double worst = 0.0;
        for (SampleCount i = 0; i < 64; ++i) {
            const SampleIndex at = kRunUp + i;
            const double before =
                at == kRunUp ? untouched.channel(0)[at - 1] : audio.channel(0)[at - 1];
            worst = std::max(worst, std::abs(static_cast<double>(audio.channel(0)[at]) - before));
        }
        return worst;
    };

    // The step the material makes on its own, for scale.
    double natural = 0.0;
    for (SampleCount i = 1; i < 64; ++i) {
        natural =
            std::max(natural, std::abs(static_cast<double>(untouched.channel(0)[kRunUp + i]) -
                                       static_cast<double>(untouched.channel(0)[kRunUp + i - 1])));
    }

    REQUIRE(seamStep(0) > 5.0 * natural);
    REQUIRE(seamStep(kBlend) < 1.2 * natural);
}

TEST_CASE("The reported reduction is what the band's level actually did") {
    // Measured against the band, not against the mix: a tone alone in the
    // middle band, its RMS before and after, and the figure the processor
    // reported for that band.
    constexpr double kToneHz = 707.10678;

    const auto check = [&](double thresholdDb, double ratio) {
        AudioBuffer audio = tones(96000, {{kToneHz, 0.5}});
        const AudioBuffer original = copyOf(audio);

        MultibandSettings settings;
        settings.crossoverHz = {50.0, 10000.0};
        settings.bands = bandsOf(3);
        for (MultibandBandSettings& band : settings.bands) {
            band.compressor.thresholdDb = thresholdDb;
            band.compressor.ratio = ratio;
            band.compressor.kneeDb = 0.0;
            band.compressor.attackSeconds = 0.0002;
            band.compressor.releaseSeconds = 5.0;
        }

        const Result<MultibandResult> result = compressMultiband(audio.view(), kRate, settings);
        REQUIRE(result);

        const double moved =
            decibels(rmsOf(original, 72000, 24000)) - decibels(rmsOf(audio, 72000, 24000));
        CAPTURE(thresholdDb, ratio, moved);
        REQUIRE(result.value().gainReductionDb[1] == Approx(moved).margin(0.02));
        return result.value().gainReductionDb[1];
    };

    // Each of these is the static curve at -6.0206 dBFS: three quarters of the
    // overshoot at 4:1, nine tenths of it at 10:1, half of it at 2:1, and none
    // of it at 1:1 however far over the threshold the signal is.
    REQUIRE(check(-20.0, 4.0) == Approx(10.4846).margin(0.02));
    REQUIRE(check(-30.0, 10.0) == Approx(21.5815).margin(0.02));
    REQUIRE(check(-12.0, 2.0) == Approx(2.9897).margin(0.02));
    REQUIRE(check(-40.0, 1.0) == 0.0);
}
