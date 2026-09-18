#include <sa/spectral/TimeStretch.h>

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
using namespace sa::spectral;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

[[nodiscard]] AudioBuffer tones(SampleCount frames, const std::vector<double>& frequencies,
                                double amplitude, int channels = 1) {
    AudioBuffer buffer{channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo(), frames};
    for (int channel = 0; channel < channels; ++channel) {
        float* out = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            double value = 0.0;
            for (const double frequency : frequencies) {
                value += amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                              static_cast<double>(i) / kRate);
            }
            out[i] = static_cast<float>(value);
        }
    }
    return buffer;
}

/// Amplitude of one frequency over a window, by direct correlation.
[[nodiscard]] double amplitudeAt(const float* samples, SampleCount start, SampleCount count,
                                 double frequency) {
    double real = 0.0;
    double imaginary = 0.0;
    for (SampleCount i = 0; i < count; ++i) {
        const double angle =
            2.0 * std::numbers::pi * frequency * static_cast<double>(start + i) / kRate;
        real += static_cast<double>(samples[start + i]) * std::cos(angle);
        imaginary += static_cast<double>(samples[start + i]) * std::sin(angle);
    }
    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(count);
}

[[nodiscard]] double meanSquare(const float* samples, SampleCount start, SampleCount count) {
    double total = 0.0;
    for (SampleCount i = 0; i < count; ++i) {
        total += static_cast<double>(samples[start + i]) * static_cast<double>(samples[start + i]);
    }
    return total / static_cast<double>(count);
}

/// The share of the signal's power that is *not* in the frequencies it should
/// be in. The single number that says whether a stretch kept the material or
/// smeared it.
[[nodiscard]] double strayPowerFraction(const float* samples, SampleCount start, SampleCount count,
                                        const std::vector<double>& frequencies) {
    double wanted = 0.0;
    for (const double frequency : frequencies) {
        const double amplitude = amplitudeAt(samples, start, count, frequency);
        wanted += amplitude * amplitude / 2.0;
    }
    const double total = meanSquare(samples, start, count);
    return total > 0.0 ? std::max(0.0, 1.0 - wanted / total) : 1.0;
}

} // namespace

TEST_CASE("A stretched tone is the same tone, for longer", "[spectral][stretch]") {
    // The whole claim of a phase vocoder in one test: the length changes and
    // the frequency does not. Doing it by resampling would pass the length half
    // and fail the frequency half, which is exactly the mistake this exists to
    // avoid.
    const AudioBuffer audio = tones(static_cast<SampleCount>(2.0 * kRate), {440.0}, 0.5);

    StretchSettings settings;
    settings.factor = 2.0;
    const auto stretched = timeStretch(audio, settings);
    REQUIRE(stretched);

    const AudioBuffer& out = stretched.value();
    CHECK(out.frames() == audio.frames() * 2);

    // Measured clear of both ends, over a whole number of cycles of 440 Hz.
    const auto start = static_cast<SampleCount>(0.5 * kRate);
    const auto count = static_cast<SampleCount>(kRate * 3.0);
    CHECK(amplitudeAt(out.channel(0), start, count, 440.0) == Approx(0.5).margin(0.02));

    const double stray = strayPowerFraction(out.channel(0), start, count, {440.0});
    INFO("stray power " << stray * 100.0 << "%");
    CHECK(stray < 0.01);
}

TEST_CASE("Stretching lands on exactly the length asked for", "[spectral][stretch]") {
    // A caller fitting a take into a slot needs the slot's length, not a length
    // that fell out of a frame count.
    const AudioBuffer audio = tones(100000, {300.0}, 0.4);
    for (const double factor : {0.5, 0.75, 1.0, 1.37, 2.0, 3.5}) {
        StretchSettings settings;
        settings.factor = factor;
        const auto stretched = timeStretch(audio, settings);
        REQUIRE(stretched);
        const auto wanted =
            static_cast<SampleCount>(std::llround(static_cast<double>(audio.frames()) * factor));
        INFO("factor " << factor);
        CHECK(stretched.value().frames() == wanted);
    }
}

