#include <sa/dsp/OfflineDynamics.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};

/// A tone at a given amplitude, so what a gain stage did to it is a number.
[[nodiscard]] AudioBuffer tone(SampleCount frames, double amplitude, int channels = 1,
                               double hz = 440.0) {
    AudioBuffer audio{ChannelLayout::discrete(channels), frames};
    for (int channel = 0; channel < channels; ++channel) {
        for (SampleCount i = 0; i < frames; ++i) {
            audio.channel(channel)[i] =
                static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz *
                                                        static_cast<double>(i) / kRate.hz()));
        }
    }
    return audio;
}

[[nodiscard]] double peakOf(const AudioBuffer& audio, SampleIndex from, SampleCount count,
                            int channel = 0) {
    double peak = 0.0;
    for (SampleCount i = 0; i < count; ++i) {
        peak = std::max(peak, std::abs(static_cast<double>(audio.channel(channel)[from + i])));
    }
    return peak;
}

[[nodiscard]] double decibels(double linear) {
    return 20.0 * std::log10(std::max(linear, 1e-12));
}

} // namespace

TEST_CASE("A compressor given a run-up is already working at the first sample") {
    // The reason the run-up exists. Without it the passage opens with a burst
    // of the uncompressed signal for the length of the attack.
    constexpr SampleCount kRunUp = 24000;
    constexpr SampleCount kBody = 24000;

    CompressorSettings settings;
    settings.thresholdDb = -20.0;
    settings.ratio = 4.0;
    settings.attackSeconds = 0.050;
    settings.releaseSeconds = 0.200;
    settings.kneeDb = 0.0;

    AudioBuffer with = tone(kRunUp + kBody, 0.5);
    REQUIRE(compressOffline(with.view(), kRate, settings, kRunUp, 0));

    AudioBuffer without = tone(kBody, 0.5);
    REQUIRE(compressOffline(without.view(), kRate, settings, 0, 0));

    // In the first ten milliseconds, the cold start is still passing nearly the
    // whole signal while the warmed one is already holding it down.
    constexpr SampleCount kEarly = 480;
    const double warmed = peakOf(with, kRunUp, kEarly);
    const double cold = peakOf(without, 0, kEarly);

    REQUIRE(cold > warmed * 1.5);
    // And the warmed one is already close to where a steady signal ends up:
    // -20 dB threshold, 4:1, input at -6 dB, so about -16.5 dB out.
    const double settled = decibels(peakOf(with, kRunUp + kBody - kEarly, kEarly));
    REQUIRE(decibels(warmed) == Approx(settled).margin(1.0));
}

TEST_CASE("A compressor holds a steady level to its transfer curve") {
    CompressorSettings settings;
    settings.thresholdDb = -20.0;
    settings.ratio = 4.0;
    settings.attackSeconds = 0.005;
    settings.releaseSeconds = 0.050;
    settings.kneeDb = 0.0;

    // A square wave, not a sine, and the distinction is the point. This
    // compressor detects on sample magnitude, and a sine's magnitude is not
    // steady -- it sweeps to zero and back twice a cycle, so the envelope
    // releases between peaks and settles below them. Measured, a -6 dBFS sine
    // through these settings comes out at -15.4 rather than the -16.5 the
    // static curve gives, and that is the processor being right about a signal
    // whose level really does vary rather than the curve being wrong.
    //
    // A square wave has genuinely constant magnitude, so the detector sees one
    // level and the static curve applies exactly: -6.02 dBFS in, 14 dB over
    // the threshold, out 14/4 = 3.5 dB over it.
    AudioBuffer audio{ChannelLayout::mono(), 48000};
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        audio.channel(0)[i] = (i / 55) % 2 == 0 ? 0.5f : -0.5f;
    }
    REQUIRE(compressOffline(audio.view(), kRate, settings, 24000, 0));

    const double out = decibels(peakOf(audio, 40000, 8000));
    REQUIRE(compressorGainDb(settings, decibels(0.5)) == Approx(-10.5).margin(0.02));
    REQUIRE(out == Approx(decibels(0.5) - 10.5).margin(0.2));
}

