#include <sa/dsp/Stft.h>

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

std::vector<float> randomSignal(SampleCount n, unsigned seed = 5) {
    std::mt19937 rng{seed};
    std::uniform_real_distribution<float> dist{-0.8f, 0.8f};
    std::vector<float> signal(static_cast<std::size_t>(n));
    for (float& sample : signal) {
        sample = dist(rng);
    }
    return signal;
}

Stft makeStft(int fftSize, int hop, WindowType type = WindowType::Hann) {
    auto result = Stft::create(fftSize, hop, type);
    REQUIRE(result.hasValue());
    return std::move(result).value();
}

/// Largest absolute sample error after an untouched analyse/synthesise cycle.
double roundTripError(int fftSize, int hop, SampleCount length, WindowType type) {
    const Stft stft = makeStft(fftSize, hop, type);
    const auto signal = randomSignal(length);

    const SampleCount frames = stft.frameCount(length);
    std::vector<std::complex<float>> spectra(static_cast<std::size_t>(frames) *
                                             static_cast<std::size_t>(stft.binCount()));
    std::vector<float> restored(static_cast<std::size_t>(length));

    stft.analyse(signal.data(), length, spectra.data());
    stft.synthesise(spectra.data(), frames, restored.data(), length);

    double worst = 0.0;
    for (SampleCount i = 0; i < length; ++i) {
        const auto index = static_cast<std::size_t>(i);
        worst = std::max(worst, std::abs(static_cast<double>(restored[index] - signal[index])));
    }
    return worst;
}

} // namespace

TEST_CASE("STFT construction validates its parameters", "[dsp][stft]") {
    CHECK(Stft::create(1024, 256).hasValue());

    CHECK_FALSE(Stft::create(1000, 250).hasValue());  // fftSize not a power of two
    CHECK_FALSE(Stft::create(1024, 0).hasValue());    // zero hop
    CHECK_FALSE(Stft::create(1024, 2048).hasValue()); // hop larger than the window
    CHECK_FALSE(Stft::create(1024, 300).hasValue());  // hop does not divide fftSize
}

TEST_CASE("Frame count covers every input sample", "[dsp][stft]") {
    const Stft stft = makeStft(1024, 256);
    CHECK(stft.frameCount(0) == 0);
    CHECK(stft.binCount() == 513);

    // Every input sample must sit under a complete set of overlapping windows,
    // so the frame grid starts before sample zero and runs past the end.
    for (SampleCount length : {1, 100, 1024, 48000}) {
        const SampleCount frames = stft.frameCount(length);
        INFO("length " << length);
        CHECK(frames * stft.hopSize() >= length);
    }
}

TEST_CASE("Analysis/synthesis round-trip is transparent", "[dsp][stft][cola]") {
    // The CI gate from docs/03-architecture.md §5. Editing works on masks over
    // the analysis output, so any error here is inherited by every spectral
    // edit as an artefact -- and shows up as the metallic smear that makes
    // amateur spectral tools sound amateur.
    struct Case {
        int fftSize;
        int hop;
        SampleCount length;
    };

    for (const Case& test : {Case{1024, 256, 48000}, Case{1024, 512, 48000}, Case{2048, 512, 20000},
                             Case{512, 128, 10000}, Case{256, 64, 5000}, Case{4096, 1024, 30000}}) {
        const double error = roundTripError(test.fftSize, test.hop, test.length, WindowType::Hann);
        INFO("fftSize " << test.fftSize << " hop " << test.hop << " length " << test.length
                        << " error " << error);
        CHECK(error < 1e-4);
    }
}

TEST_CASE("Round-trip stays exact at the signal edges", "[dsp][stft][cola]") {
    // Edges are where overlap-add schemes usually leak. Front padding exists
    // precisely so the first and last samples see as many windows as the middle.
    const Stft stft = makeStft(1024, 256);
    const SampleCount length = 4096;
    const auto signal = randomSignal(length, 21);

    const SampleCount frames = stft.frameCount(length);
    std::vector<std::complex<float>> spectra(static_cast<std::size_t>(frames) *
                                             static_cast<std::size_t>(stft.binCount()));
    std::vector<float> restored(static_cast<std::size_t>(length));

    stft.analyse(signal.data(), length, spectra.data());
    stft.synthesise(spectra.data(), frames, restored.data(), length);

    for (SampleCount i : {SampleCount{0}, SampleCount{1}, SampleCount{2}, length - 2, length - 1}) {
        const auto index = static_cast<std::size_t>(i);
        INFO("edge sample " << i);
        CHECK(restored[index] == Approx(signal[index]).margin(1e-4));
    }
}

TEST_CASE("Round-trip holds for a signal shorter than one window", "[dsp][stft][cola]") {
    CHECK(roundTripError(1024, 256, 100, WindowType::Hann) < 1e-4);
    CHECK(roundTripError(1024, 256, 1, WindowType::Hann) < 1e-4);
}

TEST_CASE("Round-trip holds for windows that do not satisfy COLA on their own",
          "[dsp][stft][cola]") {
    // Weighted overlap-add divides by the summed squared window, so even
    // Blackman-Harris reconstructs exactly. A scalar normalisation would leave
    // a periodic ripple here.
    CHECK(roundTripError(1024, 256, 20000, WindowType::BlackmanHarris) < 1e-4);
    CHECK(roundTripError(1024, 128, 20000, WindowType::Hamming) < 1e-4);
}

TEST_CASE("A steady sine concentrates in its bin", "[dsp][stft]") {
    const Stft stft = makeStft(1024, 256);
    const SampleCount length = 8192;
    const int targetBin = 100;

    std::vector<float> signal(static_cast<std::size_t>(length));
    for (SampleCount i = 0; i < length; ++i) {
        signal[static_cast<std::size_t>(i)] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * targetBin * static_cast<double>(i) / 1024.0));
    }

    const SampleCount frames = stft.frameCount(length);
    const auto bins = static_cast<std::size_t>(stft.binCount());
    std::vector<std::complex<float>> spectra(static_cast<std::size_t>(frames) * bins);
    stft.analyse(signal.data(), length, spectra.data());

    // Inspect a frame well inside the signal, clear of the padded edges.
    const auto frameOffset = static_cast<std::size_t>(frames / 2) * bins;
    int loudestBin = 0;
    float loudest = 0.0f;
    for (int k = 0; k < stft.binCount(); ++k) {
        const float magnitude = std::abs(spectra[frameOffset + static_cast<std::size_t>(k)]);
        if (magnitude > loudest) {
            loudest = magnitude;
            loudestBin = k;
        }
    }
    CHECK(loudestBin == targetBin);
}

TEST_CASE("Silence analyses and resynthesises as silence", "[dsp][stft]") {
    const Stft stft = makeStft(512, 128);
    const SampleCount length = 2048;
    const std::vector<float> silence(static_cast<std::size_t>(length), 0.0f);

    const SampleCount frames = stft.frameCount(length);
    std::vector<std::complex<float>> spectra(static_cast<std::size_t>(frames) *
                                             static_cast<std::size_t>(stft.binCount()));
    std::vector<float> restored(static_cast<std::size_t>(length), 1.0f);

    stft.analyse(silence.data(), length, spectra.data());
    stft.synthesise(spectra.data(), frames, restored.data(), length);

    for (SampleCount i = 0; i < length; ++i) {
        REQUIRE(restored[static_cast<std::size_t>(i)] == Approx(0.0).margin(1e-6));
    }
}
