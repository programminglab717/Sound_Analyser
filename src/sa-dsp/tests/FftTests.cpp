#include <sa/dsp/Fft.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

using namespace sa::dsp;
using Catch::Approx;

namespace {

/// Textbook O(N^2) DFT. Slow, obviously correct, and the only honest way to
/// show the fast transform agrees with the definition rather than merely with
/// itself.
std::vector<std::complex<double>> naiveDft(const std::vector<std::complex<double>>& input) {
    const auto n = input.size();
    std::vector<std::complex<double>> output(n);
    for (std::size_t k = 0; k < n; ++k) {
        std::complex<double> sum{0.0, 0.0};
        for (std::size_t i = 0; i < n; ++i) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(k) *
                                 static_cast<double>(i) / static_cast<double>(n);
            sum += input[i] * std::complex<double>{std::cos(angle), std::sin(angle)};
        }
        output[k] = sum;
    }
    return output;
}

std::vector<float> randomSignal(int n, unsigned seed = 42) {
    std::mt19937 rng{seed};
    std::uniform_real_distribution<float> dist{-1.0f, 1.0f};
    std::vector<float> signal(static_cast<std::size_t>(n));
    for (float& sample : signal) {
        sample = dist(rng);
    }
    return signal;
}

} // namespace

TEST_CASE("FFT sizes must be powers of two", "[dsp][fft]") {
    CHECK(ComplexFft::isSupportedSize(2));
    CHECK(ComplexFft::isSupportedSize(1024));
    CHECK_FALSE(ComplexFft::isSupportedSize(1000));
    CHECK_FALSE(ComplexFft::isSupportedSize(0));
    CHECK_FALSE(ComplexFft::isSupportedSize(-8));

    CHECK(RealFft::isSupportedSize(4));
    CHECK_FALSE(RealFft::isSupportedSize(2));
}

TEST_CASE("Complex FFT agrees with the DFT definition", "[dsp][fft]") {
    for (int size : {4, 8, 16, 64, 256}) {
        const auto signal = randomSignal(size, static_cast<unsigned>(size));

        std::vector<std::complex<double>> reference(static_cast<std::size_t>(size));
        std::vector<std::complex<float>> actual(static_cast<std::size_t>(size));
        for (int i = 0; i < size; ++i) {
            const auto index = static_cast<std::size_t>(i);
            reference[index] = std::complex<double>{signal[index], 0.0};
            actual[index] = std::complex<float>{signal[index], 0.0f};
        }

        const auto expected = naiveDft(reference);
        ComplexFft{size}.forward(actual.data());

        for (int k = 0; k < size; ++k) {
            const auto index = static_cast<std::size_t>(k);
            INFO("size " << size << " bin " << k);
            CHECK(actual[index].real() == Approx(expected[index].real()).margin(1e-3));
            CHECK(actual[index].imag() == Approx(expected[index].imag()).margin(1e-3));
        }
    }
}

TEST_CASE("Complex FFT round-trips", "[dsp][fft]") {
    for (int size : {8, 64, 1024}) {
        const auto signal = randomSignal(size, 7);
        std::vector<std::complex<float>> data(static_cast<std::size_t>(size));
        for (int i = 0; i < size; ++i) {
            data[static_cast<std::size_t>(i)] =
                std::complex<float>{signal[static_cast<std::size_t>(i)], 0.0f};
        }

        const ComplexFft fft{size};
        fft.forward(data.data());
        fft.inverse(data.data());

        for (int i = 0; i < size; ++i) {
            const auto index = static_cast<std::size_t>(i);
            INFO("size " << size << " sample " << i);
            CHECK(data[index].real() == Approx(signal[index]).margin(1e-5));
            CHECK(data[index].imag() == Approx(0.0).margin(1e-5));
        }
    }
}

TEST_CASE("Real FFT agrees with the complex FFT", "[dsp][fft]") {
    // The real transform packs pairs into a half-length complex transform and
    // untangles. An error in the untangling is invisible unless it is checked
    // against the full-length result.
    for (int size : {4, 8, 16, 64, 256, 1024}) {
        const auto signal = randomSignal(size, static_cast<unsigned>(size * 3));

        std::vector<std::complex<float>> full(static_cast<std::size_t>(size));
        for (int i = 0; i < size; ++i) {
            full[static_cast<std::size_t>(i)] =
                std::complex<float>{signal[static_cast<std::size_t>(i)], 0.0f};
        }
        ComplexFft{size}.forward(full.data());

        const RealFft realFft{size};
        std::vector<std::complex<float>> bins(static_cast<std::size_t>(realFft.binCount()));
        realFft.forward(signal.data(), bins.data());

        for (int k = 0; k < realFft.binCount(); ++k) {
            const auto index = static_cast<std::size_t>(k);
            INFO("size " << size << " bin " << k);
            CHECK(bins[index].real() == Approx(full[index].real()).margin(1e-3));
            CHECK(bins[index].imag() == Approx(full[index].imag()).margin(1e-3));
        }
    }
}

TEST_CASE("Real FFT round-trips", "[dsp][fft]") {
    for (int size : {4, 16, 256, 2048}) {
        const auto signal = randomSignal(size, 11);
        const RealFft fft{size};

        std::vector<std::complex<float>> bins(static_cast<std::size_t>(fft.binCount()));
        std::vector<float> restored(static_cast<std::size_t>(size));

        fft.forward(signal.data(), bins.data());
        fft.inverse(bins.data(), restored.data());

        for (int i = 0; i < size; ++i) {
            const auto index = static_cast<std::size_t>(i);
            INFO("size " << size << " sample " << i);
            CHECK(restored[index] == Approx(signal[index]).margin(1e-4));
        }
    }
}

TEST_CASE("DC and Nyquist bins are real", "[dsp][fft]") {
    const int size = 64;
    const RealFft fft{size};
    std::vector<float> signal(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i) {
        signal[static_cast<std::size_t>(i)] = (i % 2 == 0) ? 1.0f : -1.0f;
    }

    std::vector<std::complex<float>> bins(static_cast<std::size_t>(fft.binCount()));
    fft.forward(signal.data(), bins.data());

    CHECK(bins[0].imag() == Approx(0.0).margin(1e-5));
    CHECK(bins[static_cast<std::size_t>(size / 2)].imag() == Approx(0.0).margin(1e-5));

    // Alternating +/-1 is exactly Nyquist: all energy in the last bin.
    CHECK(bins[0].real() == Approx(0.0).margin(1e-4));
    CHECK(bins[static_cast<std::size_t>(size / 2)].real() == Approx(64.0).margin(1e-3));
}

TEST_CASE("A bin-centred sine lands in exactly one bin", "[dsp][fft]") {
    const int size = 1024;
    const int targetBin = 64;
    const RealFft fft{size};

    std::vector<float> signal(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i) {
        signal[static_cast<std::size_t>(i)] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * targetBin * i / static_cast<double>(size)));
    }

    std::vector<std::complex<float>> bins(static_cast<std::size_t>(fft.binCount()));
    fft.forward(signal.data(), bins.data());

    for (int k = 0; k < fft.binCount(); ++k) {
        const float magnitude = std::abs(bins[static_cast<std::size_t>(k)]);
        INFO("bin " << k);
        if (k == targetBin) {
            CHECK(magnitude == Approx(static_cast<float>(size) / 2.0f).epsilon(1e-3));
        } else {
            CHECK(magnitude < 1e-2f);
        }
    }
}
