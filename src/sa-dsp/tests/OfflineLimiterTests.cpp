#include <sa/dsp/Fft.h>
#include <sa/dsp/OfflineLimiter.h>

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

/// True peak by exact band-limited reconstruction: zero-pad the spectrum and
/// transform back.
///
/// Deliberately not our own interpolator. A limiter checked with the detector
/// it was built against agrees with itself and proves nothing; this shares no
/// code with either, so when the two disagree the disagreement is real. It was
/// validated against tones whose true peak is known analytically -- see the
/// first test.
[[nodiscard]] double truePeak(const float* samples, SampleCount count, int factor = 16) {
    constexpr int kChunk = 4096;
    if (count < kChunk) {
        return 0.0;
    }
    const RealFft forward{kChunk};
    const RealFft inverse{kChunk * factor};

    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(forward.binCount()));
    std::vector<std::complex<float>> padded(static_cast<std::size_t>(inverse.binCount()));
    std::vector<float> fine(static_cast<std::size_t>(kChunk) * static_cast<std::size_t>(factor));
    std::vector<float> block(kChunk);

    double peak = 0.0;
    const SampleCount margin = (kChunk * factor) / 16;
    for (SampleCount start = 0; start + kChunk <= count; start += kChunk / 2) {
        std::copy_n(samples + start, kChunk, block.data());
        forward.forward(block.data(), spectrum.data());

        std::fill(padded.begin(), padded.end(), std::complex<float>{});
        std::copy(spectrum.begin(), spectrum.end(), padded.begin());
        inverse.inverse(padded.data(), fine.data());

        // The inverse is scaled for its own length, so undo that.
        for (SampleCount i = margin; i < static_cast<SampleCount>(fine.size()) - margin; ++i) {
            peak = std::max(
                peak, std::abs(static_cast<double>(fine[static_cast<std::size_t>(i)]) * factor));
        }
    }
    return peak;
}

[[nodiscard]] double decibels(double linear) {
    return linear > 0.0 ? 20.0 * std::log10(linear) : -200.0;
}

AudioBuffer hotMaterial(SampleCount frames, int channels, unsigned seed) {
    std::mt19937 engine{seed};
    std::normal_distribution<float> noise{0.0f, 0.35f};

    AudioBuffer buffer{channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo(), frames};
    for (int channel = 0; channel < channels; ++channel) {
        float* out = buffer.channel(channel);
        double loudest = 0.0;
        for (SampleCount i = 0; i < frames; ++i) {
            // A quarter-rate tone is the classic inter-sample peak generator:
            // its samples sit well below its crest.
            const double tone =
                0.7 * std::sin(2.0 * std::numbers::pi * 0.25 * static_cast<double>(i) +
                               static_cast<double>(channel));
            out[i] = static_cast<float>(tone) + noise(engine);
            loudest = std::max(loudest, std::abs(static_cast<double>(out[i])));
        }
        for (SampleCount i = 0; i < frames; ++i) {
            out[i] = static_cast<float>(out[i] / loudest * 0.999);
        }
    }
    return buffer;
}

} // namespace

TEST_CASE("The reconstruction used to judge the limiter is itself correct",
          "[dsp][limiter][offline]") {
    // A quarter-rate tone shifted by an eighth of a cycle: its samples reach
    // A/sqrt(2) while its true peak is A. A method that cannot see that cannot
    // judge a true-peak limiter.
    constexpr SampleCount kFrames = 16384;
    constexpr double kAmplitude = 0.5;
    std::vector<float> tone(static_cast<std::size_t>(kFrames));
    for (SampleCount i = 0; i < kFrames; ++i) {
        tone[static_cast<std::size_t>(i)] = static_cast<float>(
            kAmplitude * std::sin(2.0 * std::numbers::pi * 0.25 * static_cast<double>(i) +
                                  std::numbers::pi / 4.0));
    }
    double samplePeak = 0.0;
    for (const float value : tone) {
        samplePeak = std::max(samplePeak, std::abs(static_cast<double>(value)));
    }

    CHECK(samplePeak == Approx(kAmplitude / std::numbers::sqrt2).margin(1e-5));
    CHECK(truePeak(tone.data(), kFrames) == Approx(kAmplitude).margin(0.005));
}

