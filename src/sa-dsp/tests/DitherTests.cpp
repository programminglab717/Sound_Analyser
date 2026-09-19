#include <sa/dsp/Dither.h>

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

/// Quantise the way the writer does, so a test measures the whole path rather
/// than the ditherer in isolation.
[[nodiscard]] double quantise(double value, int bits) {
    const double scale = std::pow(2.0, bits - 1);
    return std::round(std::clamp(value, -1.0, 1.0) * scale) / scale;
}

[[nodiscard]] AudioBuffer constant(double value, SampleCount frames, int channels = 1) {
    AudioBuffer audio{ChannelLayout::discrete(channels), frames};
    for (int channel = 0; channel < channels; ++channel) {
        std::fill_n(audio.channel(channel), frames, static_cast<float>(value));
    }
    return audio;
}

[[nodiscard]] double meanOf(const AudioBuffer& audio, int bits) {
    double total = 0.0;
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        total += quantise(audio.channel(0)[i], bits);
    }
    return total / static_cast<double>(audio.frames());
}

} // namespace

TEST_CASE("Dither removes the bias that rounding leaves on a constant") {
    // The defining property, and the easiest to state: a value sitting a third
    // of the way between two codes rounds to the nearer one every single time,
    // so its average comes out at the code rather than at the value. Dithered,
    // the average is the value -- which is to say the signal survives below the
    // least significant bit.
    constexpr int kBits = 8;
    const double step = 1.0 / std::pow(2.0, kBits - 1);
    const double value = 5.0 * step + step / 3.0;

    AudioBuffer undithered = constant(value, 200000);
    REQUIRE(meanOf(undithered, kBits) == Approx(5.0 * step).margin(1e-12));
    REQUIRE(std::abs(meanOf(undithered, kBits) - value) > step / 4.0);

    AudioBuffer dithered = constant(value, 200000);
    auto ditherer = Ditherer::create({DitherType::Tpdf, kBits, 1}, 1);
    REQUIRE(ditherer);
    ditherer.value().process(dithered.view());
    REQUIRE(meanOf(dithered, kBits) == Approx(value).margin(step / 50.0));
}

TEST_CASE("Dither decorrelates the error from the signal") {
    // The other half of what dither is for. Undithered, the error is a
    // function of the signal, so the two correlate strongly; dithered, the
    // error is noise and the correlation collapses.
    constexpr int kBits = 6;
    constexpr SampleCount kFrames = 100000;

    const auto errorCorrelation = [&](DitherType type) {
        AudioBuffer audio{ChannelLayout::mono(), kFrames};
        std::vector<double> original(static_cast<std::size_t>(kFrames));
        for (SampleCount i = 0; i < kFrames; ++i) {
            // Deliberately quiet: at a few codes of amplitude the distortion
            // products are enormous relative to the signal, which is exactly
            // the case a fading tail runs into.
            original[static_cast<std::size_t>(i)] =
                0.02 * std::sin(2.0 * std::numbers::pi * 997.0 * static_cast<double>(i) / 48000.0);
            audio.channel(0)[i] = static_cast<float>(original[static_cast<std::size_t>(i)]);
        }

        auto ditherer = Ditherer::create({type, kBits, 1}, 1);
        REQUIRE(ditherer);
        ditherer.value().process(audio.view());

        double sumSignal = 0.0;
        double sumError = 0.0;
        double sumProduct = 0.0;
        for (SampleCount i = 0; i < kFrames; ++i) {
            const double signal = original[static_cast<std::size_t>(i)];
            const double error = quantise(audio.channel(0)[i], kBits) - signal;
            sumSignal += signal * signal;
            sumError += error * error;
            sumProduct += signal * error;
        }
        return std::abs(sumProduct) / std::sqrt(sumSignal * sumError);
    };

    const double plain = errorCorrelation(DitherType::None);
    const double dithered = errorCorrelation(DitherType::Tpdf);
    REQUIRE(plain > 0.2);
    REQUIRE(dithered < plain / 5.0);
}

TEST_CASE("Noise shaping moves the noise up the band, not away") {
    // What a shaper does and does not do. It cannot remove noise -- it moves
    // it, and in total there is more of it. The test says both.
    constexpr int kBits = 8;
    constexpr SampleCount kFrames = 65536;

    const auto errorSpectrumSplit = [&](DitherType type) {
        AudioBuffer audio{ChannelLayout::mono(), kFrames};
        // Silence, so the error is the whole output and nothing else is in it.
        auto ditherer = Ditherer::create({type, kBits, 1}, 1);
        REQUIRE(ditherer);
        ditherer.value().process(audio.view());

        // Energy below and above a quarter of the sample rate, by the simplest
        // means available: a one-pole split is enough to say which half the
        // noise is in, and avoids pulling an FFT into this test.
        double low = 0.0;
        double high = 0.0;
        double smoothed = 0.0;
        for (SampleCount i = 0; i < kFrames; ++i) {
            const double value = quantise(audio.channel(0)[i], kBits);
            smoothed += 0.2 * (value - smoothed);
            low += smoothed * smoothed;
            const double residual = value - smoothed;
            high += residual * residual;
        }
        return std::pair{low, high};
    };

    const auto [flatLow, flatHigh] = errorSpectrumSplit(DitherType::Tpdf);
    const auto [shapedLow, shapedHigh] = errorSpectrumSplit(DitherType::TpdfNoiseShaped);

    // Less noise down where the ear is, more of it up where it is not.
    REQUIRE(shapedLow < flatLow);
    REQUIRE(shapedHigh > flatHigh);
    // And more in total, which is the trade being made rather than a fault.
    REQUIRE(shapedLow + shapedHigh > flatLow + flatHigh);
}