TEST_CASE("The edges are blended, so a compressed selection does not step") {
    // The step is at a boundary where the compressor is *engaged*, so this
    // needs the run-up: started cold, the processor applies no gain at the
    // first sample and there is no step there to spread. With a run-up it is
    // already holding the signal down by several decibels when the selection
    // begins, and that is the jump the blend exists for.
    // A quarter period past the half-second mark, so the seam lands on a crest
    // of the 440 Hz tone rather than on a zero crossing. At a crossing every
    // gain gives zero and the largest possible step reads as none, which is how
    // this test first passed while measuring nothing at all.
    constexpr SampleCount kRunUp = 24000 + 27;
    constexpr SampleCount kBody = 24000;
    constexpr SampleCount kBlend = 2400;

    CompressorSettings settings;
    settings.thresholdDb = -30.0;
    settings.ratio = 8.0;
    settings.attackSeconds = 0.001;
    settings.releaseSeconds = 0.010;

    const AudioBuffer original = tone(kRunUp + kBody, 0.5);

    // The seam as the caller will actually hear it: untouched audio up to the
    // selection, processed audio from there on. The measure is the jump across
    // that one join, against the ordinary sample-to-sample step of the same
    // material, which is what makes a number like 0.05 either fine or a click.
    //
    // A single sample would not do. At 440 Hz and 48 kHz the run-up here ends
    // exactly on a zero crossing, where every gain gives zero and a real step
    // reads as none -- which is how this test first passed while measuring
    // nothing.
    const auto seamStep = [&](SampleCount blend) {
        AudioBuffer audio = tone(kRunUp + kBody, 0.5);
        REQUIRE(compressOffline(audio.view(), kRate, settings, kRunUp, blend));

        double worst = 0.0;
        for (SampleCount i = 0; i < 64; ++i) {
            const SampleIndex at = kRunUp + i;
            const double before =
                at == kRunUp ? original.channel(0)[at - 1] : audio.channel(0)[at - 1];
            worst = std::max(worst, std::abs(static_cast<double>(audio.channel(0)[at]) - before));
        }
        return worst;
    };

    // The natural step of the material, for scale.
    double natural = 0.0;
    for (SampleCount i = 1; i < 64; ++i) {
        natural =
            std::max(natural, std::abs(static_cast<double>(original.channel(0)[kRunUp + i]) -
                                       static_cast<double>(original.channel(0)[kRunUp + i - 1])));
    }

    const double abrupt = seamStep(0);
    const double blended = seamStep(kBlend);
    // Unblended, the join jumps far beyond anything the material does on its
    // own -- the full difference between the compressed crest and the crest.
    REQUIRE(abrupt > 5.0 * natural);
    // Blended, it does not: the join is ordinary motion.
    REQUIRE(blended < 1.2 * natural);
}

TEST_CASE("A linked pair applies the same gain to both channels") {
    // The image cannot move if the gain is identical, and it is identical only
    // if the sidechain is shared. A transient on one channel alone is the test.
    constexpr SampleCount kFrames = 24000;
    AudioBuffer audio{ChannelLayout::stereo(), kFrames};
    for (SampleCount i = 0; i < kFrames; ++i) {
        const auto value = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRate.hz()));
        audio.channel(0)[i] = i > kFrames / 2 ? value : 0.05f * value;
        audio.channel(1)[i] = 0.05f * value;
    }
    AudioBuffer original{ChannelLayout::stereo(), kFrames};
    for (int channel = 0; channel < 2; ++channel) {
        std::copy_n(audio.channel(channel), kFrames, original.channel(channel));
    }

    CompressorSettings settings;
    settings.thresholdDb = -20.0;
    settings.ratio = 8.0;
    settings.attackSeconds = 0.001;
    settings.releaseSeconds = 0.100;

    OfflineDynamicsSettings options;
    options.linkStereo = true;
    REQUIRE(compressOffline(audio.view(), kRate, settings, 0, 0, options));

    // The quiet channel must have been pulled down by the loud one's transient.
    const double quietBefore = peakOf(original, 3 * kFrames / 4, kFrames / 8, 1);
    const double quietAfter = peakOf(audio, 3 * kFrames / 4, kFrames / 8, 1);
    REQUIRE(quietAfter < quietBefore * 0.9);
}

