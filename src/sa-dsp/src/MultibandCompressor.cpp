#include <sa/core/ChannelLayout.h>
#include <sa/dsp/Decibels.h>
#include <sa/dsp/MultibandCompressor.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace sa::dsp {

namespace {

/// One crossover, held as a pair of designed but never-run cascades.
///
/// Kept as prototypes rather than as live filters because the tree needs more
/// than one instance of the same crossover: the low band is run through the
/// all-pass equivalent of every crossover above it, and each of those runs
/// needs its own state. Copying a cascade that has never seen a sample gives a
/// cleared one, so the copies are the instances.
struct Crossover {
    BiquadCascade low;
    BiquadCascade high;
};

/// Sample magnitude below which a gain cannot be read off a pair of samples.
///
/// The gain a compressor applied is the ratio of the sample after to the sample
/// before, and float32 carries that ratio to about seven figures at any
/// magnitude -- until the denominator reaches the denormal range, where the
/// mantissa is being eaten away and the ratio stops meaning anything. 1e-20 is
/// some four hundred decibels below full scale and eighteen orders of magnitude
/// above the smallest normal float, so nothing that could carry a gain worth
/// reporting is skipped here.
inline constexpr double kMeasurableSample = 1e-20;

/// One half of a Linkwitz-Riley crossover: a Butterworth of half the order,
/// cascaded with itself.
[[nodiscard]] Result<BiquadCascade> linkwitzRileyHalf(FilterType type, int order, SampleRate rate,
                                                      double frequency) {
    Result<BiquadCascade> designed = BiquadCascade::butterworth(type, order / 2, rate, frequency);
    if (!designed) {
        return designed.error();
    }

    BiquadCascade cascade = std::move(designed).value();
    const int butterworthSections = cascade.sectionCount();
    for (int i = 0; i < butterworthSections; ++i) {
        const BiquadCoefficients* section = cascade.sectionCoefficients(i);
        // `i` is below the count the cascade itself just reported, so this is
        // never null. The branch is here because the accessor's type cannot say
        // so, and a silent dereference would be worse than a dead error path.
        if (section == nullptr) {
            return Error{ErrorCode::OutOfRange, "a crossover section went out of range"};
        }
        const BiquadCoefficients copy = *section;
        if (const Status status = cascade.append(copy); !status) {
            return status.error();
        }
    }
    return cascade;
}

[[nodiscard]] Result<Crossover> makeCrossover(SampleRate rate, int order, double frequency) {
    Result<BiquadCascade> low = linkwitzRileyHalf(FilterType::LowPass, order, rate, frequency);
    if (!low) {
        return low.error();
    }
    Result<BiquadCascade> high = linkwitzRileyHalf(FilterType::HighPass, order, rate, frequency);
    if (!high) {
        return high.error();
    }
    // A pole that has drifted onto or outside the unit circle makes a
    // resonator, not a crossover, and a resonator in a band splitter rings
    // through every band at once. Reported rather than returned, for the same
    // reason the filter bank reports it.
    if (!low.value().isStable() || !high.value().isStable()) {
        return Error{ErrorCode::InvalidArgument,
                     "the crossover came out unstable at this frequency and order"};
    }

    Crossover crossover;
    crossover.low = std::move(low).value();
    crossover.high = std::move(high).value();
    return crossover;
}

/// Takes one crossover's low half out of `source` into `low`, leaving the high
/// half in `source` -- which is what the next crossover up the tree splits.
void split(const Crossover& crossover, AudioBufferView source, AudioBufferView low) {
    for (int channel = 0; channel < source.channelCount(); ++channel) {
        BiquadCascade lowPass = crossover.low;
        BiquadCascade highPass = crossover.high;
        float* from = source.channel(channel);
        float* to = low.channel(channel);
        for (SampleCount i = 0; i < source.frames(); ++i) {
            const float sample = from[i];
            to[i] = lowPass.processSample(sample);
            from[i] = highPass.processSample(sample);
        }
    }
}

/// Runs the all-pass equivalent of `crossover` over `audio` in place.
///
/// The two halves added, rather than an all-pass designed from its own
/// coefficients. They are the same filter -- for a Linkwitz-Riley pair the sum
/// is exactly the Butterworth-pole all-pass -- but adding the halves makes the
/// band sum telescope as an identity rather than as two designers agreeing to
/// seven figures.
void allPass(const Crossover& crossover, AudioBufferView audio) {
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        BiquadCascade lowPass = crossover.low;
        BiquadCascade highPass = crossover.high;
        float* samples = audio.channel(channel);
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            const float sample = samples[i];
            samples[i] = lowPass.processSample(sample) + highPass.processSample(sample);
        }
    }
}

/// Most the compressor pulled this band down, in decibels and positive.
///
/// `from` and `to` bound the region where the processing is fully applied. The
/// crossfades at each end are outside it deliberately: there the gain is partly
/// the crossfade and partly the compressor, so with any makeup gain at all the
/// reading would say the band had been reduced by the makeup at the exact
/// sample where nothing had been done to it.
[[nodiscard]] double peakReductionDb(ConstAudioBufferView before, ConstAudioBufferView after,
                                     SampleCount from, SampleCount to, double makeupGainDb) {
    double peak = 0.0;
    for (int channel = 0; channel < before.channelCount(); ++channel) {
        const float* was = before.channel(channel);
        const float* is = after.channel(channel);
        for (SampleCount i = from; i < to; ++i) {
            const double input = static_cast<double>(was[i]);
            if (std::abs(input) < kMeasurableSample) {
                continue;
            }
            const double appliedDb = gainToDecibels(static_cast<double>(is[i]) / input);
            peak = std::max(peak, makeupGainDb - appliedDb);
        }
    }
    return peak;
}

