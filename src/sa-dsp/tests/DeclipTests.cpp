#include <sa/dsp/Declip.h>
#include <sa/dsp/LinearPrediction.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace sa;
using namespace sa::dsp;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

[[nodiscard]] AudioBuffer material(SampleCount frames, int channels = 1, unsigned seed = 3) {
    std::mt19937 engine{seed};
    std::normal_distribution<float> hiss{0.0f, 0.005f};

    AudioBuffer buffer{channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo(), frames};
    for (int channel = 0; channel < channels; ++channel) {
        float* out = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i) / kRate;
            const double phase = static_cast<double>(channel) * 0.4;
            out[i] = static_cast<float>(
                0.55 * std::sin(2.0 * std::numbers::pi * 180.0 * t + phase) +
                0.28 * std::sin(2.0 * std::numbers::pi * 431.0 * t + phase) +
                0.14 * std::sin(2.0 * std::numbers::pi * 1103.0 * t + phase) + hiss(engine));
        }
    }
    return buffer;
}

[[nodiscard]] AudioBuffer copyOf(const AudioBuffer& source) {
    AudioBuffer copy{source.layout(), source.frames()};
    for (int channel = 0; channel < source.channelCount(); ++channel) {
        std::copy_n(source.channel(channel), source.frames(), copy.channel(channel));
    }
    return copy;
}

void clipAt(AudioBuffer& audio, double level) {
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        float* out = audio.channel(channel);
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            out[i] = static_cast<float>(std::clamp(static_cast<double>(out[i]), -level, level));
        }
    }
}

[[nodiscard]] double peakOf(const AudioBuffer& audio, int channel) {
    double peak = 0.0;
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        peak = std::max(peak, std::abs(static_cast<double>(audio.channel(channel)[i])));
    }
    return peak;
}

/// Error energy against a reference, relative to the reference, in dB. Both
/// signals are scaled to the same peak first, so a level difference does not
/// masquerade as a shape difference.
[[nodiscard]] double shapeErrorDb(const AudioBuffer& actual, const AudioBuffer& wanted,
                                  int channel) {
    const double actualPeak = peakOf(actual, channel);
    const double wantedPeak = peakOf(wanted, channel);
    if (!(actualPeak > 0.0) || !(wantedPeak > 0.0)) {
        return 0.0;
    }
    const double scale = wantedPeak / actualPeak;

    double error = 0.0;
    double signal = 0.0;
    for (SampleCount i = 0; i < wanted.frames(); ++i) {
        const double difference = static_cast<double>(actual.channel(channel)[i]) * scale -
                                  static_cast<double>(wanted.channel(channel)[i]);
        error += difference * difference;
        signal += static_cast<double>(wanted.channel(channel)[i]) *
                  static_cast<double>(wanted.channel(channel)[i]);
    }
    return error > 0.0 ? 10.0 * std::log10(error / signal) : -200.0;
}

} // namespace

TEST_CASE("Clipped peaks come back", "[dsp][declip]") {
    const AudioBuffer clean = material(96000);
    AudioBuffer clipped = copyOf(clean);
    clipAt(clipped, 0.55);

    AudioBuffer restored = copyOf(clipped);
    DeclipSettings settings;
    settings.fitToCeiling = false; // Compare shapes, not levels.
    const auto report = declip(restored.view(), settings);
    REQUIRE(report);

    const double before = shapeErrorDb(clipped, clean, 0);
    const double after = shapeErrorDb(restored, clean, 0);
    INFO("restored " << report.value().runs << " runs, " << report.value().samplesRestored
                     << " samples; shape error " << before << " dB -> " << after << " dB");

    CHECK(report.value().runs > 50);
    CHECK(after < before - 6.0);
    // The peaks genuinely went back above where they were cut off.
    CHECK(report.value().restoredPeak > 0.55);
    CHECK(peakOf(restored, 0) > peakOf(clipped, 0));
}

