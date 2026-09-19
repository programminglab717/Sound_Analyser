#include <sa/dsp/Stft.h>
#include <sa/spectral/Dereverb.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <vector>

namespace sa::spectral {

namespace {

/// Decay constant of the statistical room model, in nepers per second.
///
/// An envelope of exp(-dt) carries energy exp(-2dt), which has fallen 60 dB
/// when 2dt = 6*ln(10). So d = 3*ln(10)/T60, and no constant has to be
/// remembered anywhere else in this file.
[[nodiscard]] double decayRateFor(double t60Seconds) noexcept {
    return 3.0 * std::numbers::ln10 / t60Seconds;
}

} // namespace

Status reduceReverb(AudioBufferView audio, SampleRate rate, const DereverbSettings& settings) {
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "no audio"};
    }
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate must be positive"};
    }
    if (settings.reductionDb < 0.0) {
        // Negative would be an instruction to add reverberation, which is a
        // different tool and not one that can be built out of a subtraction.
        return Error{ErrorCode::InvalidArgument, "reduction cannot be negative"};
    }
    if (settings.decaySeconds <= 0.0) {
        return Error{ErrorCode::InvalidArgument, "the assumed decay must be positive"};
    }
    if (settings.floorDb > 0.0) {
        // A floor above the input would let the gain exceed one, so a bin the
        // estimate said was all tail would come out louder than it went in.
        return Error{ErrorCode::InvalidArgument, "the floor cannot sit above the input"};
    }
    if (settings.lateOnsetSeconds < 0.0) {
        return Error{ErrorCode::InvalidArgument, "the late onset cannot be negative"};
    }
    if (settings.hop <= 0 || settings.hop > std::numeric_limits<int>::max()) {
        return Error{ErrorCode::InvalidArgument, "hop must be a positive sample count"};
    }

    // Everything else about the analysis -- the power of two, the hop dividing
    // the window -- is the transform's own contract, and checking it here would
    // be a second copy of it to drift out of step.
    const auto hop = static_cast<int>(settings.hop);
    auto stft = dsp::Stft::create(settings.fftSize, hop, settings.window);
    if (!stft) {
        return stft.error();
    }

    const SampleCount length = audio.frames();
    const double hopSeconds = static_cast<double>(hop) / rate.hz();
    const double wantedOnset = settings.lateOnsetSeconds > 0.0
                                   ? settings.lateOnsetSeconds
                                   : 2.0 * static_cast<double>(settings.fftSize) / rate.hz();

    // The frame the estimate looks back at must not overlap the frame it is
    // correcting, or the "late" energy it reports includes that frame's own
    // direct sound and the repair subtracts the signal from itself. Windows are
    // fftSize long and start a hop apart, so that is fftSize/hop frames, which
    // divides exactly because the transform insists the hop divide the window.
    const SampleCount windowFrames = settings.fftSize / hop;
    const SampleCount delay =
        std::max(windowFrames, static_cast<SampleCount>(std::llround(wantedOnset / hopSeconds)));

    const SampleCount frames = stft.value().frameCount(length);
    if (frames <= delay) {
        // Not one frame of this recording has a full window of history behind
        // it, so the estimate is zero everywhere and the repair is a round trip
        // wearing a costume. Refusing says so, rather than returning audio that
        // has been through a transform for nothing and reporting success. At
        // the defaults and 48 kHz the shortest file this accepts is 53 ms.
        return Error{ErrorCode::InvalidArgument,
                     "the audio is shorter than the late reverberation it would remove"};
    }

    // Rounded to the frame grid, then used at the rounded value: the decay the
    // gain is computed from has to be the decay over the gap actually used, not
    // the one that was asked for.
    const double onsetSeconds = static_cast<double>(delay) * hopSeconds;
    const double decay = std::exp(-2.0 * decayRateFor(settings.decaySeconds) * onsetSeconds);

    // reductionDb is a proportion of the estimated late *power*: 10 dB is 90%
    // of it, 20 dB is 99%, 0 dB is none of it and multiplies every bin by
    // exactly one.
    const double removal = 1.0 - std::pow(10.0, -settings.reductionDb / 10.0);
    const auto scale = static_cast<float>(decay * removal);
    const auto floorGain = static_cast<float>(std::pow(10.0, settings.floorDb / 20.0));
    const auto smoothing = static_cast<float>(std::clamp(settings.timeSmoothing, 0.0, 0.99));
    const int smear = std::max(0, settings.frequencySmoothingBins);

    const int bins = stft.value().binCount();
    const auto binCount = static_cast<std::size_t>(bins);

    std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(frames) * binCount);

    // The observed powers of the last `delay` frames, kept apart from the
    // spectrum because the spectrum is edited in place. An estimate built from
    // frames this pass had already attenuated would chase its own output
    // downward, and each pass round the loop would remove a little more than
    // the model says is there.
    const SampleCount ringFrames = delay + 1;
    std::vector<float> history(static_cast<std::size_t>(ringFrames) * binCount);
    std::vector<float> gains(binCount);
    std::vector<float> smoothed(binCount);
    std::vector<float> previous(binCount, 1.0f);

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        float* samples = audio.channel(channel);
        stft.value().analyse(samples, length, spectrum.data());
        std::fill(history.begin(), history.end(), 0.0f);
        std::fill(previous.begin(), previous.end(), 1.0f);

        for (SampleCount frame = 0; frame < frames; ++frame) {
            const auto offset = static_cast<std::size_t>(frame) * binCount;

            // Slot (frame + 1) mod (delay + 1) still holds frame - delay, which
            // is the one wanted; slot frame mod (delay + 1) holds
            // frame - delay - 1, which nothing needs again and is where this
            // frame goes. The two are never the same slot because delay is at
            // least one.
            const auto pastRow = static_cast<std::size_t>((frame + 1) % ringFrames) * binCount;
            const auto thisRow = static_cast<std::size_t>(frame % ringFrames) * binCount;

            for (int bin = 0; bin < bins; ++bin) {
                const auto index = static_cast<std::size_t>(bin);
                const float magnitude = std::abs(spectrum[offset + index]);
                const float power = magnitude * magnitude;

                // Zero before the history has filled, so the first frames of a
                // file are left alone -- correctly, since no tail has arrived
                // yet to remove.
                const float late = scale * history[pastRow + index];
                const float remaining = power > late ? power - late : 0.0f;

                // Power subtraction, where the denoiser next door uses a Wiener
                // gain. Wiener's extra attenuation where signal and estimate
                // are comparable buys less warble, and it buys it by taking a
                // second bite out of sustained material -- which is the
                // material a de-reverb is most often asked to leave alone, and
                // which this model already cannot tell from its own tail. The
                // floor and the two smoothers below are what hold the warble
                // down here instead.
                const float gain = power > 0.0f ? std::sqrt(remaining / power) : 1.0f;
                gains[index] = std::max(floorGain, gain);
                history[thisRow + index] = power;
            }

            // Across frequency, then across time. Both exist to stop a bin
            // crossing back and forth over its own estimate frame by frame,
            // which is heard as warbling rather than as less reverberation.
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

        stft.value().synthesise(spectrum.data(), frames, samples, length);
    }
    return Status{};
}

} // namespace sa::spectral
