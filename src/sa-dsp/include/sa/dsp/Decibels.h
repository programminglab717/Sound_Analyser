#pragma once

#include <algorithm>
#include <cmath>

namespace sa::dsp {

/// The level treated as silence.
///
/// Gain computers take the logarithm of the input every sample, and a signal
/// crosses zero constantly -- so without a floor the control path spends part
/// of every cycle holding an infinity. -200 dBFS is 56 dB below the noise floor
/// of 24-bit delivery, so nothing that survives to an output is ever clamped
/// here, and the control path stays finite.
inline constexpr double kSilenceDecibels = -200.0;

/// Linear gain for a level in decibels. Anything at or below the silence floor
/// maps to exact zero rather than a very small number, so muting is really mute.
[[nodiscard]] inline double decibelsToGain(double decibels) noexcept {
    return decibels <= kSilenceDecibels ? 0.0 : std::pow(10.0, decibels * 0.05);
}

/// Level in decibels of a linear gain, floored at kSilenceDecibels. Takes the
/// magnitude, so it reads a waveform sample as well as a gain.
[[nodiscard]] inline double gainToDecibels(double gain) noexcept {
    const double magnitude = std::abs(gain);
    return magnitude <= 0.0 ? kSilenceDecibels
                            : std::max(kSilenceDecibels, 20.0 * std::log10(magnitude));
}

} // namespace sa::dsp
