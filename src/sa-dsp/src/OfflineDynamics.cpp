#include <sa/dsp/OfflineDynamics.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace sa::dsp {

namespace {

/// Run one processor per channel, or one linked pair, over the whole buffer.
///
/// Templated on the processor for the same reason StereoLink is: the two differ
/// only in what their detector does and what their gain computer says, and both
/// are already behind detect() and applyDetected().
template <typename Processor>
[[nodiscard]] Status runOffline(AudioBufferView audio, SampleRate rate,
                                const typename Processor::Settings& settings, SampleCount runUp,
                                SampleCount blend, const OfflineDynamicsSettings& options) {
    const int channels = audio.channelCount();
    const SampleCount frames = audio.frames();
    if (channels <= 0 || frames <= 0) {
        return {};
    }
    if (runUp < 0 || blend < 0 || runUp > frames) {
        return Error{ErrorCode::InvalidArgument, "run-up and blend must fit inside the buffer"};
    }

    // The processed region is what is left after the run-up, and a blend cannot
    // be longer than half of it or the two crossfades would overlap and the
    // middle would be neither one thing nor the other.
    const SampleCount processed = frames - runUp;
    blend = std::min(blend, processed / 2);

    // The original is kept for the edge blends. Only the processed region is
    // needed, since the run-up is discarded by the caller either way.
    std::vector<std::vector<float>> original(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel) {
        const float* from = audio.channel(channel) + runUp;
        original[static_cast<std::size_t>(channel)].assign(from, from + processed);
    }

    if (options.linkStereo && channels == 2) {
        auto pair = StereoLink<Processor>::create(rate, settings, options.link);
        if (!pair) {
            return pair.error();
        }
        pair.value().processInPlace(audio.channel(0), audio.channel(1), frames);
    } else {
        for (int channel = 0; channel < channels; ++channel) {
            auto one = Processor::create(rate, settings);
            if (!one) {
                return one.error();
            }
            one.value().processInPlace(audio.channel(channel), frames);
        }
    }

    if (blend <= 0) {
        return {};
    }

    // Equal-gain rather than equal-power. The two sides of this join are the
    // same audio at two different gains, not two different signals, so they are
    // perfectly correlated and a linear blend is what keeps the level smooth --
    // an equal-power blend would bulge in the middle.
    for (int channel = 0; channel < channels; ++channel) {
        float* samples = audio.channel(channel) + runUp;
        const std::vector<float>& was = original[static_cast<std::size_t>(channel)];
        for (SampleCount i = 0; i < blend; ++i) {
            const auto mix = static_cast<float>(i + 1) / static_cast<float>(blend + 1);
            samples[i] = (1.0f - mix) * was[static_cast<std::size_t>(i)] + mix * samples[i];

            const SampleCount j = processed - 1 - i;
            samples[j] = (1.0f - mix) * was[static_cast<std::size_t>(j)] + mix * samples[j];
        }
    }
    return {};
}

} // namespace

SampleCount dynamicsRunUp(SampleRate rate, double attackSeconds, double releaseSeconds) noexcept {
    const double slowest = std::max(attackSeconds, releaseSeconds);
    const double seconds = std::max(0.2, 10.0 * slowest);
    return static_cast<SampleCount>(seconds * rate.hz());
}

Status compressOffline(AudioBufferView audio, SampleRate rate, const CompressorSettings& compressor,
                       SampleCount runUp, SampleCount blend,
                       const OfflineDynamicsSettings& settings) {
    return runOffline<Compressor>(audio, rate, compressor, runUp, blend, settings);
}

Status gateOffline(AudioBufferView audio, SampleRate rate, const GateSettings& gate,
                   SampleCount runUp, SampleCount blend, const OfflineDynamicsSettings& settings) {
    return runOffline<Gate>(audio, rate, gate, runUp, blend, settings);
}

} // namespace sa::dsp