TEST_CASE("An unclipped crest is not mistaken for a clipped one", "[dsp][declip]") {
    // The case flatness alone cannot decide. A low tone is genuinely flat at
    // the top of its arc -- flatter, sample to sample, than real clipping often
    // survives being converted to 16-bit -- so the detector has to ask what the
    // model says should have been there rather than how flat it looks.
    AudioBuffer audio{ChannelLayout::mono(), 96000};
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        audio.channel(0)[i] = static_cast<float>(
            0.9 * std::sin(2.0 * std::numbers::pi * 50.0 * static_cast<double>(i) / kRate));
    }
    const AudioBuffer before = copyOf(audio);

    const auto report = declip(audio.view());
    REQUIRE(report);
    INFO("found " << report.value().runs << " runs in a plain 50 Hz tone");
    CHECK(report.value().runs == 0);

    double worst = 0.0;
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(audio.channel(0)[i]) -
                                         static_cast<double>(before.channel(0)[i])));
    }
    CHECK(worst == 0.0);
}

TEST_CASE("Ordinary material is left alone", "[dsp][declip]") {
    AudioBuffer audio = material(96000);
    const AudioBuffer before = copyOf(audio);

    const auto report = declip(audio.view());
    REQUIRE(report);
    INFO("found " << report.value().runs << " runs in unclipped material");
    CHECK(report.value().runs == 0);

    double worst = 0.0;
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(audio.channel(0)[i]) -
                                         static_cast<double>(before.channel(0)[i])));
    }
    CHECK(worst == 0.0);
}

TEST_CASE("The result is brought down to fit, and says so", "[dsp][declip]") {
    // Peaks put back above a ceiling the material was already touching go over
    // full scale by definition. A repair that leaves the file clipping on
    // export has undone itself.
    const AudioBuffer clean = material(96000);
    AudioBuffer clipped = copyOf(clean);
    clipAt(clipped, 0.6);

    const auto report = declip(clipped.view());
    REQUIRE(report);
    REQUIRE(report.value().runs > 0);

    const double ceiling = std::pow(10.0, -0.1 / 20.0);
    INFO("restored peak " << report.value().restoredPeak << ", gain " << report.value().gainDb
                          << " dB, final peak " << peakOf(clipped, 0));
    CHECK(peakOf(clipped, 0) <= ceiling + 1e-6);
    if (report.value().restoredPeak > ceiling) {
        CHECK(report.value().gainDb < 0.0);
        // And the reported gain is the gain actually applied.
        CHECK(report.value().restoredPeak * std::pow(10.0, report.value().gainDb / 20.0) ==
              Approx(peakOf(clipped, 0)).epsilon(1e-4));
    }
}

TEST_CASE("A flat top too long to be a peak is left alone", "[dsp][declip]") {
    AudioBuffer audio = material(48000);
    // Nine hundred samples pinned at the ceiling: that is not a clipped peak,
    // it is a passage driven so hard there is no shape left to infer.
    for (SampleCount i = 20000; i < 20900; ++i) {
        audio.channel(0)[i] = 0.99f;
    }
    const AudioBuffer before = copyOf(audio);

    DeclipSettings settings;
    settings.maximumRun = 512;
    settings.fitToCeiling = false;
    const auto report = declip(audio.view(), settings);
    REQUIRE(report);
    CHECK(report.value().tooLong >= 1);

    double worst = 0.0;
    for (SampleCount i = 20000; i < 20900; ++i) {
        worst = std::max(worst, std::abs(static_cast<double>(audio.channel(0)[i]) -
                                         static_cast<double>(before.channel(0)[i])));
    }
    CHECK(worst == 0.0);
}

