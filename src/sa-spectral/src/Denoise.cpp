#include <sa/dsp/Stft.h>
#include <sa/spectral/Denoise.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace sa::spectral {

namespace {

/// Below this the profile is treated as silence in that bin, so a band the
/// noise passage never excited does not become a divide by almost zero and a
/// gain of almost anything.
constexpr float kProfileFloor = 1e-9f;

} // namespace

Result<NoiseProfile> NoiseProfile::learn(ConstAudioBufferView audio, SampleRate rate,
                                         SampleIndex start, SampleIndex end,
                                         const SpectralEditSettings& settings) {
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "no audio"};
    }
    if (end <= start) {
        return Error{ErrorCode::InvalidArgument, "range is empty"};
    }
    if (start < 0 || end > audio.frames()) {
        return Error{ErrorCode::OutOfRange, "range falls outside the audio"};
    }
    if (rate.hz() <= 0.0) {
        return Error{ErrorCode::InvalidArgument, "sample rate must be positive"};
    }

    auto stft = dsp::Stft::create(settings.fftSize, settings.hopSize, settings.window);
    if (!stft) {
        return stft.error();
    }

    const SampleCount length = end - start;

    // Two full analysis windows, measured in samples rather than in frames.
    //
    // Counting frames does not work: the transform pads the front by a window
    // minus a hop, so sixty-four samples of audio still produce four frames.
    // Averaging those gives a profile that describes the analysis window rather
    // than the noise, and subtracting it would remove a shape the recording
    // never had. Two windows is about 170 ms at 48 kHz with the default size --
    // short, but genuinely a measurement.
    if (length < 2 * static_cast<SampleCount>(settings.fftSize)) {
        return Error{ErrorCode::InvalidArgument,
                     "the noise range is shorter than two analysis windows"};
    }
    const SampleCount frames = stft.value().frameCount(length);

    NoiseProfile profile;
    profile.binCount_ = stft.value().binCount();
    profile.channels_ = audio.channelCount();
    profile.fftSize_ = settings.fftSize;
    profile.rate_ = rate;
    profile.framesLearned_ = frames;
    profile.magnitudes_.assign(static_cast<std::size_t>(profile.binCount_) *
                                   static_cast<std::size_t>(profile.channels_),
                               0.0f);

    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(frames) *
                                              static_cast<std::size_t>(profile.binCount_));

    for (int channel = 0; channel < profile.channels_; ++channel) {
        stft.value().analyse(audio.channel(channel) + start, length, spectrum.data());

        // The mean, not the minimum or a low percentile. A minimum tracks the
        // quietest window and so under-states a floor that fluctuates, leaving
        // audible residue; the mean is what a Wiener gain is derived against.
        for (SampleCount frame = 0; frame < frames; ++frame) {
            const auto offset =
                static_cast<std::size_t>(frame) * static_cast<std::size_t>(profile.binCount_);
            for (int bin = 0; bin < profile.binCount_; ++bin) {
                profile.magnitudes_[static_cast<std::size_t>(channel) *
                                        static_cast<std::size_t>(profile.binCount_) +
                                    static_cast<std::size_t>(bin)] +=
                    std::abs(spectrum[offset + static_cast<std::size_t>(bin)]);
            }
        }
        for (int bin = 0; bin < profile.binCount_; ++bin) {
            profile.magnitudes_[static_cast<std::size_t>(channel) *
                                    static_cast<std::size_t>(profile.binCount_) +
                                static_cast<std::size_t>(bin)] /= static_cast<float>(frames);
        }
    }
    return profile;
}

float NoiseProfile::magnitudeAt(int channel, int bin) const noexcept {
    if (channel < 0 || channel >= channels_ || bin < 0 || bin >= binCount_) {
        return 0.0f;
    }
    return magnitudes_[static_cast<std::size_t>(channel) * static_cast<std::size_t>(binCount_) +
                       static_cast<std::size_t>(bin)];
}

