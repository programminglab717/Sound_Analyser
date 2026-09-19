#include <sa/analysis/NullTest.h>
#include <sa/analysis/RoomAcoustics.h>
#include <sa/analysis/SweepMeasurement.h>
#include <sa/dsp/Fft.h>
#include <sa/dsp/Stft.h>
#include <sa/spectral/Dereverb.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::spectral;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};

/// Envelope time constant for a decay whose T60 is known.
///
/// An envelope exp(-t/tau) carries energy exp(-2t/tau), which falls 60 dB after
/// 6.9078*tau. The same arithmetic the repair itself is built on, written out
/// here so the tests do not have to trust the implementation's version of it.
[[nodiscard]] double tauForT60(double t60Seconds) {
    return t60Seconds * std::log10(std::numbers::e) * 2.0 * 10.0 / 60.0;
}

/// Convolution through the transform, because the direct sum is not affordable.
///
/// A three second sweep through a 1.2 second room is 8e9 multiplies done
/// directly, which is minutes per test case under the sanitiser.
[[nodiscard]] std::vector<float> convolve(const float* a, SampleCount aCount, const float* b,
                                          SampleCount bCount) {
    const SampleCount total = aCount + bCount - 1;
    int size = 4;
    while (static_cast<SampleCount>(size) < total) {
        size *= 2;
    }
    const dsp::RealFft fft{size};
    const auto bins = static_cast<std::size_t>(fft.binCount());

    std::vector<float> padded(static_cast<std::size_t>(size), 0.0f);
    std::vector<std::complex<float>> spectrumA(bins);
    std::vector<std::complex<float>> spectrumB(bins);

    std::copy_n(a, aCount, padded.begin());
    fft.forward(padded.data(), spectrumA.data());
    std::fill(padded.begin(), padded.end(), 0.0f);
    std::copy_n(b, bCount, padded.begin());
    fft.forward(padded.data(), spectrumB.data());
    for (std::size_t i = 0; i < bins; ++i) {
        spectrumA[i] *= spectrumB[i];
    }

    std::vector<float> out(static_cast<std::size_t>(size));
    fft.inverse(spectrumA.data(), out.data());
    out.resize(static_cast<std::size_t>(total));
    return out;
}

/// A room, idealised the way the repair assumes it: a direct sound followed by
/// an exponentially decaying noise tail.
///
/// `tailLevel` sets how live it is. The whole tail carries roughly
/// tailLevel^2 * rate * tau / 2 times the direct sound's energy, so 0.05 at
/// 48 kHz and T60 = 0.8 s is about seven times as much reverberant energy as
/// direct -- a room nobody would choose to record dialogue in, which is the
/// point.
[[nodiscard]] std::vector<float> smoothRoom(double t60Seconds, double tailLevel,
                                            unsigned seed = 4441u) {
    const double tau = tauForT60(t60Seconds);
    const auto taps = static_cast<std::size_t>(kRate.hz() * t60Seconds * 1.5);
    std::mt19937 engine{seed};
    std::normal_distribution<double> noise{0.0, 1.0};

    std::vector<float> response(taps);
    for (std::size_t i = 0; i < taps; ++i) {
        const double t = static_cast<double>(i) / kRate.hz();
        response[i] = static_cast<float>(tailLevel * std::exp(-t / tau) * noise(engine));
    }
    response[0] += 1.0f;
    return response;
}

/// One hard reflection and nothing else: the case the model does not describe.
[[nodiscard]] std::vector<float> slapback(double delaySeconds, double level) {
    const auto at = static_cast<std::size_t>(kRate.hz() * delaySeconds);
    std::vector<float> response(at + 64, 0.0f);
    response[0] = 1.0f;
    response[at] = static_cast<float>(level);
    return response;
}

