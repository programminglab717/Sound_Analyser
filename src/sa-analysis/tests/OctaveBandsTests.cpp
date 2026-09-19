#include <sa/analysis/OctaveBands.h>

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>

using namespace sa;
using namespace sa::analysis;
using Catch::Approx;

namespace {

constexpr SampleRate kRate{48000.0};

[[nodiscard]] AudioBuffer tone(double hz, double amplitude = 1.0, SampleCount frames = 48000 * 2) {
    AudioBuffer audio{ChannelLayout::mono(), frames};
    for (SampleCount i = 0; i < frames; ++i) {
        audio.channel(0)[i] =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz *
                                                    static_cast<double>(i) / kRate.hz()));
    }
    return audio;
}

[[nodiscard]] const Band& bandAt(const std::vector<Band>& bands, double centre) {
    const auto found = std::find_if(bands.begin(), bands.end(), [centre](const Band& band) {
        return std::abs(band.centreHz - centre) < 0.51;
    });
    REQUIRE(found != bands.end());
    return *found;
}

} // namespace

TEST_CASE("The third-octave layout is the series everyone writes on an axis") {
    const std::vector<Band> bands = bandLayout(kRate);
    // All thirty-one, 20 Hz to 20 kHz: the top band reaches 22449 Hz, which
    // still fits under Nyquist at 48 kHz.
    REQUIRE(bands.size() == 31);
    REQUIRE(bands.front().centreHz == Approx(20.0));
    REQUIRE(bands.back().centreHz == Approx(20000.0));

    // Adjacent centres are a third of an octave apart, and each band's top is
    // the next one's bottom to within the rounding of the nominal series.
    for (std::size_t i = 1; i < bands.size(); ++i) {
        const double ratio = bands[i].centreHz / bands[i - 1].centreHz;
        REQUIRE(ratio == Approx(std::exp2(1.0 / 3.0)).epsilon(0.02));
        REQUIRE(bands[i].lowHz == Approx(bands[i - 1].highHz).epsilon(0.02));
    }
}

TEST_CASE("The octave layout is ten bands an octave apart") {
    OctaveBandSettings settings;
    settings.width = BandWidth::Octave;
    const std::vector<Band> bands = bandLayout(kRate, settings);
    REQUIRE(bands.size() == 10);
    for (std::size_t i = 1; i < bands.size(); ++i) {
        REQUIRE(bands[i].centreHz == Approx(2.0 * bands[i - 1].centreHz).epsilon(0.02));
    }
}

TEST_CASE("A band never extends past Nyquist") {
    for (const double hz : {8000.0, 22050.0, 44100.0, 48000.0, 96000.0}) {
        const std::vector<Band> bands = bandLayout(SampleRate{hz});
        for (const Band& band : bands) {
            REQUIRE(band.highHz <= hz * 0.5);
        }
    }
    // An 8 kHz file cannot show the top of the range, and says so by leaving
    // those bands out rather than reporting them as quiet.
    REQUIRE(bandLayout(SampleRate{8000.0}).size() < bandLayout(kRate).size());
}

TEST_CASE("A tone lands in its own band and nowhere else") {
    for (const double hz : {100.0, 1000.0, 5000.0}) {
        const AudioBuffer audio = tone(hz);
        const auto measured = measureBands(audio.view(), kRate);
        REQUIRE(measured);

        const Band& home = bandAt(measured.value(), hz);
        REQUIRE(home.levelDb == Approx(0.0).margin(0.5));

        // Every other band is far below it. A Hann window's skirts reach the
        // neighbours, so the bar is 25 dB rather than silence.
        for (const Band& band : measured.value()) {
            if (std::abs(band.centreHz - hz) < 0.51) {
                continue;
            }
            REQUIRE(band.levelDb < home.levelDb - 25.0);
        }
    }
}

TEST_CASE("A full-scale sine reads 0 dBFS whatever band it is in") {
    // The normalisation that makes two measurements comparable, and the one a
    // reader assumes without being told. A band at the top covers hundreds of
    // bins and a band at the bottom covers three; neither may change the
    // answer.
    for (const double hz : {63.0, 1000.0, 8000.0}) {
        const AudioBuffer audio = tone(hz, 1.0);
        const auto measured = measureBands(audio.view(), kRate);
        REQUIRE(measured);
        REQUIRE(bandAt(measured.value(), hz).levelDb == Approx(0.0).margin(0.6));
    }
}

TEST_CASE("Halving the amplitude takes every band down 6 dB") {
    std::mt19937 engine{3};
    std::normal_distribution<float> noise{0.0f, 0.1f};
    AudioBuffer loud{ChannelLayout::mono(), 48000 * 2};
    AudioBuffer quiet{ChannelLayout::mono(), 48000 * 2};
    for (SampleCount i = 0; i < loud.frames(); ++i) {
        const float value = noise(engine);
        loud.channel(0)[i] = value;
        quiet.channel(0)[i] = 0.5f * value;
    }

    const auto a = measureBands(loud.view(), kRate);
    const auto b = measureBands(quiet.view(), kRate);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a.value().size() == b.value().size());
    for (std::size_t i = 0; i < a.value().size(); ++i) {
        REQUIRE(a.value()[i].levelDb - b.value()[i].levelDb == Approx(6.0206).margin(0.01));
    }
}

TEST_CASE("White noise rises 1 dB per third-octave band") {
    // The classic reason bands and bins are not the same picture. White noise
    // is flat per hertz, and a third-octave band is 23 per cent wider than the
    // one below it, so on a band display white noise rises by
    // 10*log10(2^(1/3)) = 1.0 dB a band. An FFT shows it flat. Neither is
    // wrong and they answer different questions.
    std::mt19937 engine{11};
    std::normal_distribution<float> noise{0.0f, 0.1f};
    AudioBuffer audio{ChannelLayout::mono(), 48000 * 4};
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        audio.channel(0)[i] = noise(engine);
    }

    const auto measured = measureBands(audio.view(), kRate);
    REQUIRE(measured);

    // Across the middle of the range, where there are plenty of bins per band
    // and the answer is not dominated by the count being small.
    const Band& low = bandAt(measured.value(), 500.0);
    const Band& high = bandAt(measured.value(), 5000.0);
    const double bandsBetween = 10.0; // 500 to 5000 is ten third-octaves.
    const double perBand = (high.levelDb - low.levelDb) / bandsBetween;
    REQUIRE(perBand == Approx(10.0 * std::log10(std::exp2(1.0 / 3.0))).margin(0.15));
}

TEST_CASE("Silence reads at the floor rather than at nothing") {
    AudioBuffer audio{ChannelLayout::mono(), 48000};
    const auto measured = measureBands(audio.view(), kRate);
    REQUIRE(measured);
    for (const Band& band : measured.value()) {
        REQUIRE(band.levelDb <= -100.0);
        REQUIRE(std::isfinite(band.levelDb));
    }
}

TEST_CASE("Audio too short for one window gives the layout and no levels") {
    AudioBuffer audio = tone(1000.0, 1.0, 100);
    const auto measured = measureBands(audio.view(), kRate);
    REQUIRE(measured);
    REQUIRE(measured.value().size() == bandLayout(kRate).size());
    for (const Band& band : measured.value()) {
        REQUIRE(band.levelDb == kDecibelFloor);
    }
}

TEST_CASE("An empty buffer is the layout alone") {
    AudioBuffer audio{ChannelLayout::mono(), 0};
    const auto measured = measureBands(audio.view(), kRate);
    REQUIRE(measured);
    REQUIRE(measured.value().size() == bandLayout(kRate).size());
}
