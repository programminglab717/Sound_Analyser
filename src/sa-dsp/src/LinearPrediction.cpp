#include <sa/dsp/LinearPrediction.h>
#include <sa/dsp/Window.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace sa::dsp {

namespace {

/// A ridge on the zero lag, in relative terms.
///
/// The autocorrelation matrix of a nearly periodic passage is nearly singular,
/// and Levinson-Durbin on a singular matrix produces reflection coefficients at
/// the edge of the unit circle and a filter that rings. Lifting the diagonal by
/// a millionth is far below anything audible and makes the recursion behave on
/// material -- a held sine, a synthetic test tone -- that is otherwise its
/// worst case.
constexpr double kRidge = 1e-6;

} // namespace

Result<LinearPrediction> fitLinearPrediction(const float* samples, SampleCount count, int order) {
    if (samples == nullptr) {
        return Error{ErrorCode::InvalidArgument, "no samples to fit"};
    }
    if (order < 1 || order > kMaximumPredictionOrder) {
        return Error{ErrorCode::OutOfRange, "prediction order is outside 1 to 256"};
    }
    if (count < static_cast<SampleCount>(2 * order)) {
        return Error{ErrorCode::InvalidArgument,
                     "a fit needs at least twice as many samples as it has poles"};
    }

    const Window window{WindowType::Hann, static_cast<int>(count)};

    std::vector<double> windowed(static_cast<std::size_t>(count));
    for (SampleCount i = 0; i < count; ++i) {
        windowed[static_cast<std::size_t>(i)] =
            static_cast<double>(samples[i]) * static_cast<double>(window[static_cast<int>(i)]);
    }

    std::vector<double> autocorrelation(static_cast<std::size_t>(order) + 1, 0.0);
    for (int lag = 0; lag <= order; ++lag) {
        double total = 0.0;
        for (SampleCount i = lag; i < count; ++i) {
            total +=
                windowed[static_cast<std::size_t>(i)] * windowed[static_cast<std::size_t>(i - lag)];
        }
        autocorrelation[static_cast<std::size_t>(lag)] = total;
    }

    if (!(autocorrelation[0] > 0.0)) {
        return Error{ErrorCode::InvalidArgument, "the passage is silent, so it has no model"};
    }
    autocorrelation[0] *= 1.0 + kRidge;

    LinearPrediction result;
    result.coefficients.assign(static_cast<std::size_t>(order) + 1, 0.0);
    result.coefficients[0] = 1.0;

    double residualPower = autocorrelation[0];
    std::vector<double> previous(static_cast<std::size_t>(order) + 1, 0.0);

    int fitted = 0;
    for (int step = 1; step <= order; ++step) {
        double accumulated = autocorrelation[static_cast<std::size_t>(step)];
        for (int j = 1; j < step; ++j) {
            accumulated += result.coefficients[static_cast<std::size_t>(j)] *
                           autocorrelation[static_cast<std::size_t>(step - j)];
        }

        const double reflection = -accumulated / residualPower;
        // A reflection coefficient at or past the unit circle means the
        // recursion has run out of information -- numerically, not musically.
        // Stopping leaves a shorter but stable filter, which is the right
        // answer; carrying on produces an unstable one, which is not.
        if (!std::isfinite(reflection) || std::abs(reflection) >= 1.0) {
            break;
        }

        std::copy(result.coefficients.begin(), result.coefficients.end(), previous.begin());
        for (int j = 1; j < step; ++j) {
            result.coefficients[static_cast<std::size_t>(j)] =
                previous[static_cast<std::size_t>(j)] +
                reflection * previous[static_cast<std::size_t>(step - j)];
        }
        result.coefficients[static_cast<std::size_t>(step)] = reflection;

        residualPower *= 1.0 - reflection * reflection;
        fitted = step;
        if (!(residualPower > 0.0)) {
            break;
        }
    }

    // Trailing coefficients from steps the recursion never reached are zero,
    // which is a filter of the order it actually managed. Reporting that order
    // rather than the one asked for keeps the residual loops honest.
    result.coefficients.resize(static_cast<std::size_t>(fitted) + 1);
    result.error = residualPower / static_cast<double>(count);
    return result;
}

void predictionResidual(const LinearPrediction& prediction, const float* samples, SampleCount count,
                        float* residual) noexcept {
    const int order = prediction.order();
    for (SampleCount i = 0; i < count; ++i) {
        double total = 0.0;
        const auto reach = static_cast<int>(std::min<SampleCount>(order, i));
        for (int k = 0; k <= reach; ++k) {
            total += prediction.coefficients[static_cast<std::size_t>(k)] *
                     static_cast<double>(samples[i - k]);
        }
        residual[i] = static_cast<float>(total);
    }
}

void reversePredictionResidual(const LinearPrediction& prediction, const float* samples,
                               SampleCount count, float* residual) noexcept {
    const int order = prediction.order();
    for (SampleCount i = count - 1; i >= 0; --i) {
        double total = 0.0;
        const auto reach = static_cast<int>(std::min<SampleCount>(order, count - 1 - i));
        for (int k = 0; k <= reach; ++k) {
            total += prediction.coefficients[static_cast<std::size_t>(k)] *
                     static_cast<double>(samples[i + k]);
        }
        residual[i] = static_cast<float>(total);
    }
}

} // namespace sa::dsp