TEST_CASE("A restored sample is never quieter than the one it replaces", "[dsp][declip]") {
    // The one thing genuinely known about a clipped sample: it was at least as
    // large as the ceiling it hit. A restoration that dips below that has put a
    // dent where there was a flat top, which is a worse artefact than the flat
    // top was.
    const AudioBuffer clean = material(96000);
    AudioBuffer clipped = copyOf(clean);
    clipAt(clipped, 0.5);
    const AudioBuffer before = copyOf(clipped);

    DeclipSettings settings;
    settings.fitToCeiling = false;
    REQUIRE(declip(clipped.view(), settings));

    for (SampleCount i = 0; i < clipped.frames(); ++i) {
        const double was = static_cast<double>(before.channel(0)[i]);
        const double now = static_cast<double>(clipped.channel(0)[i]);
        if (std::abs(was) >= 0.5 - 1e-6) {
            INFO("frame " << i << ": " << was << " -> " << now);
            REQUIRE(std::abs(now) >= std::abs(was) - 1e-6);
            REQUIRE((now >= 0.0) == (was >= 0.0));
        }
    }
}

TEST_CASE("A single clipped tone is reported as unrestorable, not guessed at", "[dsp][declip]") {
    // The limit, pinned so that nobody later "fixes" it by loosening the
    // self-check and starts inventing peaks instead.
    //
    // On periodic material the flat top is not damage in the signal, it *is*
    // the signal, repeated every cycle with no unclipped example anywhere for
    // the model to learn the real peak from. A model long enough to span a
    // useful fraction of the period learns it and has nothing to restore. The
    // right behaviour is then to restore nothing and say so, which is what
    // this checks -- and to show that the same tone an octave and a half lower,
    // whose period the model cannot span, comes back exactly.
    const auto clippedTone = [](double frequency) {
        AudioBuffer audio{ChannelLayout::mono(), 96000};
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            audio.channel(0)[i] = static_cast<float>(std::clamp(
                0.5 * std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / kRate),
                -0.35, 0.35));
        }
        return audio;
    };

    DeclipSettings settings;
    settings.fitToCeiling = false;

    AudioBuffer high = clippedTone(1000.0); // Period 48 samples, order 32.
    const auto highReport = declip(high.view(), settings);
    REQUIRE(highReport);
    INFO("1 kHz: " << highReport.value().runs << " runs, peak " << peakOf(high, 0));
    CHECK(highReport.value().runs == 0);
    CHECK(peakOf(high, 0) == Approx(0.35).margin(1e-5));

    AudioBuffer low = clippedTone(180.0); // Period 267 samples.
    const auto lowReport = declip(low.view(), settings);
    REQUIRE(lowReport);
    INFO("180 Hz: " << lowReport.value().runs << " runs, peak " << peakOf(low, 0));
    CHECK(lowReport.value().runs > 100);
    CHECK(peakOf(low, 0) == Approx(0.5).margin(0.01));
}

TEST_CASE("Declipping refuses what it cannot do", "[dsp][declip]") {
    AudioBuffer audio = material(48000);
    const AudioBufferView view = audio.view();

    CHECK_FALSE(declip(AudioBufferView{}).hasValue());

    DeclipSettings settings;
    settings.tolerance = 0.0;
    CHECK_FALSE(declip(view, settings).hasValue());
    settings.tolerance = 1.0;
    CHECK_FALSE(declip(view, settings).hasValue());

    settings = DeclipSettings{};
    settings.minimumRun = 0;
    CHECK_FALSE(declip(view, settings).hasValue());

    settings = DeclipSettings{};
    settings.maximumRun = kMaximumInterpolationGap + 1;
    CHECK_FALSE(declip(view, settings).hasValue());

    settings = DeclipSettings{};
    settings.order = 0;
    CHECK_FALSE(declip(view, settings).hasValue());

    settings = DeclipSettings{};
    settings.ceilingDb = 1.0;
    CHECK_FALSE(declip(view, settings).hasValue());

    settings = DeclipSettings{};
    settings.minimumRecovery = 0.0;
    CHECK_FALSE(declip(view, settings).hasValue());
}
