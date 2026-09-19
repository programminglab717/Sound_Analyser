#pragma once

#include <sa/core/Types.h>

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

/// A selected span of time, in samples. Empty means nothing is selected and the
/// `start` is a caret: where a paste or an insert would land.
struct TimeSelection {
    SampleIndex start = 0;
    SampleIndex end = 0;

    [[nodiscard]] bool isEmpty() const noexcept { return end <= start; }

    [[nodiscard]] SampleCount length() const noexcept { return isEmpty() ? 0 : end - start; }

    [[nodiscard]] friend bool operator==(const TimeSelection& a, const TimeSelection& b) noexcept {
        return a.start == b.start && a.end == b.end;
    }
};

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

/// Top and bottom of the spectrum panel's level axis, in dBFS.
///
/// Zero at the top because that is full scale and there is nothing above it to
/// show; -108 at the bottom because it is a round eighteen decades of nothing
/// and puts the noise floor of a 16-bit delivery comfortably on the display.
inline constexpr double kSpectrumTopDb = 0.0;
inline constexpr double kSpectrumBottomDb = -108.0;

/// The level the EQ curve's 0 dB line is drawn at, in dBFS.
///
/// Mid-axis, so that a full boost and a full cut both have room before the
/// curve leaves the panel. Which line it is matters far less than the scale it
/// is drawn on: gain uses the spectrum's own ruler, so a 6 dB boost is exactly
/// as tall as the 6 dB peak someone is using it to correct. A curve on its own
/// private decibel scale looks like an EQ and cannot be read against anything.
///
/// -54 rather than any other middle, because the panel's level grid runs every
/// 12 dB from the top: the lines at -48 and -60 dBFS are then exactly +6 and
/// -6 dB of EQ, so the curve gets a gain scale without a single line drawn for
/// it.
inline constexpr double kEqZeroLevelDb = -54.0;

/// The plotting area of the spectrum panel, and the arithmetic mapping it to
/// frequency and level.
///
/// A plain value rather than something the widget keeps to itself, because two
/// things draw in this space now -- the spectrum, and the EQ curve over it --
/// and a boost that does not sit exactly over the peak it is correcting is
/// worse than no curve at all. It is also what lets every one of these
/// mappings be checked without a window.
///
/// `width` counts columns, so the rightmost is `left + width - 1`, and
/// `height` counts rows the same way. Neither covers the label strip below the
/// plot, which is the widget's business and not the axis's.
struct SpectrumPlot {
    int left = 0;
    int top = 0;
    int width = 1;
    int height = 1;
    double nyquistHz = 24000.0;

    /// Frequency at the left edge of column `x`, on the logarithmic axis the
    /// panel draws. Columns outside the plot clamp to its ends rather than
    /// running off the bottom of a log axis.
    [[nodiscard]] double frequencyAtX(int x) const noexcept;

    [[nodiscard]] int xAtFrequency(double hz) const noexcept;

    /// Row a level in dBFS is drawn at. Levels off the axis clamp to its ends.
    [[nodiscard]] int yAtLevel(double decibels) const noexcept;

    /// Inverse of yAtLevel, to within the rounding that chose the row.
    [[nodiscard]] double levelAtY(int y) const noexcept;

    /// Row an EQ gain is drawn at: yAtLevel's ruler, shifted so that 0 dB of
    /// gain sits on kEqZeroLevelDb.
    [[nodiscard]] int yAtGain(double gainDb) const noexcept {
        return yAtLevel(kEqZeroLevelDb + gainDb);
    }

    [[nodiscard]] double gainAtY(int y) const noexcept { return levelAtY(y) - kEqZeroLevelDb; }
};

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
