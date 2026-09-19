#include <sa/dsp/Declick.h>
#include <sa/dsp/LinearPrediction.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace sa::dsp {

namespace {

/// Median of a half-normal distribution, as a fraction of its deviation. The
/// constant that turns a median absolute residual into a standard deviation
/// without letting the clicks -- which is exactly what is being looked for --
/// inflate the scale they are measured against.
constexpr double kMedianToDeviation = 1.4826;

/// Lifts the diagonal of the interpolation system, relatively. The system is
/// singular wherever the model has a perfect null, which a synthetic tone
/// obligingly provides; this is far below the audio and makes the solve
/// well-posed there.
constexpr double kSolveRidge = 1e-9;

struct Run {
    SampleIndex start = 0;
    SampleIndex end = 0; // Exclusive.
};

/// Solves a symmetric positive-definite system in place by Cholesky.
/// False where the matrix is not positive definite, which the caller treats as
/// "this passage cannot be interpolated" rather than as an error.
[[nodiscard]] bool solveSymmetric(std::vector<double>& matrix, std::vector<double>& rhs, int n) {
    const auto at = [n](std::vector<double>& m, int row, int column) -> double& {
        return m[static_cast<std::size_t>(row) * static_cast<std::size_t>(n) +
                 static_cast<std::size_t>(column)];
    };

    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            double total = at(matrix, i, j);
            for (int k = 0; k < j; ++k) {
                total -= at(matrix, i, k) * at(matrix, j, k);
            }
            if (i == j) {
                if (!(total > 0.0) || !std::isfinite(total)) {
                    return false;
                }
                at(matrix, i, i) = std::sqrt(total);
            } else {
                at(matrix, i, j) = total / at(matrix, j, j);
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        double total = rhs[static_cast<std::size_t>(i)];
        for (int k = 0; k < i; ++k) {
            total -= at(matrix, i, k) * rhs[static_cast<std::size_t>(k)];
        }
        rhs[static_cast<std::size_t>(i)] = total / at(matrix, i, i);
    }
    for (int i = n - 1; i >= 0; --i) {
        double total = rhs[static_cast<std::size_t>(i)];
        for (int k = i + 1; k < n; ++k) {
            total -= at(matrix, k, i) * rhs[static_cast<std::size_t>(k)];
        }
        rhs[static_cast<std::size_t>(i)] = total / at(matrix, i, i);
    }
    return true;
}

/// Replaces [gap.start, gap.end) with the values the model finds least
/// surprising given everything within `order` samples either side.
///
/// The derivation, because the result looks like magic otherwise. Minimising
/// the residual energy over every window that touches the gap, with respect to
/// each unknown sample, gives one equation per unknown:
///
///     sum over all q of R[q - m] * x[q] = 0,   R[d] = sum over t of a[t]a[t-d]
///
/// Splitting q into the unknowns and the knowns turns that into a symmetric
/// system whose matrix is R of the index difference -- Toeplitz, banded by the
/// filter order, and positive definite for any model with power in it.
[[nodiscard]] bool interpolate(float* channel, SampleCount frames, const Run& gap,
                               const LinearPrediction& prediction) {
    const int order = prediction.order();
    const auto length = static_cast<int>(gap.end - gap.start);
    if (length <= 0 || order <= 0) {
        return false;
    }
    if (gap.start - order < 0 || gap.end + order > frames) {
        return false;
    }

    std::vector<double> correlation(static_cast<std::size_t>(order) + 1, 0.0);
    for (int lag = 0; lag <= order; ++lag) {
        double total = 0.0;
        for (int k = 0; k + lag <= order; ++k) {
            total += prediction.coefficients[static_cast<std::size_t>(k)] *
                     prediction.coefficients[static_cast<std::size_t>(k + lag)];
        }
        correlation[static_cast<std::size_t>(lag)] = total;
    }
    if (!(correlation[0] > 0.0)) {
        return false;
    }

    const auto r = [&](SampleIndex difference) {
        const auto distance = static_cast<SampleIndex>(std::abs(difference));
        return distance <= order ? correlation[static_cast<std::size_t>(distance)] : 0.0;
    };

    std::vector<double> matrix(static_cast<std::size_t>(length) * static_cast<std::size_t>(length),
                               0.0);
    std::vector<double> rhs(static_cast<std::size_t>(length), 0.0);

    for (int i = 0; i < length; ++i) {
        for (int j = 0; j < length; ++j) {
            matrix[static_cast<std::size_t>(i) * static_cast<std::size_t>(length) +
                   static_cast<std::size_t>(j)] = r(i - j);
        }
        matrix[static_cast<std::size_t>(i) * static_cast<std::size_t>(length) +
               static_cast<std::size_t>(i)] += correlation[0] * kSolveRidge;

        const SampleIndex here = gap.start + i;
        double total = 0.0;
        for (SampleIndex q = here - order; q <= here + order; ++q) {
            if (q >= gap.start && q < gap.end) {
                continue; // Unknown: it belongs on the other side.
            }
            total += r(q - here) * static_cast<double>(channel[q]);
        }
        rhs[static_cast<std::size_t>(i)] = -total;
    }

    if (!solveSymmetric(matrix, rhs, length)) {
        return false;
    }
    for (int i = 0; i < length; ++i) {
        const double value = rhs[static_cast<std::size_t>(i)];
        if (!std::isfinite(value)) {
            return false;
        }
        channel[gap.start + i] = static_cast<float>(value);
    }
    return true;
}

} // namespace