TEST_CASE("A factor of one is very nearly a copy", "[spectral][stretch]") {
    // Not bit-exact: the vocoder rebuilds phase from measured frequencies
    // rather than carrying the original through, so a factor of one is a
    // round trip through that reconstruction rather than a shortcut past it.
    // The point of measuring it is that this error is the floor under every
    // other stretch, so it is worth knowing rather than assuming.
    const AudioBuffer audio = tones(static_cast<SampleCount>(2.0 * kRate), {440.0, 1320.0}, 0.3);

    StretchSettings settings;
    settings.factor = 1.0;
    const auto copied = timeStretch(audio, settings);
    REQUIRE(copied);
    REQUIRE(copied.value().frames() == audio.frames());

    const auto start = static_cast<SampleCount>(0.5 * kRate);
    const auto count = static_cast<SampleCount>(kRate);
    double worst = 0.0;
    for (SampleCount i = start; i < start + count; ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(copied.value().channel(0)[i]) -
                                         static_cast<double>(audio.channel(0)[i])));
    }
    INFO("worst sample difference " << worst);
    // Both partials survive at their own amplitudes, which is the part that
    // matters; the waveform itself may be shifted in phase and is not compared.
    CHECK(amplitudeAt(copied.value().channel(0), start, count, 440.0) == Approx(0.3).margin(0.01));
    CHECK(amplitudeAt(copied.value().channel(0), start, count, 1320.0) == Approx(0.3).margin(0.01));
    CHECK(strayPowerFraction(copied.value().channel(0), start, count, {440.0, 1320.0}) < 0.01);
}

TEST_CASE("Locking a partial's bins together is what keeps the signal", "[spectral][stretch]") {
    // Not a refinement: without locking, more than half the amplitude of a
    // plain tone is lost. The bins that make up the tone come out of its onset
    // skewed against each other and stay skewed, so they cancel part of it away
    // for the rest of the file. See StretchSettings::lockPhases.
    const AudioBuffer audio = tones(static_cast<SampleCount>(2.0 * kRate), {440.0}, 0.25);
    const auto start = static_cast<SampleCount>(0.5 * kRate);
    const auto count = static_cast<SampleCount>(kRate * 3.0);

    StretchSettings locked;
    locked.factor = 2.0;
    StretchSettings loose = locked;
    loose.lockPhases = false;

    const auto withLocking = timeStretch(audio, locked);
    const auto withoutLocking = timeStretch(audio, loose);
    REQUIRE(withLocking);
    REQUIRE(withoutLocking);

    const double held = amplitudeAt(withLocking.value().channel(0), start, count, 440.0);
    const double lost = amplitudeAt(withoutLocking.value().channel(0), start, count, 440.0);
    INFO("locked " << held << ", unlocked " << lost);
    CHECK(held == Approx(0.25).margin(0.005));
    // Measured at 0.129, which is 5.7 dB down. The bound is loose because the
    // exact figure depends on where the tone falls between bins; what is being
    // pinned is that this is a large loss, not a nuance.
    CHECK(lost < 0.25 * 0.75);
}

TEST_CASE("Notes far enough apart to be resolved come through intact", "[spectral][stretch]") {
    // Three octaves of the same note: as far apart as partials get, and the
    // case the default window is sized for.
    const std::vector<double> chord{220.0, 440.0, 880.0};
    const AudioBuffer audio = tones(static_cast<SampleCount>(2.0 * kRate), chord, 0.25);

    StretchSettings settings;
    settings.factor = 2.0;
    const auto stretched = timeStretch(audio, settings);
    REQUIRE(stretched);

    const auto start = static_cast<SampleCount>(0.5 * kRate);
    const auto count = static_cast<SampleCount>(kRate * 3.0);
    for (const double note : chord) {
        INFO("note " << note);
        CHECK(amplitudeAt(stretched.value().channel(0), start, count, note) ==
              Approx(0.25).margin(0.005));
    }
    CHECK(strayPowerFraction(stretched.value().channel(0), start, count, chord) < 0.005);
}

