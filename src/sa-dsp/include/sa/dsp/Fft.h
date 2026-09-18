#pragma once

#include <sa/core/Types.h>

#include <complex>
#include <cstdint>
#include <vector>

namespace sa::dsp {

/// In-place iterative radix-2 complex FFT.
///
/// Built in-house rather than taken from a library because FFTW is GPL and
/// therefore excluded (ADR 0006). PFFFT would be roughly 2-3x faster and is
/// allowlisted; swapping it in later is confined to this class.
///
/// Not audio-thread safe to construct -- the twiddle and bit-reversal tables
/// allocate. Construct once, reuse. transform() itself allocates nothing.
class ComplexFft {
public:
    explicit ComplexFft(int size);

    [[nodiscard]] int size() const noexcept { return size_; }

    /// Forward transform, in place. Unnormalised.
    void forward(std::complex<float>* data) const noexcept;

    /// Inverse transform, in place. Scaled by 1/N so that
    /// inverse(forward(x)) == x.
    void inverse(std::complex<float>* data) const noexcept;

    [[nodiscard]] static bool isSupportedSize(int size) noexcept {
        return size >= 2 && (size & (size - 1)) == 0;
    }

private:
    void transform(std::complex<float>* data, bool conjugate) const noexcept;

    int size_ = 0;
    std::vector<std::uint32_t> bitReversal_;
    std::vector<std::complex<float>> twiddles_;
};

/// Real-input FFT producing size/2 + 1 complex bins.
///
/// Implemented as a half-size complex FFT plus an untangling pass, which is the
/// standard trick and halves the work versus zero-stuffing the imaginary part.
class RealFft {
public:
    explicit RealFft(int size);

    [[nodiscard]] int size() const noexcept { return size_; }

    /// Number of output bins: DC through Nyquist inclusive.
    [[nodiscard]] int binCount() const noexcept { return size_ / 2 + 1; }

    /// `input` holds size() real samples; `output` receives binCount() bins.
    void forward(const float* input, std::complex<float>* output) const;

    /// `input` holds binCount() bins; `output` receives size() real samples.
    /// Normalised, so inverse(forward(x)) == x.
    void inverse(const std::complex<float>* input, float* output) const;

    [[nodiscard]] static bool isSupportedSize(int size) noexcept {
        return size >= 4 && (size & (size - 1)) == 0;
    }

private:
    int size_ = 0;
    ComplexFft half_;
    std::vector<std::complex<float>> twiddles_;
    mutable std::vector<std::complex<float>> scratch_;
};

} // namespace sa::dsp
