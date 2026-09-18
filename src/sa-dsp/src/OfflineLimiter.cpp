#include <sa/dsp/ExactTruePeak.h>
#include <sa/dsp/OfflineLimiter.h>
#include <sa/dsp/Resampler.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace sa::dsp {

namespace {

/// Run one channel through a converter from end to end, returning what came out.
[[nodiscard]] Result<std::vector<float>> convert(const float* input, SampleCount count,
                                                 SampleRate from, SampleRate to) {
    ResamplerSpec spec;
    spec.inputRate = from;
    spec.outputRate = to;
    spec.quality = ResamplerQuality::Best;

    auto resampler = Resampler::create(spec);
    if (!resampler) {
        return resampler.error();
    }

    const auto ratio = to.hz() / from.hz();
    const auto capacity =
        static_cast<SampleCount>(std::ceil(static_cast<double>(count) * ratio)) + 8;
    std::vector<float> output(static_cast<std::size_t>(capacity), 0.0f);

    SampleCount consumed = 0;
    SampleCount produced = 0;
    while (consumed < count && produced < capacity) {
        const auto step = resampler.value().process(input + consumed, count - consumed,
                                                    output.data() + produced, capacity - produced);
        if (step.inputConsumed == 0 && step.outputProduced == 0) {
            break;
        }
        consumed += step.inputConsumed;
        produced += step.outputProduced;
    }
    while (produced < capacity) {
        const auto drained = resampler.value().flush(output.data() + produced, capacity - produced);
        if (drained <= 0) {
            break;
        }
        produced += drained;
    }

    output.resize(static_cast<std::size_t>(produced));
    return output;
}

/// Undo a limiter's look-ahead delay in place.
void alignForLatency(std::vector<float>& samples, SampleCount latency) {
    if (latency <= 0 || latency >= static_cast<SampleCount>(samples.size())) {
        return;
    }
    samples.erase(samples.begin(), samples.begin() + latency);
}

} // namespace

Status limitOffline(AudioBufferView audio, SampleRate rate, const OfflineLimitSettings& settings) {
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "no audio"};
    }
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate must be valid"};
    }
    if (settings.oversampling < 1 || settings.oversampling > 16) {
        return Error{ErrorCode::InvalidArgument, "oversampling must be between 1 and 16"};
    }

    const int channels = audio.channelCount();
    const SampleCount frames = audio.frames();
    const SampleRate fine{rate.hz() * settings.oversampling};

    // Sample-peak limiting at the higher rate *is* true-peak limiting at the
    // base rate, so the detector's own oversampling would be doing the same
    // work twice.
    LimiterSettings limiter = settings.limiter;
    limiter.truePeak = false;

    std::vector<std::vector<float>> upsampled(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel) {
        auto converted = settings.oversampling == 1
                             ? Result<std::vector<float>>{std::vector<float>(
                                   audio.channel(channel), audio.channel(channel) + frames)}
                             : convert(audio.channel(channel), frames, rate, fine);
        if (!converted) {
            return converted.error();
        }
        upsampled[static_cast<std::size_t>(channel)] = std::move(converted).value();
    }

    const auto fineFrames = static_cast<SampleCount>(upsampled.front().size());
    std::vector<std::vector<float>> limited(static_cast<std::size_t>(channels));
    SampleCount latency = 0;

    if (channels == 2 && settings.linkStereo) {
        auto pair = StereoLink<Limiter>::create(fine, limiter, settings.link);
        if (!pair) {
            return pair.error();
        }
        latency = pair.value().left().latencySamples();
        limited[0].assign(static_cast<std::size_t>(fineFrames), 0.0f);
        limited[1].assign(static_cast<std::size_t>(fineFrames), 0.0f);
        pair.value().process(upsampled[0].data(), upsampled[1].data(), limited[0].data(),
                             limited[1].data(), fineFrames);
    } else {
        for (int channel = 0; channel < channels; ++channel) {
            auto one = Limiter::create(fine, limiter);
            if (!one) {
                return one.error();
            }
            latency = one.value().latencySamples();
            auto& out = limited[static_cast<std::size_t>(channel)];
            out.assign(static_cast<std::size_t>(fineFrames), 0.0f);
            one.value().process(upsampled[static_cast<std::size_t>(channel)].data(), out.data(),
                                fineFrames);
        }
    }

    for (auto& channel : limited) {
        alignForLatency(channel, latency);
    }

    for (int channel = 0; channel < channels; ++channel) {
        auto& source = limited[static_cast<std::size_t>(channel)];
        std::vector<float> back;
        if (settings.oversampling == 1) {
            back = std::move(source);
        } else {
            auto converted =
                convert(source.data(), static_cast<SampleCount>(source.size()), fine, rate);
            if (!converted) {
                return converted.error();
            }
            back = std::move(converted).value();
        }

        // The conversion back is linear phase and rings a little, which can put
        // a sample a hair over the ceiling even though the oversampled signal
        // was under it. A final hard clamp costs nothing and makes the promise
        // exact on the grid; the reconstruction is already under it because
        // that is what limiting at the higher rate achieved.
        const auto ceiling = static_cast<float>(std::pow(10.0, limiter.ceilingDb / 20.0));
        float* destination = audio.channel(channel);
        const auto available = std::min<SampleCount>(frames, static_cast<SampleCount>(back.size()));
        for (SampleCount i = 0; i < available; ++i) {
            destination[i] = std::clamp(back[static_cast<std::size_t>(i)], -ceiling, ceiling);
        }
        std::fill_n(destination + available, frames - available, 0.0f);
    }

    if (!settings.trimToCeiling) {
        return Status{};
    }

    // Measure what actually came out, exactly rather than through an
    // interpolator that droops, and trim by the excess. The worst channel
    // decides, because trimming them by different amounts would move the image.
    double worst = 0.0;
    for (int channel = 0; channel < channels; ++channel) {
        auto measured = exactTruePeak(audio.channel(channel), frames);
        if (!measured) {
            return measured.error();
        }
        worst = std::max(worst, measured.value());
    }

    const auto ceiling = std::pow(10.0, limiter.ceilingDb / 20.0);
    if (worst <= ceiling || worst <= 0.0) {
        return Status{};
    }

    const auto trim = static_cast<float>(ceiling / worst);
    for (int channel = 0; channel < channels; ++channel) {
        float* destination = audio.channel(channel);
        for (SampleCount i = 0; i < frames; ++i) {
            destination[i] *= trim;
        }
    }
    return Status{};
}

} // namespace sa::dsp
