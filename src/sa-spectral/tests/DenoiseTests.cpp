#include <sa/spectral/Denoise.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::spectral;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

/// Deterministic white noise, so a failure is reproducible rather than a story
/// about a seed.
std::vector<float> whiteNoise(SampleCount frames, float amplitude, unsigned seed) {
    std::mt19937 engine{seed};
    std::uniform_real_distribution<float> spread{-amplitude, amplitude};
    std::vector<float> out(static_cast<std::size_t>(frames));
    for (float& sample : out) {
        sample = spread(engine);
    }
    return out;
}

/// `noiseSeconds` of noise alone, then the same noise with a tone on top. The
/// first passage is what a user would select to learn from, the second is what
/// they would clean.
AudioBuffer noiseThenTone(double noiseSeconds, double toneSeconds, double toneHz,
                          float toneAmplitude, float noiseAmplitude, int channels = 1) {
    const auto noiseFrames = static_cast<SampleCount>(noiseSeconds * kRate);
    const auto toneFrames = static_cast<SampleCount>(toneSeconds * kRate);
    const SampleCount total = noiseFrames + toneFrames;

    AudioBuffer buffer{channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo(), total};
    for (int channel = 0; channel < channels; ++channel) {
        const auto noise =
            whiteNoise(total, noiseAmplitude, 1234u + static_cast<unsigned>(channel));
        float* out = buffer.channel(channel);
        for (SampleCount i = 0; i < total; ++i) {
            float value = noise[static_cast<std::size_t>(i)];
            if (i >= noiseFrames) {
                value +=
                    toneAmplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * toneHz *
                                                                static_cast<double>(i) / kRate));
            }
            out[i] = value;
        }
    }
    return buffer;
}

/// Amplitude at one frequency, by direct correlation.
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

double rms(const AudioBuffer& audio, SampleIndex start, SampleCount length, int channel = 0) {
    const float* samples = audio.channel(channel);
    double total = 0.0;
    for (SampleCount i = 0; i < length; ++i) {
        total += static_cast<double>(samples[start + i]) * samples[start + i];
    }
    return std::sqrt(total / static_cast<double>(length));
}

} // namespace

TEST_CASE("A profile learned from noise lowers the floor and keeps the tone",
          "[spectral][denoise]") {
    // Two seconds of hiss, then two seconds of hiss with a 1 kHz tone in it.
    AudioBuffer audio = noiseThenTone(2.0, 2.0, 1000.0, 0.25f, 0.05f);
    const auto noiseFrames = static_cast<SampleCount>(2.0 * kRate);

    const double toneBefore = toneAmplitude(audio, 1000.0, noiseFrames + 24000, 48000);
    const double floorBefore = rms(audio, 12000, 48000);

    auto profile = NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 0, noiseFrames);
    REQUIRE(profile.hasValue());
    CHECK(profile.value().channelCount() == 1);
    CHECK(profile.value().framesLearned() > 10);

    DenoiseSettings settings;
    settings.reductionDb = 18.0;
    REQUIRE(denoise(audio.view(), SampleRate{kRate}, profile.value(), 0, audio.frames(), settings)
                .ok());

    const double toneAfter = toneAmplitude(audio, 1000.0, noiseFrames + 24000, 48000);
    const double floorAfter = rms(audio, 12000, 48000);

    const double floorDrop = 20.0 * std::log10(floorAfter / floorBefore);
    const double toneChange = 20.0 * std::log10(toneAfter / toneBefore);

    // Measured across settings, with a 1 kHz tone 14 dB above white noise:
    //
    //     asked   floor moved   tone moved
    //      6 dB      -5.9 dB      -0.19 dB
    //     12 dB     -10.9 dB      -0.19 dB
    //     18 dB     -14.6 dB      -0.20 dB
    //     24 dB     -17.0 dB      -0.20 dB
    //
    // The floor tracks the request closely and then saturates, which is what a
    // Wiener gain does: bins where signal and noise are comparable keep a
    // partial gain rather than being pushed to the floor, and that is the
    // difference between a denoiser and a gate. The tone is untouched
    // throughout. The bounds below are loose enough to survive a different
    // noise seed and tight enough that losing either property fails.
    CHECK(floorDrop < -10.0);
    CHECK(toneChange > -2.0);
    CHECK(toneChange < 1.0);
}

TEST_CASE("Reduction is a limit, not a subtraction", "[spectral][denoise]") {
    // Asking for a huge reduction must not turn the signal into a hole: the
    // floor gain bounds how far any bin can fall, so a tone well above the
    // noise is untouched however aggressive the setting.
    AudioBuffer audio = noiseThenTone(2.0, 2.0, 1000.0, 0.4f, 0.02f);
    const auto noiseFrames = static_cast<SampleCount>(2.0 * kRate);

    auto profile = NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 0, noiseFrames);
    REQUIRE(profile.hasValue());

    const double before = toneAmplitude(audio, 1000.0, noiseFrames + 24000, 48000);

    DenoiseSettings settings;
    settings.reductionDb = 60.0;
    REQUIRE(denoise(audio.view(), SampleRate{kRate}, profile.value(), 0, audio.frames(), settings)
                .ok());

    const double after = toneAmplitude(audio, 1000.0, noiseFrames + 24000, 48000);
    CHECK(20.0 * std::log10(after / before) > -2.0);
}

