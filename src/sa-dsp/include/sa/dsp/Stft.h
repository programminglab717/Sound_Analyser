#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/dsp/Fft.h>
#include <sa/dsp/Window.h>

#include <complex>
#include <vector>

namespace sa::dsp {

/// Short-time Fourier transform with overlap-add resynthesis.
///
/// The spectral engine's foundation. Editing operates on masks over the output
/// of analyse(), and synthesise() puts the audio back; if the untouched
/// round-trip is not transparent, every spectral edit inherits that error as an
/// artefact. Round-trip reconstruction error is therefore a CI gate, not a
/// nice-to-have (docs/03-architecture.md §5).
class Stft {
public:
    /// `fftSize` must be a power of two >= 4. `hopSize` must divide fftSize for
    /// the round-trip guarantee to hold.
    [[nodiscard]] static Result<Stft> create(int fftSize, int hopSize,
                                             WindowType windowType = WindowType::Hann);

    [[nodiscard]] int fftSize() const noexcept { return fftSize_; }

    [[nodiscard]] int hopSize() const noexcept { return hopSize_; }

    [[nodiscard]] int binCount() const noexcept { return fft_.binCount(); }

    [[nodiscard]] const Window& window() const noexcept { return window_; }

    /// Frames produced for `samples` input samples.
    ///
    /// The signal is treated as zero-padded by fftSize - hopSize at the front
    /// and enough at the back to flush the final window, so every input sample
    /// is covered by a full set of overlapping windows and the first and last
    /// samples reconstruct as exactly as the middle ones.
    [[nodiscard]] SampleCount frameCount(SampleCount samples) const noexcept;

    /// Transform `samples` into `output`, which must hold
    /// frameCount(count) * binCount() complex values, frame-major.
    void analyse(const float* samples, SampleCount count, std::complex<float>* output) const;

    /// Overlap-add `frames` back into `output`, which must hold `count` samples.
    void synthesise(const std::complex<float>* frames, SampleCount frameCount, float* output,
                    SampleCount count) const;

private:
    Stft(int fftSize, int hopSize, WindowType windowType);

    int fftSize_ = 0;
    int hopSize_ = 0;
    int padding_ = 0;
    RealFft fft_;
    Window window_;
    /// Sum of squared window values at each output position over one hop
    /// period, used to normalise overlap-add exactly rather than by a scalar
    /// approximation.
    std::vector<float> normalisation_;
};

} // namespace sa::dsp