[[nodiscard]] Status validate(SampleRate rate, const MultibandSettings& settings,
                              SampleCount frames) {
    if (!rate.isValid()) {
        return Error{ErrorCode::InvalidArgument, "sample rate is not a usable audio rate"};
    }
    if (settings.crossoverHz.empty()) {
        return Error{ErrorCode::InvalidArgument, "a multiband split needs at least one crossover"};
    }
    if (settings.bands.size() != settings.crossoverHz.size() + 1) {
        return Error{ErrorCode::InvalidArgument,
                     "there must be exactly one more band than there are crossovers"};
    }
    if (settings.order < 4 || settings.order % 4 != 0 || settings.order > kMaxCrossoverOrder) {
        return Error{ErrorCode::OutOfRange,
                     "the crossover order must be a multiple of four, from 4 upwards"};
    }

    const double nyquist = rate.hz() * 0.5;
    double previous = 0.0;
    for (const double frequency : settings.crossoverHz) {
        if (!std::isfinite(frequency) || frequency <= 0.0 || frequency >= nyquist) {
            return Error{ErrorCode::InvalidArgument,
                         "every crossover must be finite and strictly between zero and Nyquist"};
        }
        if (frequency <= previous) {
            return Error{ErrorCode::InvalidArgument,
                         "the crossovers must be strictly increasing, low to high"};
        }
        previous = frequency;
    }

    if (settings.runUp < 0 || settings.blend < 0 || settings.runUp > frames) {
        return Error{ErrorCode::InvalidArgument, "the run-up and blend must fit inside the buffer"};
    }

    // Every band, including the bypassed ones. Settings that would be rejected
    // the moment the bypass was switched off are rejected now, so that turning
    // a band on cannot be what fails. Compressor::create is the validator
    // because it is the one that decides, and a second copy of those rules here
    // would be a second copy to keep in step.
    for (const MultibandBandSettings& band : settings.bands) {
        if (Result<Compressor> compressor = Compressor::create(rate, band.compressor);
            !compressor) {
            return compressor.error();
        }
    }
    return {};
}

} // namespace

Result<MultibandResult> compressMultiband(AudioBufferView audio, SampleRate rate,
                                          const MultibandSettings& settings) {
    if (const Status status = validate(rate, settings, audio.frames()); !status) {
        return status.error();
    }

    MultibandResult result;
    result.gainReductionDb.assign(settings.bands.size(), 0.0);
    if (audio.isEmpty()) {
        return result;
    }

    const std::size_t bandCount = settings.bands.size();
    const std::size_t crossoverCount = settings.crossoverHz.size();

    std::vector<Crossover> crossovers;
    crossovers.reserve(crossoverCount);
    for (const double frequency : settings.crossoverHz) {
        Result<Crossover> crossover = makeCrossover(rate, settings.order, frequency);
        if (!crossover) {
            return crossover.error();
        }
        crossovers.push_back(std::move(crossover).value());
    }

    const bool anySoloed = std::any_of(settings.bands.begin(), settings.bands.end(),
                                       [](const MultibandBandSettings& band) { return band.solo; });

    // The window the reported gain reduction is measured over: the processed
    // region less the crossfades, which compressOffline clamps to half of it.
    const int channels = audio.channelCount();
    const SampleCount frames = audio.frames();
    const SampleCount blend = std::min(settings.blend, (frames - settings.runUp) / 2);
    const SampleCount measureFrom = settings.runUp + blend;
    const SampleCount measureTo = frames - blend;

    const ChannelLayout layout = ChannelLayout::discrete(channels);
    AudioBuffer remainder{layout, frames};
    AudioBuffer band{layout, frames};
    AudioBuffer before{layout, frames};

    // The buffer is both the input and the output, so the input moves aside
    // first and the bands are summed back over what is left.
    for (int channel = 0; channel < channels; ++channel) {
        std::copy_n(audio.channel(channel), frames, remainder.channel(channel));
        std::fill_n(audio.channel(channel), frames, 0.0f);
    }

    for (std::size_t index = 0; index < bandCount; ++index) {
        if (index + 1 < bandCount) {
            split(crossovers[index], remainder.view(), band.view());
        } else {
            for (int channel = 0; channel < channels; ++channel) {
                std::copy_n(remainder.channel(channel), frames, band.channel(channel));
            }
        }

        // The compensation: the phase of every crossover above this band, which
        // this band did not otherwise pass through. Without it the bands sum to
        // a scoop at each crossover below the top one.
        for (std::size_t above = index + 1; above < crossoverCount; ++above) {
            allPass(crossovers[above], band.view());
        }

        const MultibandBandSettings& bandSettings = settings.bands[index];
        if (anySoloed && !bandSettings.solo) {
            continue;
        }

        if (!bandSettings.bypass) {
            for (int channel = 0; channel < channels; ++channel) {
                std::copy_n(band.channel(channel), frames, before.channel(channel));
            }
            if (const Status status =
                    compressOffline(band.view(), rate, bandSettings.compressor, settings.runUp,
                                    settings.blend, settings.dynamics);
                !status) {
                return status.error();
            }
            result.gainReductionDb[index] =
                peakReductionDb(before.view(), band.view(), measureFrom, measureTo,
                                bandSettings.compressor.makeupGainDb);
        }

        for (int channel = 0; channel < channels; ++channel) {
            const float* from = band.channel(channel);
            float* to = audio.channel(channel);
            for (SampleCount i = 0; i < frames; ++i) {
                to[i] += from[i];
            }
        }
    }

    return result;
}

} // namespace sa::dsp
