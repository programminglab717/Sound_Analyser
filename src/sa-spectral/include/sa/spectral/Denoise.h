#pragma once

#include <sa/core/AudioBuffer.h>
#include <sa/core/Result.h>
#include <sa/core/Types.h>
#include <sa/spectral/SpectralEdit.h>

#include <vector>

namespace sa::spectral {

/// The average spectrum of a stretch of noise, learned from the recording
/// itself.
///
/// Broadband denoise cannot work from a model of noise in general, because
/// "noise" is whatever the user did not want: tape hiss, a fan, a preamp, rain
/// on a window. It works from a model of *this* noise, taken from a passage
/// where nothing else is happening. That is why the user has to choose the
/// passage, and why a profile learned from a passage containing speech will
/// remove speech.
class NoiseProfile {
public:
    /// Learn from [start, end) of `audio`. The range should contain noise and
    /// nothing else.
    [[nodiscard]] static Result<NoiseProfile> learn(ConstAudioBufferView audio, SampleRate rate,
                                                    SampleIndex start, SampleIndex end,
                                                    const SpectralEditSettings& settings = {});

    [[nodiscard]] bool isEmpty() const noexcept { return magnitudes_.empty(); }

    [[nodiscard]] int binCount() const noexcept { return binCount_; }

    [[nodiscard]] int channelCount() const noexcept { return channels_; }

    [[nodiscard]] int fftSize() const noexcept { return fftSize_; }

    [[nodiscard]] SampleRate sampleRate() const noexcept { return rate_; }

    [[nodiscard]] SampleCount framesLearned() const noexcept { return framesLearned_; }

    /// Mean magnitude of `bin` on `channel`. Zero out of range.
    [[nodiscard]] float magnitudeAt(int channel, int bin) const noexcept;

private:
    std::vector<float> magnitudes_; // channel-major, binCount per channel
    int binCount_ = 0;
    int channels_ = 0;
    int fftSize_ = 0;
    SampleRate rate_{0.0};
    SampleCount framesLearned_ = 0;
};

struct DenoiseSettings {
    SpectralEditSettings analysis;

    /// How far the noise floor is pushed down, in dB. This is a *limit*, not a
    /// subtraction: a bin can be attenuated by at most this much, so setting it
    /// high does not turn quiet passages into holes.
    double reductionDb = 12.0;

    /// How much of the learned profile to subtract before deciding a bin is
    /// signal. Above 1 removes more residual noise at the cost of artefacts;
    /// this is the single knob that trades one against the other.
    double oversubtraction = 1.5;

    /// How much of the previous frame's gain to carry forward, 0 to 1.
    ///
    /// Without it, a bin flickering either side of the threshold gains and
    /// mutes frame by frame, which is heard as the warbling "musical noise"
    /// that gives spectral subtraction its bad name. Smoothing in time is the
    /// cheapest thing that stops it.
    double timeSmoothing = 0.55;

    /// Width in bins of the smoothing applied across frequency, 0 for none.
    /// Same purpose as timeSmoothing, in the other axis.
    int frequencySmoothingBins = 2;
};

/// Reduce noise matching `profile` across [start, end) of `audio`.
///
/// Uses a Wiener-style gain from the estimated signal-to-noise ratio per bin
/// rather than flat subtraction: subtraction drives a bin to silence the moment
/// its magnitude dips below the profile, and the resulting on-off chatter is
/// exactly what people mean when they say a denoiser sounds "underwater".
[[nodiscard]] Status denoise(AudioBufferView audio, SampleRate rate, const NoiseProfile& profile,
                             SampleIndex start, SampleIndex end,
                             const DenoiseSettings& settings = {});

} // namespace sa::spectral