TEST_CASE("An unlinked pair leaves the quiet channel alone") {
    constexpr SampleCount kFrames = 24000;
    AudioBuffer audio{ChannelLayout::stereo(), kFrames};
    for (SampleCount i = 0; i < kFrames; ++i) {
        const auto value = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRate.hz()));
        audio.channel(0)[i] = value;
        audio.channel(1)[i] = 0.01f * value;
    }

    CompressorSettings settings;
    settings.thresholdDb = -20.0;
    settings.ratio = 8.0;

    OfflineDynamicsSettings options;
    options.linkStereo = false;
    REQUIRE(compressOffline(audio.view(), kRate, settings, 0, 0, options));

    // 0.5 x 0.01 is -46 dBFS, well under the -20 dB threshold, so an unlinked
    // processor on that channel has nothing to do and must leave it exactly.
    REQUIRE(decibels(peakOf(audio, kFrames / 2, kFrames / 4, 1)) == Approx(-46.02).margin(0.2));
}

TEST_CASE("A gate closes on the noise and opens on the signal") {
    // Half a second of hiss, then half a second of tone over it.
    constexpr SampleCount kHalf = 24000;
    AudioBuffer audio{ChannelLayout::mono(), 2 * kHalf};
    unsigned state = 12345;
    for (SampleCount i = 0; i < 2 * kHalf; ++i) {
        state = 1103515245u * state + 12345u;
        const auto hiss =
            static_cast<float>(0.01 * (static_cast<double>(state % 65536u) / 32768.0 - 1.0));
        const auto note = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRate.hz()));
        audio.channel(0)[i] = i < kHalf ? hiss : hiss + note;
    }

    GateSettings settings;
    settings.thresholdDb = -20.0;
    settings.hysteresisDb = 6.0;
    settings.attackSeconds = 0.001;
    settings.holdSeconds = 0.010;
    settings.releaseSeconds = 0.050;
    settings.rangeDb = -60.0;

    REQUIRE(gateOffline(audio.view(), kRate, settings, 0, 0));

    // The noise-only half is brought right down; the tone is not.
    const double noise = peakOf(audio, kHalf / 2, kHalf / 4);
    const double signal = peakOf(audio, kHalf + kHalf / 2, kHalf / 4);
    REQUIRE(decibels(noise) < -60.0);
    REQUIRE(decibels(signal) == Approx(-6.0).margin(1.0));
}

TEST_CASE("A run-up longer than the buffer is refused rather than read past") {
    AudioBuffer audio = tone(1000, 0.5);
    REQUIRE_FALSE(compressOffline(audio.view(), kRate, CompressorSettings{}, 2000, 0));
    REQUIRE_FALSE(compressOffline(audio.view(), kRate, CompressorSettings{}, -1, 0));
    REQUIRE_FALSE(gateOffline(audio.view(), kRate, GateSettings{}, 0, -1));
}

TEST_CASE("An empty buffer is a no-op") {
    AudioBuffer audio{ChannelLayout::stereo(), 0};
    REQUIRE(compressOffline(audio.view(), kRate, CompressorSettings{}, 0, 0));
    REQUIRE(gateOffline(audio.view(), kRate, GateSettings{}, 0, 0));
}

TEST_CASE("A blend longer than the region is clamped rather than overlapping") {
    AudioBuffer audio = tone(1000, 0.5);
    // Asking for a blend far longer than the audio must not crossfade the same
    // samples twice from both ends.
    REQUIRE(compressOffline(audio.view(), kRate, CompressorSettings{}, 0, 100000));
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        REQUIRE(std::isfinite(audio.channel(0)[i]));
        REQUIRE(std::abs(audio.channel(0)[i]) <= 1.0f);
    }
}

TEST_CASE("The run-up is ten time constants, floored at a fifth of a second") {
    REQUIRE(dynamicsRunUp(kRate, 0.001, 0.001) == 9600);   // The floor: 0.2 s.
    REQUIRE(dynamicsRunUp(kRate, 0.005, 0.100) == 48000);  // 10 x 100 ms.
    REQUIRE(dynamicsRunUp(kRate, 0.500, 0.100) == 240000); // Attack is slower.
}