TEST_CASE("The offline limiter holds its ceiling in the reconstruction",
          "[dsp][limiter][offline]") {
    // The streaming limiter does not, and that is why this exists: it detects on
    // an oversampled view but applies gain at the base rate, and sample-rate
    // gain modulation puts energy above Nyquist. Measured on this material, it
    // leaves about 0.2 dB over. Oversampling the signal path removes that.
    AudioBuffer audio = hotMaterial(96000, 2, 4242);

    const double before = truePeak(audio.channel(0), audio.frames());
    CHECK(decibels(before) > -0.5); // Genuinely hot to start with.

    OfflineLimitSettings settings;
    settings.limiter.ceilingDb = -1.0;
    settings.oversampling = 4;
    REQUIRE(limitOffline(audio.view(), SampleRate{kRate}, settings).ok());

    for (int channel = 0; channel < 2; ++channel) {
        const double after = decibels(truePeak(audio.channel(channel), audio.frames()));
        INFO("channel " << channel << " reconstructs at " << after << " dBTP");
        // Held, not approached. Measured at exactly -1.000 dBTP with the trim
        // on; without it the same material comes out at -0.840, which is why
        // the trim exists. The slack here is for the local measurement's own
        // edge behaviour, not for the limiter.
        CHECK(after < -1.0 + 0.02);

        // And the samples themselves are under it too, exactly.
        double samplePeak = 0.0;
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            samplePeak =
                std::max(samplePeak, std::abs(static_cast<double>(audio.channel(channel)[i])));
        }
        CHECK(decibels(samplePeak) <= -1.0 + 1e-4);
    }
}

TEST_CASE("Quiet material passes through the offline limiter untouched",
          "[dsp][limiter][offline]") {
    // A limiter that colours material it never needs to act on is a limiter
    // nobody leaves in the chain.
    AudioBuffer audio{ChannelLayout::mono(), 48000};
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        audio.channel(0)[i] = static_cast<float>(
            0.1 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate));
    }
    AudioBuffer expected{ChannelLayout::mono(), 48000};
    std::copy_n(audio.channel(0), audio.frames(), expected.channel(0));

    OfflineLimitSettings settings;
    settings.limiter.ceilingDb = -1.0;
    REQUIRE(limitOffline(audio.view(), SampleRate{kRate}, settings).ok());

    // Two conversions are not bit-exact, but they must be inaudible. Measured
    // worst difference is under 1e-3 on a 0.1 amplitude tone, which is -60 dB
    // relative to the signal.
    double worst = 0.0;
    for (SampleCount i = 2000; i < audio.frames() - 2000; ++i) {
        worst = std::max(
            worst, std::abs(static_cast<double>(audio.channel(0)[i]) - expected.channel(0)[i]));
    }
    INFO("worst difference " << worst);
    CHECK(worst < 2e-3);
}

TEST_CASE("The offline limiter refuses what it cannot do", "[dsp][limiter][offline]") {
    AudioBuffer audio{ChannelLayout::mono(), 4800};
    const AudioBufferView view = audio.view();

    OfflineLimitSettings settings;
    CHECK_FALSE(limitOffline(AudioBufferView{}, SampleRate{kRate}, settings).ok());
    CHECK_FALSE(limitOffline(view, SampleRate{0.0}, settings).ok());

    settings.oversampling = 0;
    CHECK_FALSE(limitOffline(view, SampleRate{kRate}, settings).ok());
    settings.oversampling = 64;
    CHECK_FALSE(limitOffline(view, SampleRate{kRate}, settings).ok());

    settings.oversampling = 1;
    CHECK(limitOffline(view, SampleRate{kRate}, settings).ok());
}

TEST_CASE("Length is preserved exactly", "[dsp][limiter][offline]") {
    // Two conversions and a look-ahead delay all change length internally. A
    // caller replacing a range in a document needs what it put in back.
    for (const SampleCount frames : {4800, 48000, 12345}) {
        AudioBuffer audio = hotMaterial(frames, 1, 7);
        REQUIRE(limitOffline(audio.view(), SampleRate{kRate}).ok());
        CHECK(audio.frames() == frames);
    }
}
