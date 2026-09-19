#include <sa/dsp/Dehum.h>

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

/// Notes, not tones.
///
/// The distinction is the whole test. Hum is a steady sinusoid, and a steady
/// synthetic tone is one too -- so material made of held sine waves cannot be
/// told apart from hum by this or any other method, and testing against it
/// would be testing the wrong thing. These notes have an attack, a decay and a
/// little vibrato, which is what makes them music.
[[nodiscard]] AudioBuffer notes(SampleCount frames, int channels = 1, unsigned seed = 5) {
    static constexpr double kNotes[] = {293.0, 392.0, 349.0, 440.0, 330.0, 494.0};
    std::mt19937 engine{seed};
    std::normal_distribution<float> hiss{0.0f, 0.004f};

    AudioBuffer buffer{channels == 1 ? ChannelLayout::mono() : ChannelLayout::stereo(), frames};
    const SampleCount each = std::max<SampleCount>(1, frames / 6);
    for (int channel = 0; channel < channels; ++channel) {
        float* out = buffer.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            const auto which = std::min<SampleCount>(5, i / each);
            const double into = static_cast<double>(i - which * each) / static_cast<double>(each);
            const double envelope = std::min(1.0, into * 30.0) * std::exp(-2.2 * into);
            const double frequency =
                kNotes[which] * (1.0 + 0.004 * std::sin(2.0 * std::numbers::pi * 5.2 *
                                                        static_cast<double>(i) / kRate));
            double value = 0.0;
            for (int k = 1; k <= 4; ++k) {
                value += (0.10 / k) *
                         std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(k) *
                                      static_cast<double>(i) / kRate +
                                  0.6 * static_cast<double>(k));
            }
            out[i] = static_cast<float>(envelope * value + hiss(engine));
        }
    }
    return buffer;
}

void addHum(AudioBuffer& audio, double fundamental, double level, int harmonics) {
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        float* out = audio.channel(channel);
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            double value = 0.0;
            for (int k = 1; k <= harmonics; ++k) {
                value +=
                    (level / static_cast<double>(k)) *
                    std::sin(2.0 * std::numbers::pi * fundamental * static_cast<double>(k) * t +
                             0.3 * static_cast<double>(k));
            }
            out[i] += static_cast<float>(value);
        }
    }
}

[[nodiscard]] AudioBuffer copyOf(const AudioBuffer& source) {
    AudioBuffer copy{source.layout(), source.frames()};
    for (int channel = 0; channel < source.channelCount(); ++channel) {
        std::copy_n(source.channel(channel), source.frames(), copy.channel(channel));
    }
    return copy;
}

[[nodiscard]] double levelAt(const AudioBuffer& audio, double frequency, int channel = 0) {
    double real = 0.0;
    double imaginary = 0.0;
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        const double angle = 2.0 * std::numbers::pi * frequency * static_cast<double>(i) / kRate;
        real += static_cast<double>(audio.channel(channel)[i]) * std::cos(angle);
        imaginary += static_cast<double>(audio.channel(channel)[i]) * std::sin(angle);
    }
    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(audio.frames());
}

[[nodiscard]] double errorDb(const AudioBuffer& actual, const AudioBuffer& wanted) {
    double error = 0.0;
    double signal = 0.0;
    for (int channel = 0; channel < wanted.channelCount(); ++channel) {
        for (SampleCount i = 0; i < wanted.frames(); ++i) {
            const double difference = static_cast<double>(actual.channel(channel)[i]) -
                                      static_cast<double>(wanted.channel(channel)[i]);
            error += difference * difference;
            signal += static_cast<double>(wanted.channel(channel)[i]) *
                      static_cast<double>(wanted.channel(channel)[i]);
        }
    }
    return error > 0.0 && signal > 0.0 ? 10.0 * std::log10(error / signal) : -200.0;
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

constexpr SampleCount kLength = static_cast<SampleCount>(4.0 * kRate);

} // namespace

TEST_CASE("Hum is found and taken out", "[dsp][dehum]") {
    const AudioBuffer clean = notes(kLength);
    AudioBuffer hummy = copyOf(clean);
    addHum(hummy, 50.0, 0.02, 40);

    AudioBuffer cleaned = copyOf(hummy);
    const auto report = dehum(cleaned.view(), SampleRate{kRate});
    REQUIRE(report);
    REQUIRE(report.value().found);

    INFO("found " << report.value().frequency << " Hz, " << report.value().harmonics
                  << " partials, prominence " << report.value().prominence);
    CHECK(report.value().frequency == Approx(50.0).margin(0.02));

    const double before = levelAt(hummy, 50.0);
    const double after = levelAt(cleaned, 50.0);
    INFO("50 Hz: " << before << " -> " << after);
    CHECK(after < before * 0.05); // At least 26 dB down.

    const double errorBefore = errorDb(hummy, clean);
    const double errorAfter = errorDb(cleaned, clean);
    INFO("error against the clean original " << errorBefore << " dB -> " << errorAfter << " dB");
    CHECK(errorAfter < errorBefore - 5.0);
}

