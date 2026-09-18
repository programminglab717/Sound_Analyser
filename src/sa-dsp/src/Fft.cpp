#include <sa/dsp/Fft.h>

#include <cassert>
#include <cmath>
#include <numbers>

namespace sa::dsp {

namespace {

std::uint32_t reverseBits(std::uint32_t value, int bits) noexcept {
    std::uint32_t result = 0;
    for (int i = 0; i < bits; ++i) {
        result = (result << 1) | (value & 1U);
        value >>= 1;
    }
    return result;
}

int log2Exact(int value) noexcept {
    int bits = 0;
    while ((1 << bits) < value) {
        ++bits;
    }
    return bits;
}

} // namespace

ComplexFft::ComplexFft(int size) : size_(size) {
    assert(isSupportedSize(size));

    const int bits = log2Exact(size);
    bitReversal_.resize(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i) {
        bitReversal_[static_cast<std::size_t>(i)] =
            reverseBits(static_cast<std::uint32_t>(i), bits);
    }

    // Twiddles for every stage, laid out stage by stage so the inner loop reads
    // them sequentially.
    twiddles_.resize(static_cast<std::size_t>(size / 2));
    for (int i = 0; i < size / 2; ++i) {
        const double angle =
            -2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size);
        twiddles_[static_cast<std::size_t>(i)] = std::complex<float>{
            static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
}

void ComplexFft::transform(std::complex<float>* data, bool conjugate) const noexcept {
    // Bit-reversal permutation, then in-place Cooley-Tukey butterflies.
    for (int i = 0; i < size_; ++i) {
        const auto j = static_cast<int>(bitReversal_[static_cast<std::size_t>(i)]);
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    for (int length = 2; length <= size_; length *= 2) {
        const int half = length / 2;
        const int step = size_ / length;

        for (int start = 0; start < size_; start += length) {
            for (int offset = 0; offset < half; ++offset) {
                std::complex<float> twiddle = twiddles_[static_cast<std::size_t>(offset * step)];
                if (conjugate) {
                    twiddle = std::conj(twiddle);
                }

                const std::complex<float> upper = data[start + offset];
                const std::complex<float> lower = data[start + offset + half] * twiddle;
                data[start + offset] = upper + lower;
                data[start + offset + half] = upper - lower;
            }
        }
    }
}

void ComplexFft::forward(std::complex<float>* data) const noexcept {
    transform(data, false);
}

void ComplexFft::inverse(std::complex<float>* data) const noexcept {
    transform(data, true);
    const float scale = 1.0f / static_cast<float>(size_);
    for (int i = 0; i < size_; ++i) {
        data[i] *= scale;
    }
}

RealFft::RealFft(int size) : size_(size), half_(size / 2) {
    assert(isSupportedSize(size));

    const int halfSize = size / 2;
    twiddles_.resize(static_cast<std::size_t>(halfSize + 1));
    for (int i = 0; i <= halfSize; ++i) {
        const double angle =
            -2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size);
        twiddles_[static_cast<std::size_t>(i)] = std::complex<float>{
            static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
    scratch_.resize(static_cast<std::size_t>(halfSize));
}

void RealFft::forward(const float* input, std::complex<float>* output) const {
    const int halfSize = size_ / 2;

    // Pack consecutive real pairs as the real and imaginary parts of a
    // half-length complex sequence.
    for (int i = 0; i < halfSize; ++i) {
        scratch_[static_cast<std::size_t>(i)] = std::complex<float>{input[2 * i], input[2 * i + 1]};
    }
    half_.forward(scratch_.data());

    // Untangle: separate the even- and odd-indexed spectra, then recombine.
    for (int k = 0; k <= halfSize / 2; ++k) {
        const int mirror = (halfSize - k) % halfSize;
        const std::complex<float> a = scratch_[static_cast<std::size_t>(k)];
        const std::complex<float> b = std::conj(scratch_[static_cast<std::size_t>(mirror)]);

        const std::complex<float> even = (a + b) * 0.5f;
        const std::complex<float> odd =
            (a - b) * std::complex<float>{0.0f, -0.5f} * twiddles_[static_cast<std::size_t>(k)];

        output[k] = even + odd;
        if (k > 0) {
            output[halfSize - k] = std::conj(even - odd);
        }
    }

    // DC and Nyquist are purely real and fall out of the k = 0 case directly.
    const std::complex<float> first = scratch_[0];
    output[0] = std::complex<float>{first.real() + first.imag(), 0.0f};
    output[halfSize] = std::complex<float>{first.real() - first.imag(), 0.0f};
}

void RealFft::inverse(const std::complex<float>* input, float* output) const {
    const int halfSize = size_ / 2;

    for (int k = 0; k < halfSize; ++k) {
        const int mirror = halfSize - k;
        const std::complex<float> a = input[k];
        const std::complex<float> b = std::conj(input[mirror]);

        const std::complex<float> even = (a + b) * 0.5f;
        const std::complex<float> odd = (a - b) * 0.5f *
                                        std::conj(twiddles_[static_cast<std::size_t>(k)]) *
                                        std::complex<float>{0.0f, 1.0f};

        scratch_[static_cast<std::size_t>(k)] = even + odd;
    }

    half_.inverse(scratch_.data());

    for (int i = 0; i < halfSize; ++i) {
        output[2 * i] = scratch_[static_cast<std::size_t>(i)].real();
        output[2 * i + 1] = scratch_[static_cast<std::size_t>(i)].imag();
    }
}

} // namespace sa::dsp
