#include <sa/dsp/Stft.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace sa::dsp {

Stft::Stft(int fftSize, int hopSize, WindowType windowType)
    : fftSize_(fftSize), hopSize_(hopSize), padding_(fftSize - hopSize), fft_(fftSize),
      window_(windowType, fftSize) {
    // Normalisation for weighted overlap-add.
    //
    // Output position p is touched by every frame whose window index is
    // congruent to p modulo the hop, so the correction factor depends only on
    // p % hopSize. Precomputing one hop period of sum-of-squares gives exact
    // reconstruction rather than the scalar approximation that leaves a
    // periodic ripple in the output.
    normalisation_.assign(static_cast<std::size_t>(hopSize), 0.0f);
    for (int i = 0; i < fftSize; ++i) {
        const float value = window_[i];
        normalisation_[static_cast<std::size_t>(i % hopSize)] += value * value;
    }
}

Result<Stft> Stft::create(int fftSize, int hopSize, WindowType windowType) {
    if (!RealFft::isSupportedSize(fftSize)) {
        return Error{ErrorCode::InvalidArgument, "fftSize must be a power of two >= 4"};
    }
    if (hopSize <= 0 || hopSize > fftSize) {
        return Error{ErrorCode::InvalidArgument, "hopSize must be in (0, fftSize]"};
    }
    if (fftSize % hopSize != 0) {
        return Error{ErrorCode::InvalidArgument,
                     "hopSize must divide fftSize for exact reconstruction"};
    }
    return Stft{fftSize, hopSize, windowType};
}

SampleCount Stft::frameCount(SampleCount samples) const noexcept {
    if (samples <= 0) {
        return 0;
    }
    // Frames are indexed over the front-padded signal. The last frame needed is
    // the one whose window start is at or before the final sample, so that every
    // input sample sees a complete set of overlapping windows -- the first and
    // last samples then reconstruct as exactly as the middle ones.
    return (static_cast<SampleCount>(padding_) + samples - 1) / hopSize_ + 1;
}

void Stft::analyse(const float* samples, SampleCount count, std::complex<float>* output) const {
    const SampleCount frames = frameCount(count);
    if (frames == 0) {
        return;
    }

    std::vector<float> block(static_cast<std::size_t>(fftSize_));
    const auto bins = static_cast<std::size_t>(binCount());

    for (SampleCount frame = 0; frame < frames; ++frame) {
        const SampleIndex start = frame * hopSize_ - padding_;

        for (int i = 0; i < fftSize_; ++i) {
            const SampleIndex index = start + i;
            const float sample = (index >= 0 && index < count) ? samples[index] : 0.0f;
            block[static_cast<std::size_t>(i)] = sample * window_[i];
        }

        fft_.forward(block.data(), output + static_cast<std::size_t>(frame) * bins);
    }
}

void Stft::synthesise(const std::complex<float>* frames, SampleCount frameCount, float* output,
                      SampleCount count) const {
    if (count <= 0) {
        return;
    }
    std::fill_n(output, count, 0.0f);
    if (frameCount == 0) {
        return;
    }

    std::vector<float> block(static_cast<std::size_t>(fftSize_));
    const auto bins = static_cast<std::size_t>(binCount());

    for (SampleCount frame = 0; frame < frameCount; ++frame) {
        fft_.inverse(frames + static_cast<std::size_t>(frame) * bins, block.data());
        const SampleIndex start = frame * hopSize_ - padding_;

        for (int i = 0; i < fftSize_; ++i) {
            const SampleIndex index = start + i;
            if (index < 0 || index >= count) {
                continue;
            }
            output[index] += block[static_cast<std::size_t>(i)] * window_[i];
        }
    }

    for (SampleIndex index = 0; index < count; ++index) {
        // The padding offset keeps the phase of the normalisation table aligned
        // with the frame grid rather than with sample zero.
        const auto slot = static_cast<std::size_t>((index + padding_) % hopSize_);
        const float divisor = normalisation_[slot];
        if (divisor > 0.0f) {
            output[index] /= divisor;
        }
    }
}

} // namespace sa::dsp
