#pragma once

#include <sa/core/Result.h>
#include <sa/core/Types.h>

#include <vector>

namespace sa::dsp {

/// Linear prediction: the model that says what a sample should have been.
///
/// Fit a short all-pole filter to a passage and you have a compact description
/// of its spectral envelope -- and, more usefully here, a predictor. Run the
/// signal through the inverse of that filter and what comes out is whatever the
/// model could not account for. On ordinary material that residual is small and
/// noise-like; where something happened that the signal's own recent history
/// does not explain, it is not.
///
/// That is the basis of click detection, and the same coefficients then repair
/// the click, because a model that can say a sample is wrong can also say what
/// it should have been. It is also the standard route to a formant estimate, a
/// spectral envelope, and several of the forensic measures further down the
/// roadmap, which is why it lives here as a primitive rather than inside the
/// declicker.
///
/// The fit is the autocorrelation method with Levinson-Durbin recursion:
/// O(order²) rather than the O(order³) of a general solve, guaranteed to
/// produce a stable filter, and the standard choice everywhere this appears.
struct LinearPrediction {
    /// Prediction coefficients, `order + 1` of them. `a[0]` is 1 by
    /// construction and is stored anyway, so the residual is a plain
    /// convolution -- e[n] = sum over k of a[k] * x[n - k] -- rather than a
    /// special case at k = 0 that every caller has to remember.
    ///
    /// The prediction itself is therefore the negative of the rest:
    /// x̂[n] = -sum over k >= 1 of a[k] * x[n - k].
    std::vector<double> coefficients;

    /// Mean squared residual the fit achieved, in the same units as the input
    /// squared. Divided by the signal's own mean square it is the fraction of
    /// power the model failed to explain, which is how well-modelled a passage
    /// is -- near zero for a held note, close to one for noise.
    double error = 0.0;

    [[nodiscard]] int order() const noexcept {
        return coefficients.empty() ? 0 : static_cast<int>(coefficients.size()) - 1;
    }
};

/// Highest order worth asking for. Past this the autocorrelation estimate has
/// fewer independent lags than parameters and the fit describes the window
/// rather than the signal.
inline constexpr int kMaximumPredictionOrder = 256;

/// Fit a predictor of `order` poles to `count` samples.
///
/// A Hann window is applied before the autocorrelation, which is what makes the
/// method well conditioned: without it the implicit assumption that the signal
/// is zero outside the window puts a step at each end and the fit spends poles
/// describing it.
///
/// Fails if the order is outside 1 to kMaximumPredictionOrder, if there are
/// fewer than `2 * order` samples to fit from, or if the passage is silent --
/// silence has no envelope to describe, and returning a degenerate filter that
/// a caller will divide by is worse than saying so.
[[nodiscard]] Result<LinearPrediction> fitLinearPrediction(const float* samples, SampleCount count,
                                                           int order);

/// Residual of `prediction` over `samples`, written to `residual`.
///
/// Both buffers hold `count` samples and may be the same. The first `order`
/// outputs have incomplete history and are computed against zeros, so a caller
/// judging a threshold should either skip them or supply `order` samples of
/// run-up ahead of the region it cares about.
void predictionResidual(const LinearPrediction& prediction, const float* samples, SampleCount count,
                        float* residual) noexcept;

/// The same, run backwards: e[n] = sum over k of a[k] * x[n + k].
///
/// Worth having as its own function rather than as a reversed copy, because the
/// pair is what localises a click. A click rings *forward* through the forward
/// residual for as many samples as the filter has poles, and *backward* through
/// this one; where both are large at once is the click itself rather than its
/// echo through the model.
void reversePredictionResidual(const LinearPrediction& prediction, const float* samples,
                               SampleCount count, float* residual) noexcept;

} // namespace sa::dsp
