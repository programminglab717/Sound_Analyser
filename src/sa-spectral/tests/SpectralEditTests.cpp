#include <sa/spectral/SpectralEdit.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

using namespace sa;
using namespace sa::spectral;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

AudioBuffer makeTones(SampleCount frames, std::initializer_list<std::pair<double, double>> tones,
                      int channels = 1) {
    AudioBuffer buffer{channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo(), frames};
    for (int channel = 0; channel < channels; ++channel) {
        float* out = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            double value = 0.0;
            for (const auto& [hz, amplitude] : tones) {
                value += amplitude *
                         std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate);
            }
            out[i] = static_cast<float>(value);
        }
    }
    return buffer;
}

/// Energy in a frequency band over a sample range, by direct correlation
/// against the tone rather than by another STFT -- a bug in the STFT would
/// otherwise cancel itself out of the measurement.
double toneAmplitude(const AudioBuffer& audio, double hz, SampleIndex start, SampleCount length,
                     int channel = 0) {
    const float* samples = audio.channel(channel);
    double real = 0.0;
    double imaginary = 0.0;
    for (SampleCount i = 0; i < length; ++i) {
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(start + i) / kRate;
        real += samples[start + i] * std::cos(phase);
        imaginary += samples[start + i] * std::sin(phase);
    }
    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(length);
}

double peakDifference(const AudioBuffer& a, const AudioBuffer& b) {
    double worst = 0.0;
    for (int channel = 0; channel < a.layout().count(); ++channel) {
        for (SampleCount i = 0; i < a.frames(); ++i) {
            worst = std::max(worst, std::abs(static_cast<double>(a.channel(channel)[i]) -
                                             static_cast<double>(b.channel(channel)[i])));
        }
    }
    return worst;
}

} // namespace

TEST_CASE("Attenuating a band removes that tone and leaves the others",
          "[spectral][edit][attenuate]") {
    // 400 Hz, 1000 Hz and 4000 Hz together; take out only the middle one.
    AudioBuffer audio = makeTones(96000, {{400.0, 0.3}, {1000.0, 0.3}, {4000.0, 0.3}});

    const double before400 = toneAmplitude(audio, 400.0, 24000, 48000);
    const double before1000 = toneAmplitude(audio, 1000.0, 24000, 48000);
    const double before4000 = toneAmplitude(audio, 4000.0, 24000, 48000);
    REQUIRE(before1000 == Approx(0.3).margin(0.02));

    SpectralRegion region;
    region.startSample = 0;
    region.endSample = 96000;
    region.lowHz = 900.0;
    region.highHz = 1100.0;

    REQUIRE(attenuateRegion(audio.view(), SampleRate{kRate}, region, -80.0).ok());

    const double after400 = toneAmplitude(audio, 400.0, 24000, 48000);
    const double after1000 = toneAmplitude(audio, 1000.0, 24000, 48000);
    const double after4000 = toneAmplitude(audio, 4000.0, 24000, 48000);

    // At least 40 dB down on the target, and the neighbours essentially intact.
    CHECK(after1000 < before1000 * 0.01);
    CHECK(after400 == Approx(before400).margin(0.01));
    CHECK(after4000 == Approx(before4000).margin(0.01));
}

TEST_CASE("Attenuation is confined to the time range it was given", "[spectral][edit][attenuate]") {
    AudioBuffer audio = makeTones(144000, {{1000.0, 0.4}});

    SpectralRegion region;
    region.startSample = 48000;
    region.endSample = 96000;
    region.lowHz = 800.0;
    region.highHz = 1200.0;

    REQUIRE(attenuateRegion(audio.view(), SampleRate{kRate}, region, -80.0).ok());

    // Measured well inside each third so the edges' unavoidable spread -- one
    // analysis window either side -- is not what is being asserted on.
    CHECK(toneAmplitude(audio, 1000.0, 8000, 24000) == Approx(0.4).margin(0.02));
    CHECK(toneAmplitude(audio, 1000.0, 60000, 24000) < 0.01);
    CHECK(toneAmplitude(audio, 1000.0, 112000, 24000) == Approx(0.4).margin(0.02));
}