/// Material with onsets and gaps: a syllable rate, a moving pitch, harmonics
/// and a little breath.
///
/// Reverberation lives in what happens between events, so material without gaps
/// would hide both the problem and the repair. A held tone is the other
/// interesting case and gets its own test.
[[nodiscard]] AudioBuffer voiceLike(double seconds, unsigned seed = 90210u) {
    const auto frames = static_cast<SampleCount>(seconds * kRate.hz());
    AudioBuffer buffer{ChannelLayout::mono(), frames};
    std::mt19937 engine{seed};
    std::normal_distribution<double> breath{0.0, 1.0};

    double phase = 0.0;
    for (SampleCount i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate.hz();
        const double within = std::fmod(t, 0.30);
        const double edge = 0.02;
        double envelope = 0.0;
        if (within < 0.18) {
            envelope = 1.0;
            if (within < edge) {
                envelope = 0.5 - 0.5 * std::cos(std::numbers::pi * within / edge);
            } else if (within > 0.18 - edge) {
                envelope = 0.5 - 0.5 * std::cos(std::numbers::pi * (0.18 - within) / edge);
            }
        }
        const double f0 = 120.0 + 40.0 * std::sin(2.0 * std::numbers::pi * 0.7 * t);
        phase += 2.0 * std::numbers::pi * f0 / kRate.hz();

        double value = 0.0;
        for (int harmonic = 1; harmonic <= 12; ++harmonic) {
            value += std::sin(phase * harmonic) / harmonic;
        }
        buffer.channel(0)[i] =
            static_cast<float>(envelope * (0.15 * value + 0.01 * breath(engine)));
    }
    return buffer;
}

[[nodiscard]] AudioBuffer copyOf(const AudioBuffer& source) {
    AudioBuffer out{source.layout(), source.frames()};
    for (int channel = 0; channel < source.channelCount(); ++channel) {
        std::copy_n(source.channel(channel), source.frames(), out.channel(channel));
    }
    return out;
}

/// `dry` through `room`, truncated to `frames` so the two stay comparable.
[[nodiscard]] AudioBuffer passThrough(const AudioBuffer& dry, const std::vector<float>& room,
                                      SampleCount frames) {
    const std::vector<float> wet =
        convolve(dry.channel(0), dry.frames(), room.data(), static_cast<SampleCount>(room.size()));
    AudioBuffer out{ChannelLayout::mono(), frames};
    const auto taken = std::min<SampleCount>(frames, static_cast<SampleCount>(wet.size()));
    std::copy_n(wet.begin(), taken, out.channel(0));
    return out;
}

[[nodiscard]] double rms(const AudioBuffer& audio, SampleIndex start, SampleCount length,
                         int channel = 0) {
    const float* samples = audio.channel(channel);
    double total = 0.0;
    for (SampleCount i = 0; i < length; ++i) {
        total += static_cast<double>(samples[start + i]) * samples[start + i];
    }
    return std::sqrt(total / static_cast<double>(length));
}

/// How far the repair's own per-bin gain jumps from frame to frame, in dB RMS,
/// averaged over the bins the input had energy in.
///
/// Spectral subtraction's characteristic failure is a bin whose estimate
/// crosses back and forth over its own magnitude, surviving on one frame and
/// not the next, heard as a shimmer of tones over the quiet parts. The artefact
/// is in the gain rather than in the magnitude, so the gain is what is
/// measured: the processed bin over the same bin of the input. The material's
/// own fluctuation then sits on both sides of the ratio, where it cancels, and
/// what is left is zero for a no-op and grows with exactly the thing being
/// looked for.
[[nodiscard]] double gainJitterDb(const AudioBuffer& input, const AudioBuffer& output,
                                  SampleIndex start, SampleCount length) {
    auto stft = dsp::Stft::create(1024, 256);
    REQUIRE(stft.hasValue());
    const SampleCount frames = stft.value().frameCount(length);
    const auto bins = static_cast<std::size_t>(stft.value().binCount());

    std::vector<std::complex<float>> before(static_cast<std::size_t>(frames) * bins);
    std::vector<std::complex<float>> after(before.size());
    stft.value().analyse(input.channel(0) + start, length, before.data());
    stft.value().analyse(output.channel(0) + start, length, after.data());

    double loudest = 0.0;
    std::vector<double> mean(bins, 0.0);
    for (std::size_t bin = 0; bin < bins; ++bin) {
        double total = 0.0;
        for (SampleCount frame = 0; frame < frames; ++frame) {
            total += std::abs(before[static_cast<std::size_t>(frame) * bins + bin]);
        }
        mean[bin] = total / static_cast<double>(frames);
        loudest = std::max(loudest, mean[bin]);
    }

    // Floored a million below the loudest bin's mean, so a bin pushed towards
    // nothing reports the gain of nothing rather than a ratio of two roundings.
    // Bins the input never excited are left out altogether: a gain measured
    // where there was no signal is a measurement of the window's leakage.
    const double floorMagnitude = loudest * 1e-6;
    double total = 0.0;
    int counted = 0;
    for (std::size_t bin = 0; bin < bins; ++bin) {
        if (mean[bin] < loudest * 0.01) {
            continue;
        }
        double sum = 0.0;
        double previous = 0.0;
        for (SampleCount frame = 0; frame < frames; ++frame) {
            const auto at = static_cast<std::size_t>(frame) * bins + bin;
            const double gain = 20.0 * std::log10((std::abs(after[at]) + floorMagnitude) /
                                                  (std::abs(before[at]) + floorMagnitude));
            if (frame > 0) {
                sum += (gain - previous) * (gain - previous);
            }
            previous = gain;
        }
        total += std::sqrt(sum / static_cast<double>(frames - 1));
        ++counted;
    }
    return counted > 0 ? total / counted : 0.0;
}

