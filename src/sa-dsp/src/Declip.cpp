#include <sa/dsp/Declip.h>
#include <sa/dsp/LinearPrediction.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace sa::dsp {

namespace {

struct FlatTop {
    SampleIndex start = 0;
    SampleIndex end = 0; // Exclusive.
    bool positive = true;
};

/// Runs of samples pinned at the ceiling, with a consistent sign.
///
/// The sign matters. A cycle that clips on both halves is two events, not one,
/// and the restored values have to go out in the direction the original went;
/// treating the pair as a single flat region would interpolate across a zero
/// crossing that really is there.
[[nodiscard]] std::vector<FlatTop> findFlatTops(const float* samples, SampleCount count,
                                                double ceiling, int minimumRun) {
    std::vector<FlatTop> found;
    SampleIndex runStart = -1;
    bool positive = true;

    for (SampleIndex i = 0; i <= count; ++i) {
        const double value = i < count ? static_cast<double>(samples[i]) : 0.0;
        const bool pinned = i < count && std::abs(value) >= ceiling;
        const bool sign = value >= 0.0;

        if (pinned && runStart < 0) {
            runStart = i;
            positive = sign;
        } else if (runStart >= 0 && (!pinned || sign != positive)) {
            if (i - runStart >= minimumRun) {
                found.push_back(FlatTop{runStart, i, positive});
            }
            runStart = pinned ? i : -1;
            positive = sign;
        }
    }
    return found;
}

} // namespace

Result<DeclipReport> declip(AudioBufferView audio, const DeclipSettings& settings) {
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "there is nothing to declip"};
    }
    if (!(settings.tolerance > 0.0) || settings.tolerance >= 1.0) {
        return Error{ErrorCode::OutOfRange, "the tolerance must be between 0 and 1"};
    }
    if (settings.minimumRun < 1) {
        return Error{ErrorCode::OutOfRange, "a run of fewer than one sample is not a run"};
    }
    if (settings.maximumRun < settings.minimumRun ||
        settings.maximumRun > kMaximumInterpolationGap) {
        return Error{ErrorCode::OutOfRange, "the longest run is below the shortest or above 1024"};
    }
    if (settings.order < 1 || settings.order > kMaximumPredictionOrder) {
        return Error{ErrorCode::OutOfRange, "prediction order is outside 1 to 256"};
    }
    if (settings.blockSize < 4 * settings.order) {
        return Error{ErrorCode::InvalidArgument,
                     "a block must be at least four times the model order"};
    }
    if (!(settings.minimumRecovery > 0.0) || !std::isfinite(settings.minimumRecovery)) {
        return Error{ErrorCode::OutOfRange, "the minimum recovery must be a positive fraction"};
    }
    if (!std::isfinite(settings.ceilingDb) || settings.ceilingDb > 0.0) {
        return Error{ErrorCode::OutOfRange, "the ceiling must be at or below full scale"};
    }

    const SampleCount frames = audio.frames();
    const int order = settings.order;
    DeclipReport report;
    std::vector<float> original;

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        float* samples = audio.channel(channel);

        double peak = 0.0;
        for (SampleCount i = 0; i < frames; ++i) {
            peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
        }
        if (!(peak > 0.0)) {
            continue;
        }
        const double ceiling = peak * (1.0 - settings.tolerance);
        report.detectedCeiling = std::max(report.detectedCeiling, ceiling);

        for (const FlatTop& flat : findFlatTops(samples, frames, ceiling, settings.minimumRun)) {
            const SampleCount length = flat.end - flat.start;
            if (length > settings.maximumRun) {
                ++report.tooLong;
                continue;
            }

            // The model is fitted either side of the damage, over a window
            // wide enough to describe the material. It necessarily contains
            // the flat top itself, which biases it a little towards a signal
            // that really does sit still at the ceiling -- there is nothing
            // else to fit to, and the bias is small where clipping is
            // occasional. Where it is not occasional, the runs are long and
            // maximumRun refuses them anyway.
            const SampleIndex fitStart =
                std::max<SampleIndex>(0, flat.start - settings.blockSize / 2);
            const SampleIndex fitEnd =
                std::min<SampleIndex>(frames, flat.end + settings.blockSize / 2);
            const SampleCount span = fitEnd - fitStart;
            if (span < static_cast<SampleCount>(2 * order)) {
                ++report.unsolved;
                continue;
            }

            auto fitted = fitLinearPrediction(samples + fitStart, span, order);
            if (!fitted) {
                ++report.unsolved;
                continue;
            }

            original.assign(samples + flat.start, samples + flat.end);
            if (!interpolateThroughModel(fitted.value(), samples, frames, flat.start, flat.end)) {
                ++report.unsolved;
                continue;
            }

            // What is known about the answer, imposed. A clipped sample was at
            // least as large as the ceiling it hit, in the direction it hit it,
            // so a restored value that comes back smaller is wrong however well
            // it fits the model -- and putting a dent where there was a flat
            // top would be a worse artefact than the flat top.
            for (SampleCount i = 0; i < length; ++i) {
                const float was = original[static_cast<std::size_t>(i)];
                float& now = samples[flat.start + i];
                if (flat.positive ? now < was : now > was) {
                    now = was;
                }
            }

            // Did the model actually say the signal went higher? If not, this
            // was an ordinary crest that happened to be flat, and the honest
            // thing is to put it back untouched. See
            // DeclipSettings::minimumRecovery.
            double reached = 0.0;
            for (SampleCount i = 0; i < length; ++i) {
                reached = std::max(reached, std::abs(static_cast<double>(samples[flat.start + i])));
            }
            if (reached < ceiling * (1.0 + settings.minimumRecovery)) {
                std::copy(original.begin(), original.end(), samples + flat.start);
                continue;
            }

            ++report.runs;
            report.samplesRestored += length;
            report.longestRun = std::max(report.longestRun, length);
        }
    }

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        for (SampleCount i = 0; i < frames; ++i) {
            report.restoredPeak = std::max(
                report.restoredPeak, std::abs(static_cast<double>(audio.channel(channel)[i])));
        }
    }

    if (settings.fitToCeiling && report.runs > 0) {
        const double ceiling = std::pow(10.0, settings.ceilingDb / 20.0);
        if (report.restoredPeak > ceiling) {
            const double gain = ceiling / report.restoredPeak;
            for (int channel = 0; channel < audio.channelCount(); ++channel) {
                float* samples = audio.channel(channel);
                for (SampleCount i = 0; i < frames; ++i) {
                    samples[i] = static_cast<float>(static_cast<double>(samples[i]) * gain);
                }
            }
            report.gainDb = 20.0 * std::log10(gain);
        }
    }

    return report;
}

} // namespace sa::dsp