TEST_CASE("Partials closer than the window can resolve are not separated", "[spectral][stretch]") {
    // A known limit, pinned so it cannot quietly get worse and so the rule is
    // written down somewhere a reader will find it: a Hann window's main lobe
    // is four bins wide, and partials inside one lobe are one lump to the
    // analysis. A close A major triad is two and a bit bins apart at the
    // default window, and comes out with the middle note pulled down.
    // Widening the window to where the notes are four bins apart fixes it
    // completely -- which is the whole of the rule.
    const std::vector<double> triad{220.0, 277.18, 329.63};
    const AudioBuffer audio = tones(static_cast<SampleCount>(2.0 * kRate), triad, 0.25);

    const auto start = static_cast<SampleCount>(0.5 * kRate);
    const auto count = static_cast<SampleCount>(kRate * 3.0);

    StretchSettings narrow;
    narrow.factor = 2.0;
    const auto unresolved = timeStretch(audio, narrow);
    REQUIRE(unresolved);
    const double strayNarrow =
        strayPowerFraction(unresolved.value().channel(0), start, count, triad);

    StretchSettings wide = narrow;
    wide.fftSize = 8192;
    wide.hopSize = 1024;
    const auto resolved = timeStretch(audio, wide);
    REQUIRE(resolved);
    const double strayWide = strayPowerFraction(resolved.value().channel(0), start, count, triad);

    INFO("2048 strays " << strayNarrow * 100.0 << "%, 8192 strays " << strayWide * 100.0 << "%");
    CHECK(strayNarrow > 0.02); // Measured at 7.9%: the limit is real.
    CHECK(strayWide < 0.005);  // Measured at under a tenth of a percent.
    for (const double note : triad) {
        INFO("note " << note);
        CHECK(amplitudeAt(resolved.value().channel(0), start, count, note) ==
              Approx(0.25).margin(0.005));
    }
}

TEST_CASE("An octave up is exactly twice the frequency, and the same length", "[spectral][pitch]") {
    const AudioBuffer audio = tones(static_cast<SampleCount>(2.0 * kRate), {440.0}, 0.5);

    PitchSettings settings;
    settings.semitones = 12.0;
    const auto shifted = pitchShift(audio, settings);
    REQUIRE(shifted);
    REQUIRE(shifted.value().frames() == audio.frames());

    const auto start = static_cast<SampleCount>(0.4 * kRate);
    const auto count = static_cast<SampleCount>(kRate);
    CHECK(amplitudeAt(shifted.value().channel(0), start, count, 880.0) == Approx(0.5).margin(0.03));
    // And the note it used to be is gone, not merely joined.
    CHECK(amplitudeAt(shifted.value().channel(0), start, count, 440.0) < 0.02);
    CHECK(strayPowerFraction(shifted.value().channel(0), start, count, {880.0}) < 0.02);
}

TEST_CASE("A tuning correction of a fraction of a semitone lands where it should",
          "[spectral][pitch]") {
    // What pitch shifting is actually used for: a take that is 23 cents flat.
    // A shift that is only right at whole semitones is no use for that.
    const AudioBuffer audio = tones(static_cast<SampleCount>(2.0 * kRate), {440.0}, 0.5);

    PitchSettings settings;
    settings.semitones = 0.23;
    const auto shifted = pitchShift(audio, settings);
    REQUIRE(shifted);

    const double wanted = 440.0 * pitchRatio(0.23);
    CHECK(wanted == Approx(445.88).margin(0.01));

    const auto start = static_cast<SampleCount>(0.4 * kRate);
    const auto count = static_cast<SampleCount>(kRate);
    CHECK(amplitudeAt(shifted.value().channel(0), start, count, wanted) ==
          Approx(0.5).margin(0.03));
    CHECK(strayPowerFraction(shifted.value().channel(0), start, count, {wanted}) < 0.02);
}

TEST_CASE("Both channels get the same treatment", "[spectral][stretch]") {
    // Two channels carrying the same signal have to come out carrying the same
    // signal. A phase vocoder run per channel can drift them apart, which on a
    // stereo recording is a collapsed or wandering image rather than an
    // obviously broken file -- the kind of fault that ships.
    const AudioBuffer audio = tones(static_cast<SampleCount>(1.0 * kRate), {440.0, 660.0}, 0.3, 2);

    StretchSettings settings;
    settings.factor = 1.6;
    const auto stretched = timeStretch(audio, settings);
    REQUIRE(stretched);

    double worst = 0.0;
    for (SampleCount i = 0; i < stretched.value().frames(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(stretched.value().channel(0)[i]) -
                                         static_cast<double>(stretched.value().channel(1)[i])));
    }
    CHECK(worst == Approx(0.0).margin(1e-6));
}