TEST_CASE("Attenuating by nothing leaves the audio essentially untouched",
          "[spectral][edit][attenuate]") {
    // The round trip through the STFT has to be transparent, or every spectral
    // edit inherits its error as an artefact. This is that gate, applied
    // through the editing path rather than to the STFT directly.
    const AudioBuffer original = makeTones(48000, {{300.0, 0.2}, {2500.0, 0.2}});
    AudioBuffer audio = makeTones(48000, {{300.0, 0.2}, {2500.0, 0.2}});

    SpectralRegion region;
    region.startSample = 8000;
    region.endSample = 32000;
    region.lowHz = 1000.0;
    region.highHz = 1500.0;

    REQUIRE(attenuateRegion(audio.view(), SampleRate{kRate}, region, 0.0).ok());
    CHECK(peakDifference(original, audio) < 1e-4);
}

TEST_CASE("Healing removes a click and keeps the tone running through it",
          "[spectral][edit][heal]") {
    AudioBuffer audio = makeTones(96000, {{1000.0, 0.35}});

    // A broadband click on top of the tone, 200 samples wide.
    for (SampleCount i = 0; i < 200; ++i) {
        audio.channel(0)[48000 + i] += (i % 2 == 0 ? 0.9f : -0.9f);
    }
    const double damagedPeak = std::abs(static_cast<double>(audio.channel(0)[48000]));
    REQUIRE(damagedPeak > 0.89);

    SpectralRegion region;
    region.startSample = 47000;
    region.endSample = 49200;
    region.lowHz = 0.0;
    region.highHz = 24000.0;

    REQUIRE(healRegion(audio.view(), SampleRate{kRate}, region).ok());

    // The click is gone.
    double worst = 0.0;
    for (SampleCount i = 47500; i < 48700; ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(audio.channel(0)[i])));
    }
    CHECK(worst < 0.6);

    // And the tone survived either side of the repair.
    CHECK(toneAmplitude(audio, 1000.0, 8000, 24000) == Approx(0.35).margin(0.03));
    CHECK(toneAmplitude(audio, 1000.0, 60000, 24000) == Approx(0.35).margin(0.03));
}

TEST_CASE("Healing keeps phase continuous across the gap", "[spectral][edit][heal]") {
    // A repair that restarts the phase leaves a step the ear hears as a click,
    // which is the thing being repaired. Compare against the undamaged tone:
    // a phase-continuous fill tracks it, a restarted one does not.
    const AudioBuffer reference = makeTones(96000, {{500.0, 0.4}});
    AudioBuffer audio = makeTones(96000, {{500.0, 0.4}});

    SpectralRegion region;
    region.startSample = 47000;
    region.endSample = 49000;
    region.lowHz = 0.0;
    region.highHz = 24000.0;

    REQUIRE(healRegion(audio.view(), SampleRate{kRate}, region).ok());

    double worst = 0.0;
    for (SampleCount i = 47200; i < 48800; ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(audio.channel(0)[i]) -
                                         static_cast<double>(reference.channel(0)[i])));
    }
    // Measured at 1.8e-7 on a 0.4 amplitude tone -- float32 precision, so the
    // fill is reconstructing the tone rather than approximating it. A restarted
    // phase would give errors up to 0.8, five million times larger, so this
    // bound is both tight and enormously clear of the failure it guards.
    CHECK(worst < 1e-5);
}