[[nodiscard]] analysis::SweepSettings measurementSweep() {
    analysis::SweepSettings settings;
    settings.seconds = 3.0;
    settings.startHz = 50.0;
    settings.endHz = 18000.0;
    return settings;
}

/// Play a sweep through `room`, optionally repair the recording, deconvolve,
/// and measure what a measurement engineer would have measured.
///
/// The repair is time-varying, so what comes back is not a linear impulse
/// response and is not claimed to be. It is the impulse response the room would
/// appear to have if it were measured with this processing in the chain, which
/// is exactly the question being asked.
[[nodiscard]] analysis::RoomAcoustics measureThrough(const std::vector<float>& room,
                                                     const DereverbSettings* settings,
                                                     double keepSeconds) {
    const analysis::SweepSettings sweep = measurementSweep();
    const auto excitation = analysis::generateSweep(kRate, sweep);
    REQUIRE(excitation.hasValue());

    const std::vector<float> played =
        convolve(excitation.value().channel(0), excitation.value().frames(), room.data(),
                 static_cast<SampleCount>(room.size()));

    AudioBuffer recorded{ChannelLayout::mono(), static_cast<SampleCount>(played.size())};
    std::copy(played.begin(), played.end(), recorded.channel(0));

    if (settings != nullptr) {
        REQUIRE(reduceReverb(recorded.view(), kRate, *settings).ok());
    }

    const auto impulse = analysis::deconvolveSweep(recorded.view(), kRate, sweep, keepSeconds);
    REQUIRE(impulse.hasValue());
    const auto measured = analysis::measureRoomAcoustics(impulse.value().view(), kRate);
    REQUIRE(measured.hasValue());
    REQUIRE(measured.value().valid);
    return measured.value();
}

} // namespace

