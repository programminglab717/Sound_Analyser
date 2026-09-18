#pragma once

#include <string>
#include <vector>

namespace sa::ui {

/// Width in pixels of the scale gutter that every time-aligned view reserves on
/// its left.
///
/// Fixed rather than negotiated through a layout: the ruler, the waveform and
/// the spectrogram have to agree on where sample zero lands to the pixel, and a
/// shared constant guarantees that without any of them knowing about the
/// others.
inline constexpr int kGutterWidth = 62;

/// How the vertical axis of a spectrogram maps to frequency.
enum class FrequencyScale {
    Logarithmic, ///< The default: matches how hearing works, and how music is built.
    Linear,      ///< Even spacing in Hz. Right for harmonic and intermodulation work.
};

/// Bottom of a logarithmic frequency axis, in Hz.
///
/// Zero cannot be shown on a log axis and the bottom octaves carry almost no
/// programme content, so the axis starts here. Below this is infrasound and DC
/// offset, which the waveform shows better anyway.
inline constexpr double kLogAxisMinimumHz = 20.0;

/// Frequency displayed at `fraction` of the way up an axis, where 0 is the
/// bottom of the view and 1 the top.
[[nodiscard]] double frequencyAtFraction(FrequencyScale scale, double fraction,
                                         double nyquistHz) noexcept;

/// Inverse of frequencyAtFraction. Frequencies off the bottom of a log axis
/// clamp to 0 rather than running to negative infinity.
[[nodiscard]] double fractionAtFrequency(FrequencyScale scale, double hz,
                                         double nyquistHz) noexcept;

/// A labelled position on an axis, in the axis's own units.
struct AxisTick {
    double value = 0.0;    ///< Seconds, Hz or dB depending on the axis.
    double fraction = 0.0; ///< Where it sits, 0 to 1 along the axis.
    std::string label;
    bool major = true; ///< Minor ticks get a mark but no label.
};

/// Choose readable time ticks for a visible span.
///
/// Steps are taken from a fixed ladder of values an engineer already thinks in
/// -- milliseconds, then seconds, then the 15 s / 30 s / minute divisions of a
/// transport -- rather than from a general-purpose "nice number" algorithm,
/// which happily suggests a 2.5 second grid.
[[nodiscard]] std::vector<AxisTick> timeTicks(double startSeconds, double spanSeconds, int pixels);

/// Choose frequency ticks for a spectrogram axis.
///
/// On a log axis these are the 1-2-5 per decade positions everyone reads off an
/// EQ. On a linear axis they are evenly spaced round numbers.
[[nodiscard]] std::vector<AxisTick> frequencyTicks(FrequencyScale scale, double nyquistHz,
                                                   int pixels);

/// Format a time for a ruler label: m:ss, or s.mmm when the span is short
/// enough that minutes are noise.
[[nodiscard]] std::string formatTime(double seconds, double spanSeconds);

/// Format a frequency as Hz below 1 kHz and kHz above, without trailing zeros.
[[nodiscard]] std::string formatFrequency(double hz);

} // namespace sa::ui