Status denoise(AudioBufferView audio, SampleRate rate, const NoiseProfile& profile,
               SampleIndex start, SampleIndex end, const DenoiseSettings& settings) {
    if (profile.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "the noise profile is empty"};
    }
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "no audio"};
    }
    if (end <= start) {
        return Error{ErrorCode::InvalidArgument, "range is empty"};
    }
    if (start < 0 || end > audio.frames()) {
        return Error{ErrorCode::OutOfRange, "range falls outside the audio"};
    }
    if (profile.channelCount() != audio.channelCount()) {
        return Error{ErrorCode::InvalidArgument,
                     "the profile was learned from a different channel count"};
    }
    if (profile.fftSize() != settings.analysis.fftSize) {
        // A profile is a table indexed by bin, so the analysis that reads it
        // has to divide the spectrum the same way the one that built it did.
        return Error{ErrorCode::InvalidArgument,
                     "the profile was learned at a different analysis size"};
    }
    if (std::abs(profile.sampleRate().hz() - rate.hz()) > 0.5) {
        // Same bins, different frequencies. A profile learned at 44.1 kHz
        // applied to 48 kHz audio subtracts each bin's noise from a band about
        // 9% higher than the one it came from, which removes the wrong thing
        // everywhere and looks like it worked.
        return Error{ErrorCode::InvalidArgument,
                     "the profile was learned at a different sample rate"};
    }
    if (settings.reductionDb <= 0.0) {
        return Status{}; // Asking for no reduction is not an error.
    }

    auto stft = dsp::Stft::create(settings.analysis.fftSize, settings.analysis.hopSize,
                                  settings.analysis.window);
    if (!stft) {
        return stft.error();
    }

    // Context either side, for the same reason every spectral edit needs it:
    // the effect spreads one analysis window and clipping that spread clicks.
    const SampleCount context = settings.analysis.fftSize;
    const SampleIndex spanStart = std::max<SampleIndex>(0, start - context);
    const SampleIndex spanEnd = std::min<SampleIndex>(audio.frames(), end + context);
    const SampleCount spanLength = spanEnd - spanStart;

    const SampleCount frames = stft.value().frameCount(spanLength);
    const int bins = stft.value().binCount();
    if (frames <= 0 || bins != profile.binCount()) {
        return Error{ErrorCode::InvalidArgument, "analysis does not match the profile"};
    }

    const auto floorGain = static_cast<float>(std::pow(10.0, -settings.reductionDb / 20.0));
    const auto oversubtraction = static_cast<float>(std::max(0.0, settings.oversubtraction));
    const auto smoothing = static_cast<float>(std::clamp(settings.timeSmoothing, 0.0, 0.99));
    const int smear = std::max(0, settings.frequencySmoothingBins);

    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(frames) *
                                              static_cast<std::size_t>(bins));
    std::vector<float> gains(static_cast<std::size_t>(bins));
    std::vector<float> smoothed(static_cast<std::size_t>(bins));
    std::vector<float> previous(static_cast<std::size_t>(bins), 1.0f);

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        float* samples = audio.channel(channel) + spanStart;
        stft.value().analyse(samples, spanLength, spectrum.data());
        std::fill(previous.begin(), previous.end(), 1.0f);

        for (SampleCount frame = 0; frame < frames; ++frame) {
            const auto offset = static_cast<std::size_t>(frame) * static_cast<std::size_t>(bins);

            for (int bin = 0; bin < bins; ++bin) {
                const float magnitude = std::abs(spectrum[offset + static_cast<std::size_t>(bin)]);
                const float noise =
                    std::max(kProfileFloor, profile.magnitudeAt(channel, bin) * oversubtraction);

                // Wiener gain from the a-posteriori SNR. Flat subtraction drives
                // a bin to silence the moment it dips below the profile, and the
                // resulting on-off chatter is what "underwater" means.
                const float power = magnitude * magnitude;
                const float noisePower = noise * noise;
                const float signalPower = std::max(0.0f, power - noisePower);
                const float gain = signalPower / (signalPower + noisePower);

                gains[static_cast<std::size_t>(bin)] = std::max(floorGain, gain);
            }

            // Smooth across frequency, then across time. Both exist to stop a
            // bin flickering either side of the threshold, which is heard as
            // the warbling musical noise spectral subtraction is known for.
            if (smear > 0) {
                for (int bin = 0; bin < bins; ++bin) {
                    const int low = std::max(0, bin - smear);
                    const int high = std::min(bins - 1, bin + smear);
                    float total = 0.0f;
                    for (int i = low; i <= high; ++i) {
                        total += gains[static_cast<std::size_t>(i)];
                    }
                    smoothed[static_cast<std::size_t>(bin)] =
                        total / static_cast<float>(high - low + 1);
                }
                gains.swap(smoothed);
            }

            for (int bin = 0; bin < bins; ++bin) {
                const auto index = static_cast<std::size_t>(bin);
                const float blended =
                    smoothing * previous[index] + (1.0f - smoothing) * gains[index];
                previous[index] = blended;
                spectrum[offset + index] *= blended;
            }
        }

        stft.value().synthesise(spectrum.data(), frames, samples, spanLength);
    }
    return Status{};
}

} // namespace sa::spectral