TEST_CASE("A room measured through the repair reads shorter and clearer", "[spectral][dereverb]") {
    // The end-to-end measurement, done the way a room is actually measured:
    // sweep in, deconvolve out, and read the result with the same code that
    // reads a real impulse response.
    const double t60 = 0.8;
    const std::vector<float> room = smoothRoom(t60, 0.05);

    DereverbSettings settings;
    settings.decaySeconds = t60; // The room's own decay, which is the best case.

    const analysis::RoomAcoustics before = measureThrough(room, nullptr, t60 * 1.5);
    const analysis::RoomAcoustics after = measureThrough(room, &settings, t60 * 1.5);

    INFO("T20 " << before.t20Seconds << " -> " << after.t20Seconds);
    INFO("T30 " << before.t30Seconds << " -> " << after.t30Seconds);
    INFO("EDT " << before.earlyDecaySeconds << " -> " << after.earlyDecaySeconds);
    INFO("C50 " << before.clarity50Db << " -> " << after.clarity50Db);
    INFO("D50 " << before.definition50 << " -> " << after.definition50);

    // The room that went in is the room that was asked for, which is what makes
    // any of the after figures mean anything.
    REQUIRE(before.hasT20);
    REQUIRE(before.t20Seconds == Approx(t60).epsilon(0.12));
    REQUIRE(after.hasT20);
    REQUIRE(after.hasT30);

    // Measured over three room seeds, which is what the bounds are sized
    // against rather than one lucky draw:
    //
    //     seed    T20            T30            EDT            C50           D50
    //       11    0.811 -> 0.705 0.803 -> 0.758 0.780 -> 0.493 2.33 -> 4.66  0.631 -> 0.745
    //       29    0.798 -> 0.680 0.800 -> 0.745 0.785 -> 0.494 2.37 -> 4.75  0.633 -> 0.749
    //     4441    0.810 -> 0.707 0.803 -> 0.751 0.799 -> 0.492 2.56 -> 5.06  0.643 -> 0.762
    //
    // The spread between the measures is the interesting part and it is not an
    // accident. A subtraction that takes a fixed proportion of the late energy
    // scales the decay curve downwards without tilting it: past the early/late
    // split, the processed curve is the original curve minus a constant, so its
    // slope -- which is what T20 and T30 are -- is very nearly unchanged. What
    // moves the slope figures at all is the step at the split itself, which
    // lives inside the first ten decibels and so inside EDT, and which drags
    // the -5 dB end of the T20 and T30 fits earlier. The energy ratios have no
    // such problem, because moving energy from after 50 ms to before it is
    // exactly what C50 and D50 measure.
    //
    // So EDT falls 38%, T20 13% and T30 6%, and a de-reverb of this kind is
    // honestly judged on EDT, C50 and D50 rather than on T30.
    CHECK(after.t20Seconds < before.t20Seconds * 0.90);
    CHECK(after.t30Seconds < before.t30Seconds * 0.95);
    CHECK(after.earlyDecaySeconds < before.earlyDecaySeconds * 0.70);
    CHECK(after.clarity50Db > before.clarity50Db + 2.0);
    CHECK(after.definition50 > before.definition50 + 0.09);
}

TEST_CASE("De-reverberated material sits closer to the dry original", "[spectral][dereverb]") {
    // The other way of measuring the same repair, and the one that answers the
    // question a user actually has: is this nearer to the take they wanted.
    const double t60 = 1.5;
    const std::vector<float> room = smoothRoom(t60, 0.02);
    const AudioBuffer dry = voiceLike(3.0);
    const AudioBuffer wet = passThrough(dry, room, dry.frames());
    AudioBuffer repaired = copyOf(wet);

    DereverbSettings settings;
    settings.decaySeconds = t60;
    REQUIRE(reduceReverb(repaired.view(), kRate, settings).ok());

    // Gain matching off, which is the narrower question and the right one here.
    // With it on the fit scales the reverberant take down until the residual is
    // mostly the dry signal itself -- both readings then sit near 0 dB and the
    // measure says nothing at all, which is what it did when this test was
    // first written.
    analysis::NullSettings plain;
    plain.matchGain = false;

    const auto before = analysis::nullTest(dry.view(), wet.view(), kRate, plain);
    const auto after = analysis::nullTest(dry.view(), repaired.view(), kRate, plain);
    REQUIRE(before.hasValue());
    REQUIRE(after.hasValue());
    REQUIRE(before.value().valid);
    REQUIRE(after.value().valid);

    INFO("difference from dry " << before.value().residualDb << " -> " << after.value().residualDb
                                << " dB");

    // Measured 2.04 dB -> -0.48 dB, so the difference from the dry take is
    // 2.5 dB smaller. It is bounded and it is not large: most of what separates
    // a reverberant take from a dry one is the first 85 ms, and that is exactly
    // what lateOnsetSeconds leaves alone. Shorter rooms give less -- 1.4 dB at
    // T60 0.8 -- because a shorter tail keeps more of its energy inside that
    // window.
    CHECK(after.value().residualDb < before.value().residualDb - 2.0);
}