TEST_CASE("Spectral edits refuse what they cannot do", "[spectral][edit]") {
    AudioBuffer audio = makeTones(8192, {{1000.0, 0.3}});
    const AudioBufferView view = audio.view();

    SpectralRegion empty;
    CHECK_FALSE(attenuateRegion(view, SampleRate{kRate}, empty, -20.0).ok());

    SpectralRegion inverted;
    inverted.startSample = 4000;
    inverted.endSample = 2000;
    inverted.lowHz = 100.0;
    inverted.highHz = 200.0;
    CHECK_FALSE(attenuateRegion(view, SampleRate{kRate}, inverted, -20.0).ok());

    SpectralRegion outside;
    outside.startSample = 0;
    outside.endSample = 99999;
    outside.lowHz = 100.0;
    outside.highHz = 200.0;
    CHECK_FALSE(attenuateRegion(view, SampleRate{kRate}, outside, -20.0).ok());

    SpectralRegion fine;
    fine.startSample = 1000;
    fine.endSample = 5000;
    fine.lowHz = 500.0;
    fine.highHz = 1500.0;
    CHECK_FALSE(attenuateRegion(view, SampleRate{0.0}, fine, -20.0).ok());

    SpectralEditSettings odd;
    odd.fftSize = 1000; // Not a power of two.
    CHECK_FALSE(attenuateRegion(view, SampleRate{kRate}, fine, -20.0, odd).ok());

    // A region shorter than one analysis frame cannot be healed, and says so
    // rather than producing something arbitrary.
    SpectralRegion sliver;
    sliver.startSample = 2000;
    sliver.endSample = 2003;
    sliver.lowHz = 100.0;
    sliver.highHz = 200.0;
    CHECK_FALSE(healRegion(view, SampleRate{kRate}, sliver).ok());
}

TEST_CASE("Both channels of a stereo edit are treated the same", "[spectral][edit][attenuate]") {
    AudioBuffer audio = makeTones(48000, {{1000.0, 0.3}, {3000.0, 0.3}}, 2);

    SpectralRegion region;
    region.startSample = 0;
    region.endSample = 48000;
    region.lowHz = 2800.0;
    region.highHz = 3200.0;

    REQUIRE(attenuateRegion(audio.view(), SampleRate{kRate}, region, -60.0).ok());

    for (int channel = 0; channel < 2; ++channel) {
        CHECK(toneAmplitude(audio, 3000.0, 8000, 24000, channel) < 0.01);
        CHECK(toneAmplitude(audio, 1000.0, 8000, 24000, channel) == Approx(0.3).margin(0.02));
    }
}

TEST_CASE("How much a narrow band can actually remove, and why", "[spectral][edit][attenuate]") {
    // Asking for 60 dB on a 22 Hz band around a 50 Hz hum does not give 60 dB,
    // and no mask shape can make it. At 4096 points and 48 kHz the bins are
    // 11.7 Hz apart and the analysis window's skirts put part of the tone's
    // energy outside any band that narrow, where the edit correctly leaves it
    // alone. The achievable figure is measured here rather than asserted from
    // hope, so that a regression in the mask is distinguishable from the
    // resolution limit.
    const auto measure = [](int fftSize, int hopSize) {
        AudioBuffer audio = makeTones(196608, {{50.0, 0.2}, {1000.0, 0.3}});

        SpectralRegion region;
        region.startSample = 0;
        region.endSample = 196608;
        region.lowHz = 40.0;
        region.highHz = 62.0;

        SpectralEditSettings settings;
        settings.fftSize = fftSize;
        settings.hopSize = hopSize;

        const double before = toneAmplitude(audio, 50.0, 48000, 96000);
        REQUIRE(attenuateRegion(audio.view(), SampleRate{kRate}, region, -60.0, settings).ok());
        const double after = toneAmplitude(audio, 50.0, 48000, 96000);

        // The tone well clear of the band must survive whatever the settings.
        CHECK(toneAmplitude(audio, 1000.0, 48000, 96000) == Approx(0.3).margin(0.01));
        return 20.0 * std::log10(after / before);
    };

    const double coarse = measure(4096, 1024);
    const double fine = measure(16384, 4096);

    CHECK(coarse < -30.0);
    // A longer window resolves the band, so it gets closer to what was asked.
    // If this stops holding, the mask has broken, not the physics.
    CHECK(fine < coarse);
}