TEST_CASE("Sixty is not mistaken for fifty", "[dsp][dehum]") {
    const AudioBuffer clean = notes(kLength);
    AudioBuffer hummy = copyOf(clean);
    addHum(hummy, 60.0, 0.02, 30);

    const auto report = dehum(hummy.view(), SampleRate{kRate});
    REQUIRE(report);
    REQUIRE(report.value().found);
    INFO("found " << report.value().frequency);
    CHECK(report.value().frequency == Approx(60.0).margin(0.05));
}

TEST_CASE("A supply that is not exactly nominal is still found", "[dsp][dehum]") {
    // Grids run a few hundredths of a Hertz off nominal, and by the fortieth
    // harmonic a tenth of a Hertz is four -- a different partial. So the search
    // has to be precise, not a choice between two numbers.
    const AudioBuffer clean = notes(kLength);
    AudioBuffer hummy = copyOf(clean);
    addHum(hummy, 49.93, 0.02, 40);

    const auto report = dehum(hummy.view(), SampleRate{kRate});
    REQUIRE(report);
    REQUIRE(report.value().found);
    INFO("found " << report.value().frequency);
    CHECK(report.value().frequency == Approx(49.93).margin(0.02));
}

TEST_CASE("A recording with no hum is left exactly alone", "[dsp][dehum]") {
    AudioBuffer audio = notes(kLength);
    const AudioBuffer before = copyOf(audio);

    const auto report = dehum(audio.view(), SampleRate{kRate});
    REQUIRE(report);
    INFO("prominence " << report.value().prominence << " at " << report.value().frequency);
    CHECK_FALSE(report.value().found);
    CHECK(worstDifference(audio, before) == 0.0);
}

TEST_CASE("Being told the frequency does not override the evidence", "[dsp][dehum]") {
    // Naming a frequency says where to look, not what to remove. Each partial
    // still has to be a steady sinusoid standing above its surroundings before
    // anything is subtracted -- otherwise pointing the tool at a clean file
    // would carve forty notches in it.
    AudioBuffer audio = notes(kLength);
    const AudioBuffer before = copyOf(audio);

    DehumSettings settings;
    settings.frequency = 50.0;
    const auto report = dehum(audio.view(), SampleRate{kRate}, settings);
    REQUIRE(report);
    INFO("removed " << report.value().harmonics << " partials from clean material");
    CHECK(report.value().harmonics == 0);
    CHECK(worstDifference(audio, before) == 0.0);
}

TEST_CASE("A held synthetic tone is hum, and that is not a bug", "[dsp][dehum]") {
    // Pinned deliberately. What distinguishes hum from music is steadiness, so
    // a perfectly steady tone at the mains frequency *is* hum by every
    // available test -- there is no property left to separate them. Anyone who
    // meets this in a synthetic test file should know it is the signal that is
    // unusual, not the tool.
    AudioBuffer audio{ChannelLayout::mono(), kLength};
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        audio.channel(0)[i] = static_cast<float>(
            0.2 * std::sin(2.0 * std::numbers::pi * 100.0 * static_cast<double>(i) / kRate));
    }

    DehumSettings settings;
    settings.frequency = 50.0;
    const auto report = dehum(audio.view(), SampleRate{kRate}, settings);
    REQUIRE(report);
    INFO("removed " << report.value().harmonics << " partials from a held 100 Hz tone");
    CHECK(report.value().harmonics >= 1);
    CHECK(levelAt(audio, 100.0) < 0.02);
}

TEST_CASE("Both channels are cleaned", "[dsp][dehum]") {
    const AudioBuffer clean = notes(kLength, 2);
    AudioBuffer hummy = copyOf(clean);
    addHum(hummy, 50.0, 0.02, 40);

    const auto report = dehum(hummy.view(), SampleRate{kRate});
    REQUIRE(report);
    REQUIRE(report.value().found);

    for (int channel = 0; channel < 2; ++channel) {
        INFO("channel " << channel);
        CHECK(levelAt(hummy, 50.0, channel) < 0.002);
    }
}

TEST_CASE("De-humming refuses what it cannot do", "[dsp][dehum]") {
    AudioBuffer audio = notes(48000);
    const AudioBufferView view = audio.view();

    CHECK_FALSE(dehum(AudioBufferView{}, SampleRate{kRate}).hasValue());
    CHECK_FALSE(dehum(view, SampleRate{0.0}).hasValue());

    DehumSettings settings;
    settings.frequency = -1.0;
    CHECK_FALSE(dehum(view, SampleRate{kRate}, settings).hasValue());
    settings.frequency = 30000.0;
    CHECK_FALSE(dehum(view, SampleRate{kRate}, settings).hasValue());

    settings = DehumSettings{};
    settings.harmonics = -1;
    CHECK_FALSE(dehum(view, SampleRate{kRate}, settings).hasValue());

    settings = DehumSettings{};
    settings.blockSize = 64;
    CHECK_FALSE(dehum(view, SampleRate{kRate}, settings).hasValue());

    settings = DehumSettings{};
    settings.amount = 1.5;
    CHECK_FALSE(dehum(view, SampleRate{kRate}, settings).hasValue());

    settings = DehumSettings{};
    settings.highestHarmonicHz = 0.0;
    CHECK_FALSE(dehum(view, SampleRate{kRate}, settings).hasValue());
}