TEST_CASE("Dry material is left nearly alone", "[spectral][dereverb]") {
    // The test that stops this being a downward expander wearing a de-reverb's
    // name. Run on material with no reverberation at all, it must change almost
    // nothing -- and "almost" has to be a number.
    const AudioBuffer dry = voiceLike(3.0);
    AudioBuffer processed = copyOf(dry);

    REQUIRE(reduceReverb(processed.view(), kRate, DereverbSettings{}).ok());

    const auto residual = analysis::nullTest(dry.view(), processed.view(), kRate);
    REQUIRE(residual.hasValue());
    REQUIRE(residual.value().valid);

    const double levelChange =
        20.0 * std::log10(rms(processed, 0, processed.frames()) / rms(dry, 0, dry.frames()));

    // The gain the repair applied over one syllable, which is a different
    // question from how much came off: a steady 0.5 dB of loss is inaudible and
    // half a decibel of it moving about is not.
    const double jitter = gainJitterDb(dry, processed, static_cast<SampleIndex>(1.22 * kRate.hz()),
                                       static_cast<SampleCount>(0.12 * kRate.hz()));

    INFO("residual " << residual.value().residualDb << " dB, level " << levelChange
                     << " dB, gain jitter " << jitter << " dB");

    // Measured: -0.55 dB of level, a residual 20.96 dB below the material, and
    // 0.54 dB RMS of frame-to-frame gain movement. Not nothing -- a syllable's
    // own decay looks exactly like a room's, so some of it comes off -- but
    // small enough that a take which turned out not to need this is not
    // damaged by having had it. At 43 ms of lateOnsetSeconds the same material
    // reads -1.60 dB and -17.4 dB, which is why the default is not 43 ms.
    CHECK(levelChange < 0.0);
    CHECK(levelChange > -0.8);
    CHECK(residual.value().residualDb < -19.0);
    CHECK(jitter < 1.0);
}

TEST_CASE("A held tone is where the model costs the most", "[spectral][dereverb]") {
    // Stated plainly because it is inherent rather than incidental: a steady
    // tone is indistinguishable from its own tail. The energy 85 ms ago looks
    // identical either way, so the estimate says some of the tone is
    // reverberation and some of the tone comes off. Nothing built on this model
    // avoids that; what can be done is to bound it and say so.
    //
    // The bite is exactly the fraction of a room's energy the model says
    // arrives after the split, so it grows with decaySeconds and is the reason
    // a wildly over-stated decay setting sounds like a gate. Measured on a
    // 440 Hz tone at the defaults:
    //
    //     decaySeconds   level
    //        0.4 s       -0.21 dB
    //        0.8 s       -1.00 dB
    //        1.5 s       -2.29 dB
    const auto frames = static_cast<SampleCount>(2.0 * kRate.hz());
    AudioBuffer tone{ChannelLayout::mono(), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        tone.channel(0)[i] = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRate.hz()));
    }

    double previous = 0.0;
    for (const double decay : {0.4, 0.8, 1.5}) {
        AudioBuffer processed = copyOf(tone);
        DereverbSettings settings;
        settings.decaySeconds = decay;
        REQUIRE(reduceReverb(processed.view(), kRate, settings).ok());

        // Well inside the tone, clear of both ends.
        const double change =
            20.0 * std::log10(rms(processed, 24000, 48000) / rms(tone, 24000, 48000));
        INFO("decay " << decay << " costs " << change << " dB");
        CHECK(change < previous);
        previous = change;
    }
    CHECK(previous > -3.0);
}