TEST_CASE("The noise is about one code, and scales with the depth") {
    for (const int bits : {8, 16, 24}) {
        const double step = 1.0 / std::pow(2.0, bits - 1);
        AudioBuffer audio = constant(0.0, 50000);
        auto ditherer = Ditherer::create({DitherType::Tpdf, bits, 1}, 1);
        REQUIRE(ditherer);
        REQUIRE(ditherer.value().step() == Approx(step));
        ditherer.value().process(audio.view());

        double sumOfSquares = 0.0;
        double largest = 0.0;
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            const double value = audio.channel(0)[i];
            sumOfSquares += value * value;
            largest = std::max(largest, std::abs(value));
        }
        // Triangular over [-1, 1] codes has variance 1/6 of a code squared.
        const double rms = std::sqrt(sumOfSquares / static_cast<double>(audio.frames()));
        REQUIRE(rms == Approx(step / std::sqrt(6.0)).epsilon(0.05));
        // And never more than two codes from centre, by construction.
        REQUIRE(largest <= step * 1.0001);
    }
}

TEST_CASE("None leaves the samples untouched, bit for bit") {
    AudioBuffer audio = constant(0.3, 1000, 2);
    auto ditherer = Ditherer::create({DitherType::None, 16, 1}, 2);
    REQUIRE(ditherer);
    ditherer.value().process(audio.view());
    for (int channel = 0; channel < 2; ++channel) {
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            REQUIRE(audio.channel(channel)[i] == 0.3f);
        }
    }
}

TEST_CASE("The same seed gives the same file twice") {
    // An export written twice has to be the same export, or a checksum means
    // nothing and neither does a regression test.
    const auto run = [] {
        AudioBuffer audio = constant(0.1, 5000);
        auto ditherer = Ditherer::create({DitherType::Tpdf, 16, 1}, 1);
        REQUIRE(ditherer);
        ditherer.value().process(audio.view());
        std::vector<float> out(static_cast<std::size_t>(audio.frames()));
        std::copy_n(audio.channel(0), audio.frames(), out.begin());
        return out;
    };
    REQUIRE(run() == run());

    // And a different seed gives something different, or the seed is not a seed.
    AudioBuffer other = constant(0.1, 5000);
    auto elsewhere = Ditherer::create({DitherType::Tpdf, 16, 12345}, 1);
    REQUIRE(elsewhere);
    elsewhere.value().process(other.view());
    const std::vector<float> first = run();
    bool anyDifferent = false;
    for (SampleCount i = 0; i < other.frames(); ++i) {
        anyDifferent = anyDifferent || other.channel(0)[i] != first[static_cast<std::size_t>(i)];
    }
    REQUIRE(anyDifferent);
}

TEST_CASE("Block size cannot be seen in the result") {
    const auto run = [](SampleCount blockSize) {
        AudioBuffer audio = constant(0.1, 4096, 2);
        auto ditherer = Ditherer::create({DitherType::TpdfNoiseShaped, 16, 7}, 2);
        REQUIRE(ditherer);
        for (SampleCount at = 0; at < audio.frames();) {
            const SampleCount take = std::min(blockSize, audio.frames() - at);
            ditherer.value().process(audio.view().subRange(at, take));
            at += take;
        }
        std::vector<float> out;
        for (int channel = 0; channel < 2; ++channel) {
            out.insert(out.end(), audio.channel(channel), audio.channel(channel) + audio.frames());
        }
        return out;
    };
    REQUIRE(run(4096) == run(37));
}

TEST_CASE("Dither cannot push a sample past full scale") {
    AudioBuffer audio = constant(1.0, 10000);
    auto ditherer = Ditherer::create({DitherType::Tpdf, 16, 1}, 1);
    REQUIRE(ditherer);
    ditherer.value().process(audio.view());
    for (SampleCount i = 0; i < audio.frames(); ++i) {
        REQUIRE(audio.channel(0)[i] <= 1.0f);
        REQUIRE(audio.channel(0)[i] >= -1.0f);
    }
}

TEST_CASE("Impossible settings are refused") {
    REQUIRE_FALSE(Ditherer::create({DitherType::Tpdf, 16, 1}, 0));
    REQUIRE_FALSE(Ditherer::create({DitherType::Tpdf, 1, 1}, 1));
    REQUIRE_FALSE(Ditherer::create({DitherType::Tpdf, 33, 1}, 1));
    REQUIRE(Ditherer::create({DitherType::Tpdf, 2, 1}, 1));
    REQUIRE(Ditherer::create({DitherType::Tpdf, 32, 1}, 1));
}
