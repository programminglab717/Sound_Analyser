#include <sa/analysis/Provenance.h>
#include <sa/dsp/Fft.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};
constexpr SampleCount kFrames = 48000 * 4;

/// Noise with a flat spectrum, which is the honest starting point for a test
/// about where a spectrum stops: a tone would have nothing above it either way.
[[nodiscard]] AudioBuffer hiss(SampleCount frames = kFrames, int channels = 1, unsigned seed = 7) {
    std::mt19937 engine{seed};
    std::normal_distribution<float> noise{0.0f, 0.15f};
    AudioBuffer audio{ChannelLayout::discrete(channels), frames};
    for (int channel = 0; channel < channels; ++channel) {
        for (SampleCount i = 0; i < frames; ++i) {
            audio.channel(channel)[i] = noise(engine);
        }
    }
    return audio;
}

/// Remove everything above `cutoff` the way a lossy encoder does: by deleting
/// the bands outright in the frequency domain.
///
/// A filter cascade was the first attempt and it was the wrong material. Twelve
/// one-poles is 72 dB per octave, which sounds steep and is 18 dB across the
/// quarter-octave the detector looks at -- so the detector correctly said "that
/// is a rolloff, not a cliff", and the test was wrong rather than the code. An
/// encoder does not filter; it stops coding, and what it leaves is this.
void brickWall(AudioBuffer& audio, double cutoff) {
    const auto size = static_cast<int>(audio.frames());
    REQUIRE(dsp::RealFft::isSupportedSize(size));

    dsp::RealFft fft{size};
    std::vector<std::complex<float>> bins(static_cast<std::size_t>(fft.binCount()));
    const double perBin = kRate.hz() / size;

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        fft.forward(audio.channel(channel), bins.data());
        for (int bin = 0; bin < fft.binCount(); ++bin) {
            if (perBin * bin >= cutoff) {
                bins[static_cast<std::size_t>(bin)] = {};
            }
        }
        fft.inverse(bins.data(), audio.channel(channel));
    }
}

/// Quantise to `bits` and leave the result in the float range, as a file of
/// that depth read back would be.
void quantise(AudioBuffer& audio, int bits) {
    const double scale = std::exp2(bits - 1);
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            const double value =
                std::clamp(static_cast<double>(audio.channel(channel)[i]), -1.0, 1.0);
            audio.channel(channel)[i] = static_cast<float>(std::round(value * scale) / scale);
        }
    }
}

} // namespace

TEST_CASE("Full-band material reports no cutoff") {
    const AudioBuffer audio = hiss();
    const auto found = examineProvenance(audio.view(), kRate);
    REQUIRE(found);
    REQUIRE(found.value().valid);
    REQUIRE_FALSE(found.value().hasSteepCutoff);
    REQUIRE(found.value().cutoffHz == 0.0);
}

TEST_CASE("A band-limited file reports where it stops") {
    for (const double cutoff : {11000.0, 16000.0}) {
        // A power of two, because the brick wall is applied with one transform
        // over the whole buffer. 262144 is about 5.5 seconds at 48 kHz.
        AudioBuffer audio = hiss(262144);
        brickWall(audio, cutoff);

        const auto found = examineProvenance(audio.view(), kRate);
        REQUIRE(found);
        REQUIRE(found.value().valid);
        REQUIRE(found.value().hasSteepCutoff);
        // Within a third of an octave of where the energy actually stops. The
        // detector reports where the level has fallen through the threshold,
        // which for a real rolloff is a little above the nominal corner.
        REQUIRE(found.value().cutoffHz > cutoff * 0.8);
        REQUIRE(found.value().cutoffHz < cutoff * 2.0);
        REQUIRE(found.value().cutoffDropDb >= 35.0);
    }
}