TEST_CASE("Asking for no reduction is the round trip and nothing else", "[spectral][dereverb]") {
    // Zero reduction multiplies every bin by exactly one, so what is left is
    // the analysis/synthesis round trip's own error and nothing of this file's.
    // It is measured rather than asserted away: it is the error floor every
    // other figure in these tests sits on.
    const AudioBuffer dry = voiceLike(1.5);
    AudioBuffer processed = copyOf(dry);

    DereverbSettings settings;
    settings.reductionDb = 0.0;
    REQUIRE(reduceReverb(processed.view(), kRate, settings).ok());

    double worst = 0.0;
    double error = 0.0;
    double signal = 0.0;
    for (SampleCount i = 0; i < dry.frames(); ++i) {
        const double difference = static_cast<double>(processed.channel(0)[i]) - dry.channel(0)[i];
        worst = std::max(worst, std::abs(difference));
        error += difference * difference;
        signal += static_cast<double>(dry.channel(0)[i]) * dry.channel(0)[i];
    }
    const double errorDb = 10.0 * std::log10(error / signal);
    INFO("round trip " << errorDb << " dB, worst sample " << worst);

    // Measured at -137.5 dB relative to the material, worst single sample
    // 8.9e-08 against a signal peaking near 0.3. That is float rounding through
    // a Hann analysis and a normalised overlap-add and nothing else, and it is
    // the sense in which every "no-op" claim below is meant.
    CHECK(errorDb < -130.0);
    CHECK(worst < 1e-6);
}

TEST_CASE("More reduction removes more", "[spectral][dereverb]") {
    const double t60 = 1.0;
    const std::vector<float> room = smoothRoom(t60, 0.03);
    const AudioBuffer dry = voiceLike(3.0);
    const AudioBuffer wet = passThrough(dry, room, dry.frames());

    // Between syllables: the voiced stretches run 0.30k to 0.30k + 0.18, so
    // nothing is being said here and everything still sounding is the room.
    const auto gapStart = static_cast<SampleIndex>(1.40 * kRate.hz());
    const auto gapLength = static_cast<SampleCount>(0.08 * kRate.hz());
    REQUIRE(rms(dry, gapStart, gapLength) == 0.0);

    double previous = 1.0;
    for (const double reduction : {0.0, 3.0, 6.0, 10.0, 20.0}) {
        AudioBuffer processed = copyOf(wet);
        DereverbSettings settings;
        settings.decaySeconds = t60;
        settings.reductionDb = reduction;
        REQUIRE(reduceReverb(processed.view(), kRate, settings).ok());

        const double level =
            20.0 * std::log10(rms(processed, gapStart, gapLength) / rms(wet, gapStart, gapLength));
        INFO("reduction " << reduction << " dB gives " << level << " dB in the gap");
        CHECK(level < previous);
        previous = level;
    }
    // Measured 0.00, -2.11, -2.97, -3.36, -3.56 dB. Monotone, and flattening:
    // past about 10 dB the estimate is already removing all of what it believes
    // is there and the floor is holding up the rest, so the control runs out of
    // road. That is a property worth knowing rather than a defect -- turning it
    // up further buys artefacts and not reduction.
    CHECK(previous > -6.0);
}

TEST_CASE("The gain does not flicker, and the floor and the smoothing are why",
          "[spectral][dereverb]") {
    // Spectral subtraction's characteristic failure: isolated bins that survive
    // the subtraction on one frame and not the next, heard as a shimmer of
    // tones over the quiet parts. Measured in the gap between syllables, where
    // the repair is working hardest and the artefact would be most exposed.
    const double t60 = 1.0;
    const std::vector<float> room = smoothRoom(t60, 0.03);
    const AudioBuffer dry = voiceLike(3.0);
    const AudioBuffer wet = passThrough(dry, room, dry.frames());
    AudioBuffer guarded = copyOf(wet);
    AudioBuffer bare = copyOf(wet);

    DereverbSettings settings;
    settings.decaySeconds = t60;
    REQUIRE(reduceReverb(guarded.view(), kRate, settings).ok());

    settings.floorDb = -60.0;
    settings.timeSmoothing = 0.0;
    settings.frequencySmoothingBins = 0;
    REQUIRE(reduceReverb(bare.view(), kRate, settings).ok());

    const auto gapStart = static_cast<SampleIndex>(1.40 * kRate.hz());
    const auto gapLength = static_cast<SampleCount>(0.08 * kRate.hz());

    const double guardedJitter = gainJitterDb(wet, guarded, gapStart, gapLength);
    const double bareJitter = gainJitterDb(wet, bare, gapStart, gapLength);
    const double guardedLevel =
        20.0 * std::log10(rms(guarded, gapStart, gapLength) / rms(wet, gapStart, gapLength));
    const double bareLevel =
        20.0 * std::log10(rms(bare, gapStart, gapLength) / rms(wet, gapStart, gapLength));

    INFO("guarded " << guardedLevel << " dB, jitter " << guardedJitter << " dB");
    INFO("bare " << bareLevel << " dB, jitter " << bareJitter << " dB");

    // Measured: guarded removes 3.36 dB with 2.04 dB RMS of gain movement per
    // frame; bare removes 2.17 dB with 3.68 dB. The defaults are better on both
    // counts at once, which is the case for them: letting a bin fall to nothing
    // does not remove more reverberation, it removes the same reverberation
    // more raggedly and puts a hole where the bin was.
    //
    // What is bounded here is the gain's own movement, which is the artefact.
    // Whether 2 dB RMS at a 10.7 ms hop is audible on any given material is not
    // something this measures and is not claimed.
    CHECK(guardedJitter < 2.5);
    CHECK(bareJitter > guardedJitter * 1.5);
    CHECK(guardedLevel < bareLevel);
}

