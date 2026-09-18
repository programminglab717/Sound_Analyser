#pragma once

#include <algorithm>
#include <cmath>

/// Loudness and level measurement.
///
/// Everything here is measured in double precision. The transport format is
/// float32 (docs/03-architecture.md §4), but a gated loudness integration is a
/// sum over hours of audio and a true-peak search is a 100-tap convolution per
/// sample; float accumulators visibly move the answer in both.
namespace sa::analysis {

/// Reported in place of a true -inf.
///
/// Silence has no finite level, and -inf does not stay put: it poisons any
/// average it enters, serialises badly, and turns a meter into a blank rather
/// than a pinned one. Every level in this module is therefore clamped here.
/// -200 dB is ~1e-10 full scale -- below the noise floor of any converter that
/// exists, so a reading of exactly kDecibelFloor means "nothing measurable"
/// rather than "very quiet indeed".
inline constexpr double kDecibelFloor = -200.0;

/// Amplitude ratio to dB. Sign is discarded; the floor is never crossed.
[[nodiscard]] inline double amplitudeToDecibels(double amplitude) noexcept {
    const double magnitude = std::abs(amplitude);
    if (!(magnitude > 0.0)) {
        // Also catches NaN, which would otherwise propagate out of std::log10.
        return kDecibelFloor;
    }
    return std::max(kDecibelFloor, 20.0 * std::log10(magnitude));
}

/// Power (mean square) ratio to dB.
[[nodiscard]] inline double powerToDecibels(double power) noexcept {
    if (!(power > 0.0)) {
        return kDecibelFloor;
    }
    return std::max(kDecibelFloor, 10.0 * std::log10(power));
}

/// dB back to an amplitude ratio. The floor maps to exact zero so that a
/// round trip through silence does not resurrect a 1e-10 signal.
[[nodiscard]] inline double decibelsToAmplitude(double decibels) noexcept {
    if (decibels <= kDecibelFloor) {
        return 0.0;
    }
    return std::pow(10.0, decibels / 20.0);
}

} // namespace sa::analysis
