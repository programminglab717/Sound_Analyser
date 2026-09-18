#include <sa/dsp/WindowedSinc.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sa::dsp {

double sinc(double x) noexcept {
    if (x == 0.0) {
        return 1.0;
    }
    const double scaled = std::numbers::pi * x;
    return std::sin(scaled) / scaled;
}

double besselI0(double x) noexcept {
    // I0(x) = sum over k of ((x/2)^k / k!)^2. Each term follows from the last by
    // multiplying by (x/2k)^2, so no factorial is ever formed and nothing
    // overflows before the sum itself would.
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 128; ++k) {
        const double factor = x / (2.0 * static_cast<double>(k));
        term *= factor * factor;
        sum += term;
        if (term < 1e-18 * sum) {
            break;
        }
    }
    return sum;
}

double kaiserWindow(double position, double beta) noexcept {
    if (!(std::abs(position) <= 1.0)) {
        return 0.0;
    }
    // The square root is of a quantity that rounding can push a hair below zero
    // at the edges, where it would become a NaN rather than the zero it means.
    const double inside = std::max(0.0, 1.0 - position * position);
    return besselI0(beta * std::sqrt(inside)) / besselI0(beta);
}

double kaiserBeta(double stopbandDb) noexcept {
    if (stopbandDb > 50.0) {
        return 0.1102 * (stopbandDb - 8.7);
    }
    if (stopbandDb >= 21.0) {
        return 0.5842 * std::pow(stopbandDb - 21.0, 0.4) + 0.07886 * (stopbandDb - 21.0);
    }
    // Below 21 dB the rectangular window already does better than any beta, and
    // Kaiser's fit has no branch for it.
    return 0.0;
}

double kaiserLength(double stopbandDb, double transitionWidth) noexcept {
    if (!(transitionWidth > 0.0)) {
        return 0.0;
    }
    return (stopbandDb - 8.0) / (2.285 * 2.0 * std::numbers::pi * transitionWidth);
}

} // namespace sa::dsp