TEST_CASE("A discrete reflection is not what this repairs", "[spectral][dereverb]") {
    // Said plainly because it is the honest limit of the model. A single hard
    // reflection is not the average of anything, and whether it is touched at
    // all comes down to whether it happens to land past lateOnsetSeconds.
    const auto sweep = measurementSweep();
    const auto excitation = analysis::generateSweep(kRate, sweep);
    REQUIRE(excitation.hasValue());

    // 30 ms lands inside the early window, 90 ms just past it.
    for (const double delay : {0.03, 0.09}) {
        const std::vector<float> room = slapback(delay, 0.5);
        const std::vector<float> played =
            convolve(excitation.value().channel(0), excitation.value().frames(), room.data(),
                     static_cast<SampleCount>(room.size()));
        AudioBuffer recorded{ChannelLayout::mono(), static_cast<SampleCount>(played.size())};
        std::copy(played.begin(), played.end(), recorded.channel(0));

        REQUIRE(reduceReverb(recorded.view(), kRate, DereverbSettings{}).ok());

        const auto impulse = analysis::deconvolveSweep(recorded.view(), kRate, sweep, 0.2);
        REQUIRE(impulse.hasValue());

        const auto at = static_cast<SampleIndex>(delay * kRate.hz());
        double direct = 0.0;
        for (SampleIndex i = 0; i < 8; ++i) {
            direct = std::max(direct, std::abs(static_cast<double>(impulse.value().channel(0)[i])));
        }
        double echo = 0.0;
        for (SampleIndex i = at - 8; i < at + 8; ++i) {
            echo = std::max(echo, std::abs(static_cast<double>(impulse.value().channel(0)[i])));
        }
        const double ratioDb = 20.0 * std::log10(echo / direct);
        INFO("delay " << delay << " leaves the echo at " << ratioDb << " dB, was -6.02 dB");

        // Both went in at -6.02 dB. The 30 ms one comes back at -6.06 dB, which
        // is untouched; the 90 ms one at -9.29 dB, which is three decibels off
        // an echo that is still plainly an echo. Neither is a repair, and a
        // header implying otherwise would be false.
        if (delay < 0.05) {
            CHECK(ratioDb > -6.5);
        } else {
            CHECK(ratioDb > -10.0);
            CHECK(ratioDb < -7.0);
        }
    }
}