TEST_CASE("A gentle rolloff is not called a cutoff") {
    // The false positive worth avoiding. One pole is what a microphone, a room
    // or a tape does, and it takes octaves rather than a fraction of one.
    AudioBuffer audio = hiss();
    const double w = 2.0 * std::numbers::pi * 8000.0 / kRate.hz();
    const double alpha = std::sin(w) / (1.0 + std::cos(w));
    double state = 0.0;
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        state += alpha * (audio.channel(0)[i] - state);
        audio.channel(0)[i] = static_cast<float>(state);
    }

    const auto found = examineProvenance(audio.view(), kRate);
    REQUIRE(found);
    REQUIRE_FALSE(found.value().hasSteepCutoff);
}

TEST_CASE("A padded file is reported as the depth it really uses") {
    // The case this exists for: a 16-bit master delivered as a 24-bit file.
    AudioBuffer audio = hiss(48000);
    quantise(audio, 16);

    REQUIRE(effectiveBitDepth(audio.view(), 24) == 16);
    REQUIRE(effectiveBitDepth(audio.view(), 16) == 16);

    ProvenanceSettings settings;
    settings.declaredBits = 24;
    const auto found = examineProvenance(audio.view(), kRate, settings);
    REQUIRE(found);
    REQUIRE(found.value().effectiveBits == 16);
    REQUIRE(found.value().declaredBits == 24);
    REQUIRE(found.value().isPadded);
}

TEST_CASE("A genuinely deep file is not called padded") {
    AudioBuffer audio = hiss(48000);
    quantise(audio, 24);

    REQUIRE(effectiveBitDepth(audio.view(), 24) == 24);

    ProvenanceSettings settings;
    settings.declaredBits = 24;
    const auto found = examineProvenance(audio.view(), kRate, settings);
    REQUIRE(found);
    REQUIRE_FALSE(found.value().isPadded);
}

TEST_CASE("Bit depth is found exactly, at every depth") {
    for (const int real : {8, 12, 16, 20, 24}) {
        AudioBuffer audio = hiss(24000);
        quantise(audio, real);
        REQUIRE(effectiveBitDepth(audio.view(), 24) == real);
    }
}

TEST_CASE("Silence uses no bits, and says so rather than guessing") {
    AudioBuffer audio{ChannelLayout::mono(), 1000};
    REQUIRE(effectiveBitDepth(audio.view(), 24) == 0);

    ProvenanceSettings settings;
    settings.declaredBits = 24;
    const auto found = examineProvenance(audio.view(), kRate, settings);
    REQUIRE(found);
    REQUIRE(found.value().effectiveBits == 0);
    // Not padded: nothing is known about a file with nothing in it.
    REQUIRE_FALSE(found.value().isPadded);
}

TEST_CASE("A float file is not asked the bit-depth question") {
    AudioBuffer audio = hiss(24000);
    const auto found = examineProvenance(audio.view(), kRate); // declaredBits 0.
    REQUIRE(found);
    REQUIRE(found.value().effectiveBits == 0);
    REQUIRE(found.value().declaredBits == 0);
    REQUIRE_FALSE(found.value().isPadded);
}

TEST_CASE("Nonsense input is refused rather than measured") {
    AudioBuffer empty{ChannelLayout::mono(), 0};
    const auto found = examineProvenance(empty.view(), kRate);
    REQUIRE(found);
    REQUIRE_FALSE(found.value().valid);

    REQUIRE(effectiveBitDepth(empty.view(), 24) == 0);
    AudioBuffer audio = hiss(1000);
    REQUIRE(effectiveBitDepth(audio.view(), 0) == 0);
    REQUIRE(effectiveBitDepth(audio.view(), 33) == 0);
}

TEST_CASE("A quiet recording is not mistaken for a band-limited one") {
    // The reference is the loudest the programme gets, not full scale, so a
    // recording 40 dB down is still measured against itself.
    AudioBuffer audio = hiss();
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        audio.channel(0)[i] *= 0.01f;
    }
    const auto found = examineProvenance(audio.view(), kRate);
    REQUIRE(found);
    REQUIRE_FALSE(found.value().hasSteepCutoff);
}