TEST_CASE("Asking for no reduction changes nothing at all", "[spectral][denoise]") {
    AudioBuffer audio = noiseThenTone(1.0, 1.0, 800.0, 0.3f, 0.05f);
    AudioBuffer untouched = noiseThenTone(1.0, 1.0, 800.0, 0.3f, 0.05f);

    auto profile = NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 0,
                                       static_cast<SampleCount>(kRate));
    REQUIRE(profile.hasValue());

    DenoiseSettings settings;
    settings.reductionDb = 0.0;
    REQUIRE(denoise(audio.view(), SampleRate{kRate}, profile.value(), 0, audio.frames(), settings)
                .ok());

    for (SampleCount i = 0; i < audio.frames(); ++i) {
        REQUIRE(audio.channel(0)[i] == untouched.channel(0)[i]);
    }
}

TEST_CASE("Denoising is confined to the range it was given", "[spectral][denoise]") {
    AudioBuffer audio = noiseThenTone(1.0, 3.0, 1000.0, 0.2f, 0.06f);
    const auto rate = static_cast<SampleCount>(kRate);

    auto profile = NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 0, rate);
    REQUIRE(profile.hasValue());

    const double untouchedBefore = rms(audio, 3 * rate + 4000, 20000);

    DenoiseSettings settings;
    settings.reductionDb = 20.0;
    REQUIRE(
        denoise(audio.view(), SampleRate{kRate}, profile.value(), rate, 2 * rate, settings).ok());

    // Well clear of the range and its one window of spread.
    const double untouchedAfter = rms(audio, 3 * rate + 4000, 20000);
    CHECK(untouchedAfter == Approx(untouchedBefore).epsilon(0.001));
}

TEST_CASE("Both channels of a stereo profile are learned separately", "[spectral][denoise]") {
    AudioBuffer audio = noiseThenTone(2.0, 2.0, 1000.0, 0.25f, 0.05f, 2);
    const auto noiseFrames = static_cast<SampleCount>(2.0 * kRate);

    auto profile = NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 0, noiseFrames);
    REQUIRE(profile.hasValue());
    CHECK(profile.value().channelCount() == 2);

    // Different seeds per channel, so the two profiles must not be identical --
    // if they were, one channel's noise would be modelling the other's.
    int differing = 0;
    for (int bin = 0; bin < profile.value().binCount(); ++bin) {
        if (profile.value().magnitudeAt(0, bin) != profile.value().magnitudeAt(1, bin)) {
            ++differing;
        }
    }
    CHECK(differing > profile.value().binCount() / 2);

    const double before = rms(audio, 12000, 48000, 1);
    REQUIRE(denoise(audio.view(), SampleRate{kRate}, profile.value(), 0, audio.frames()).ok());
    CHECK(20.0 * std::log10(rms(audio, 12000, 48000, 1) / before) < -6.0);
}

TEST_CASE("Denoise refuses what it cannot do", "[spectral][denoise]") {
    AudioBuffer audio = noiseThenTone(1.0, 1.0, 1000.0, 0.2f, 0.05f);
    const auto rate = static_cast<SampleCount>(kRate);

    CHECK_FALSE(NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 100, 100).hasValue());
    CHECK_FALSE(NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 0, 999999).hasValue());
    CHECK_FALSE(NoiseProfile::learn(audio.constView(), SampleRate{0.0}, 0, rate).hasValue());

    // A range shorter than two analysis windows is refused. The guard is in
    // samples, not frames: front padding means sixty-four samples still produce
    // four frames, and averaging those describes the window rather than the
    // noise.
    CHECK_FALSE(NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 0, 64).hasValue());
    CHECK_FALSE(NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 0, 8191).hasValue());
    CHECK(NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 0, 8192).hasValue());

    auto profile = NoiseProfile::learn(audio.constView(), SampleRate{kRate}, 0, rate);
    REQUIRE(profile.hasValue());

    CHECK_FALSE(denoise(audio.view(), SampleRate{kRate}, profile.value(), 500, 100).ok());
    CHECK_FALSE(denoise(audio.view(), SampleRate{kRate}, profile.value(), 0, 999999).ok());
    CHECK_FALSE(denoise(audio.view(), SampleRate{kRate}, NoiseProfile{}, 0, rate).ok());

    // A profile learned at one analysis size cannot be read by another: it is a
    // table indexed by bin, and the bins would not mean the same frequencies.
    DenoiseSettings mismatched;
    mismatched.analysis.fftSize = 1024;
    mismatched.analysis.hopSize = 256;
    CHECK_FALSE(
        denoise(audio.view(), SampleRate{kRate}, profile.value(), 0, rate, mismatched).ok());

    AudioBuffer stereo = noiseThenTone(1.0, 1.0, 1000.0, 0.2f, 0.05f, 2);
    CHECK_FALSE(denoise(stereo.view(), SampleRate{kRate}, profile.value(), 0, rate).ok());
}

TEST_CASE("A profile from a different sample rate is refused", "[spectral][denoise]") {
    // Same bin count, different frequencies. Applied anyway, every bin's noise
    // would be subtracted from a band about 9% away from the one it came from:
    // the wrong thing removed everywhere, and it would look like it worked.
    AudioBuffer audio = noiseThenTone(1.0, 1.0, 1000.0, 0.2f, 0.05f);
    const auto rate = static_cast<SampleCount>(kRate);

    auto profile = NoiseProfile::learn(audio.constView(), SampleRate{44100.0}, 0, rate);
    REQUIRE(profile.hasValue());
    CHECK(profile.value().sampleRate().hz() == Approx(44100.0));

    CHECK_FALSE(denoise(audio.view(), SampleRate{kRate}, profile.value(), 0, rate).ok());
    CHECK(denoise(audio.view(), SampleRate{44100.0}, profile.value(), 0, rate).ok());
}