TEST_CASE("Each channel is repaired from its own history", "[spectral][dereverb]") {
    const double t60 = 0.8;
    const std::vector<float> left = smoothRoom(t60, 0.05, 11u);
    const std::vector<float> right = smoothRoom(t60, 0.05, 29u);
    const AudioBuffer dry = voiceLike(2.0);

    AudioBuffer stereo{ChannelLayout::stereo(), dry.frames()};
    const AudioBuffer wetLeft = passThrough(dry, left, dry.frames());
    const AudioBuffer wetRight = passThrough(dry, right, dry.frames());
    std::copy_n(wetLeft.channel(0), dry.frames(), stereo.channel(0));
    std::copy_n(wetRight.channel(0), dry.frames(), stereo.channel(1));

    const auto gapStart = static_cast<SampleIndex>(1.40 * kRate.hz());
    const auto gapLength = static_cast<SampleCount>(0.08 * kRate.hz());
    const double leftBefore = rms(stereo, gapStart, gapLength, 0);
    const double rightBefore = rms(stereo, gapStart, gapLength, 1);

    DereverbSettings settings;
    settings.decaySeconds = t60;
    REQUIRE(reduceReverb(stereo.view(), kRate, settings).ok());

    // Two different rooms, so two different reductions: 3.19 dB and 5.43 dB.
    // A shared history would have given one figure for both.
    const double leftChange = 20.0 * std::log10(rms(stereo, gapStart, gapLength, 0) / leftBefore);
    const double rightChange = 20.0 * std::log10(rms(stereo, gapStart, gapLength, 1) / rightBefore);
    INFO("left " << leftChange << " dB, right " << rightChange << " dB");
    CHECK(leftChange < -2.0);
    CHECK(rightChange < -2.0);
    CHECK(std::abs(leftChange - rightChange) > 0.5);
}

TEST_CASE("De-reverb refuses what it cannot do", "[spectral][dereverb]") {
    AudioBuffer audio = voiceLike(0.5);

    CHECK(reduceReverb(audio.view(), kRate, DereverbSettings{}).ok());
    CHECK_FALSE(reduceReverb(AudioBufferView{}, kRate).ok());
    CHECK_FALSE(reduceReverb(audio.view(), SampleRate{0.0}).ok());

    DereverbSettings settings;
    settings.reductionDb = -1.0;
    CHECK_FALSE(reduceReverb(audio.view(), kRate, settings).ok());

    settings = DereverbSettings{};
    settings.decaySeconds = 0.0;
    CHECK_FALSE(reduceReverb(audio.view(), kRate, settings).ok());

    settings = DereverbSettings{};
    settings.floorDb = 1.0;
    CHECK_FALSE(reduceReverb(audio.view(), kRate, settings).ok());

    settings = DereverbSettings{};
    settings.lateOnsetSeconds = -0.1;
    CHECK_FALSE(reduceReverb(audio.view(), kRate, settings).ok());

    // The transform's own contract, checked here only to show it is not
    // bypassed: a size that is not a power of two, and a hop that does not
    // divide the window.
    settings = DereverbSettings{};
    settings.fftSize = 1500;
    CHECK_FALSE(reduceReverb(audio.view(), kRate, settings).ok());

    settings = DereverbSettings{};
    settings.hop = 300;
    CHECK_FALSE(reduceReverb(audio.view(), kRate, settings).ok());

    settings = DereverbSettings{};
    settings.hop = 0;
    CHECK_FALSE(reduceReverb(audio.view(), kRate, settings).ok());

    // Audio too short for a single frame to have a full window of history
    // behind it. There is no tail to estimate, so the repair would be a round
    // trip pretending to be one. The boundary at the defaults falls between
    // 2560 and 2561 samples, which is where the eighth frame -- the first one a
    // delay of eight can read a whole window behind -- appears.
    AudioBuffer brief{ChannelLayout::mono(), 2047};
    CHECK_FALSE(reduceReverb(brief.view(), kRate).ok());
    AudioBuffer edge{ChannelLayout::mono(), 2560};
    CHECK_FALSE(reduceReverb(edge.view(), kRate).ok());
    AudioBuffer justEnough{ChannelLayout::mono(), 2561};
    CHECK(reduceReverb(justEnough.view(), kRate).ok());

    // A late onset shorter than one window is raised to it rather than refused,
    // because a frame that overlaps the one it is correcting would have that
    // frame's own direct sound in its estimate.
    settings = DereverbSettings{};
    settings.lateOnsetSeconds = 0.001;
    CHECK(reduceReverb(audio.view(), kRate, settings).ok());
}