TEST_CASE("Pitch shifting keeps a stereo pair together", "[spectral][pitch]") {
    const AudioBuffer audio = tones(static_cast<SampleCount>(1.0 * kRate), {440.0}, 0.4, 2);

    PitchSettings settings;
    settings.semitones = -5.0;
    const auto shifted = pitchShift(audio, settings);
    REQUIRE(shifted);

    double worst = 0.0;
    for (SampleCount i = 0; i < shifted.value().frames(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(shifted.value().channel(0)[i]) -
                                         static_cast<double>(shifted.value().channel(1)[i])));
    }
    CHECK(worst == Approx(0.0).margin(1e-6));
}

TEST_CASE("A stretch can come out louder, and not by much", "[spectral][stretch]") {
    // Rebuilding a waveform from magnitudes and reconstructed phases does not
    // reproduce the original crest -- partials that cancelled at one instant in
    // the source need not cancel at the matching instant in the output. The
    // gain is deliberately not trimmed for it, so the size of the overshoot is
    // the thing that has to stay small, and this is what pins it.
    std::mt19937 engine{99};
    std::normal_distribution<float> noise{0.0f, 0.3f};

    for (const int kind : {0, 1, 2}) {
        AudioBuffer audio{ChannelLayout::mono(), static_cast<SampleCount>(2.0 * kRate)};
        double loudest = 0.0;
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            double value = 0.0;
            if (kind == 0) {
                value = std::sin(2.0 * std::numbers::pi * 440.0 * t);
            } else if (kind == 1) {
                value = static_cast<double>(noise(engine));
            } else {
                value = std::sin(2.0 * std::numbers::pi * 220.0 * t) +
                        0.7 * std::sin(2.0 * std::numbers::pi * 443.0 * t) +
                        0.5 * std::sin(2.0 * std::numbers::pi * 881.0 * t);
            }
            audio.channel(0)[i] = static_cast<float>(value);
            loudest = std::max(loudest, std::abs(value));
        }
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            audio.channel(0)[i] = static_cast<float>(audio.channel(0)[i] / loudest * 0.99);
        }

        for (const double factor : {0.5, 1.37, 2.0, 4.0}) {
            StretchSettings settings;
            settings.factor = factor;
            const auto stretched = timeStretch(audio, settings);
            REQUIRE(stretched);

            double peak = 0.0;
            for (SampleCount i = 0; i < stretched.value().frames(); ++i) {
                peak =
                    std::max(peak, std::abs(static_cast<double>(stretched.value().channel(0)[i])));
            }
            const double overshoot = 20.0 * std::log10(peak / 0.99);
            INFO("kind " << kind << " factor " << factor << " overshoot " << overshoot << " dB");
            // Measured worst case across these twelve combinations: +0.11 dB.
            CHECK(overshoot < 0.5);
        }
    }
}

TEST_CASE("Stretching refuses what it cannot do", "[spectral][stretch]") {
    const AudioBuffer audio = tones(48000, {440.0}, 0.5);

    CHECK_FALSE(timeStretch(AudioBuffer{}).hasValue());

    StretchSettings settings;
    settings.factor = 0.0;
    CHECK_FALSE(timeStretch(audio, settings).hasValue());
    settings.factor = 100.0;
    CHECK_FALSE(timeStretch(audio, settings).hasValue());
    settings.factor = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(timeStretch(audio, settings).hasValue());

    settings.factor = 2.0;
    settings.fftSize = 1000; // Not a power of two.
    CHECK_FALSE(timeStretch(audio, settings).hasValue());

    settings.fftSize = 2048;
    settings.hopSize = 0;
    CHECK_FALSE(timeStretch(audio, settings).hasValue());

    PitchSettings pitch;
    pitch.semitones = 60.0;
    CHECK_FALSE(pitchShift(audio, pitch).hasValue());
    pitch.semitones = -60.0;
    CHECK_FALSE(pitchShift(audio, pitch).hasValue());
    pitch.semitones = 0.0;
    CHECK_FALSE(pitchShift(AudioBuffer{}, pitch).hasValue());
}

TEST_CASE("The semitone ratio is the one everybody else uses", "[spectral][pitch]") {
    CHECK(pitchRatio(0.0) == Approx(1.0));
    CHECK(pitchRatio(12.0) == Approx(2.0));
    CHECK(pitchRatio(-12.0) == Approx(0.5));
    CHECK(pitchRatio(7.0) == Approx(1.4983).margin(1e-4)); // A fifth, near enough to 3/2.
}