Result<DeclickReport> declick(AudioBufferView audio, const DeclickSettings& settings) {
    if (audio.isEmpty()) {
        return Error{ErrorCode::InvalidArgument, "there is nothing to declick"};
    }
    if (settings.order < 1 || settings.order > kMaximumPredictionOrder) {
        return Error{ErrorCode::OutOfRange, "prediction order is outside 1 to 256"};
    }
    if (settings.blockSize < 4 * settings.order) {
        return Error{ErrorCode::InvalidArgument,
                     "a block must be at least four times the model order"};
    }
    if (settings.maximumGap < 1 || settings.maximumGap > kMaximumDeclickGap) {
        return Error{ErrorCode::OutOfRange, "the longest repair is outside 1 to 512 samples"};
    }
    if (settings.guard < 0 || settings.guard > settings.maximumGap) {
        return Error{ErrorCode::OutOfRange, "the guard is negative or longer than a repair"};
    }
    if (settings.mergeDistance < 0 || settings.mergeDistance > kMaximumDeclickGap) {
        return Error{ErrorCode::OutOfRange, "the merge distance is negative or absurdly long"};
    }
    if (!(settings.threshold > 0.0) || !std::isfinite(settings.threshold)) {
        return Error{ErrorCode::OutOfRange, "the threshold must be a positive number"};
    }

    const int order = settings.order;
    const SampleCount frames = audio.frames();
    DeclickReport report;

    std::vector<float> forward;
    std::vector<float> backward;
    std::vector<double> magnitudes;

    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        float* samples = audio.channel(channel);

        for (SampleIndex start = 0; start < frames; start += settings.blockSize) {
            const SampleIndex end = std::min<SampleIndex>(frames, start + settings.blockSize);
            // The model is fitted over a little more than the block it judges,
            // so the residual at the block's own edges has real history behind
            // it rather than the zeros the filter would otherwise assume.
            const SampleIndex fitStart = std::max<SampleIndex>(0, start - order);
            const SampleIndex fitEnd = std::min<SampleIndex>(frames, end + order);
            const SampleCount span = fitEnd - fitStart;
            if (span < static_cast<SampleCount>(2 * order)) {
                continue;
            }

            auto fitted = fitLinearPrediction(samples + fitStart, span, order);
            if (!fitted) {
                continue; // Silence, most often. Nothing to find in it.
            }

            forward.resize(static_cast<std::size_t>(span));
            backward.resize(static_cast<std::size_t>(span));
            predictionResidual(fitted.value(), samples + fitStart, span, forward.data());
            reversePredictionResidual(fitted.value(), samples + fitStart, span, backward.data());

            magnitudes.resize(static_cast<std::size_t>(span));
            for (SampleCount i = 0; i < span; ++i) {
                magnitudes[static_cast<std::size_t>(i)] =
                    std::abs(static_cast<double>(forward[static_cast<std::size_t>(i)]));
            }

            // A median, not a mean, so that the clicks -- which is exactly what
            // is being looked for -- do not inflate the scale they are measured
            // against. It is taken over the forward residual alone rather than
            // over any combination of the two, because a combination has a
            // different distribution and a median of it is not a deviation of
            // anything: an earlier version took it over the smaller of the two
            // and understated the spread by about 1.8, so a nominal five sigma
            // was in truth about three and a half and found a click in nearly
            // every block of clean material.
            const auto middle = static_cast<std::ptrdiff_t>(magnitudes.size() / 2);
            std::nth_element(magnitudes.begin(), magnitudes.begin() + middle, magnitudes.end());
            const double median = magnitudes[static_cast<std::size_t>(middle)];
            const double scale = median * kMedianToDeviation;
            if (!(scale > 0.0)) {
                continue;
            }
            const double limit = settings.threshold * scale;

            // The first and last `order` samples of the whole signal have no
            // history on one side, so their residual is the signal rather than
            // a surprise. They are also the samples a repair could not use
            // anyway, since interpolation needs that much context either side.
            const SampleIndex firstJudged = std::max<SampleIndex>(start, order);
            const SampleIndex lastJudged = std::min<SampleIndex>(end, frames - order);

            // Runs, not samples.
            //
            // The obvious test -- is this sample surprising in both
            // directions -- is wrong, and measurably so. A click two samples
            // long is a step, and a predictor for correlated material has a
            // first coefficient near -1, so it finds the second sample of a
            // step entirely unsurprising: the per-sample minimum comes out at
            // the click's height times |1 + a1|, which measured as low as 6%
            // and missed a fifth of the clicks put in front of it.
            //
            // What is actually true is that a click makes the forward residual
            // hot *from* the click onward and the backward residual hot *up
            // to* it. So each residual's hot samples are grouped into runs,
            // each run is closed up over the gaps the model's own shape leaves
            // in it, and the damage is where a forward run and a backward run
            // overlap. For a single sample or for a burst, that lands on the
            // damage exactly.
            const auto collect = [&](const std::vector<float>& residual, int closeUp) {
                std::vector<Run> found;
                SampleIndex runStart = -1;
                for (SampleIndex at = firstJudged; at <= lastJudged; ++at) {
                    const bool hot =
                        at < lastJudged &&
                        std::abs(static_cast<double>(
                            residual[static_cast<std::size_t>(at - fitStart)])) > limit;
                    if (hot && runStart < 0) {
                        runStart = at;
                    } else if (!hot && runStart >= 0) {
                        if (!found.empty() && runStart - found.back().end <= closeUp) {
                            found.back().end = at;
                        } else {
                            found.push_back(Run{runStart, at});
                        }
                        runStart = -1;
                    }
                }
                return found;
            };

            // A ring through the model is at most `order` samples long, so
            // that is how far apart two hot samples can be and still be one
            // event rather than two.
            const std::vector<Run> ahead = collect(forward, order);
            const std::vector<Run> behind = collect(backward, order);

            std::vector<Run> runs;
            std::size_t b = 0;
            for (const Run& f : ahead) {
                while (b < behind.size() && behind[b].end <= f.start) {
                    ++b;
                }
                for (std::size_t j = b; j < behind.size() && behind[j].start < f.end; ++j) {
                    const Run overlap{std::max(f.start, behind[j].start),
                                      std::min(f.end, behind[j].end)};
                    if (overlap.end > overlap.start) {
                        runs.push_back(overlap);
                    }
                }
            }

            // Coalesce before judging length, so a burst that is surprising in
            // patches is one long piece of damage rather than a handful of
            // short ones the repairer would happily invent over.
            std::vector<Run> merged;
            for (const Run& found : runs) {
                if (!merged.empty() && found.start - merged.back().end <= settings.mergeDistance) {
                    merged.back().end = found.end;
                } else {
                    merged.push_back(found);
                }
            }

            for (const Run& found : merged) {
                Run gap{std::max<SampleIndex>(0, found.start - settings.guard),
                        std::min<SampleIndex>(frames, found.end + settings.guard)};
                if (gap.end - gap.start > settings.maximumGap) {
                    ++report.tooLong;
                    continue;
                }
                if (!interpolate(samples, frames, gap, fitted.value())) {
                    ++report.unsolved;
                    continue;
                }
                ++report.clicks;
                report.samplesRepaired += gap.end - gap.start;
                report.longestRepair = std::max(report.longestRepair, gap.end - gap.start);
            }
        }
    }

    return report;
}

} // namespace sa::dsp
