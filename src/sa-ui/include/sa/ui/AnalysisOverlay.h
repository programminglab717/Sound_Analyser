#pragma once

#include <sa/analysis/OctaveBands.h>
#include <sa/analysis/PitchTrack.h>
#include <sa/core/Types.h>
#include <sa/ui/ViewGeometry.h>

#include <vector>

/// Where a pitch contour, a beat grid and a band chart land in pixels.
///
/// Split out of the widgets that draw them for the reason ViewGeometry.h gives
/// for SpectrumPlot: this is the arithmetic that can be wrong, and no window is
/// needed to be wrong in one. The mappings themselves are not repeated here --
/// TimePlot and SpectrumPlot own them -- so what is left is the part that is
/// about the *data*: where a contour breaks, which beats are on screen, and how
/// wide a band is.
namespace sa::ui {

/// The vertical axis a pitch contour is drawn on.
///
/// Logarithmic in hertz, because an octave has to be an octave wherever it
/// falls: a linear axis puts the top octave of a soprano's range in half the
/// panel and a bass's whole range in a tenth of it.
///
/// The range is the tracker's own -- PitchSettings::minHz and maxHz -- rather
/// than anything chosen for the picture. A contour cannot leave an axis drawn
/// at the bounds the contour was searched within, so the axis never has to
/// misrepresent a reading by pinning it to an edge.
struct PitchAxis {
    double lowHz = 50.0;
    double highHz = 1000.0;
    int top = 0;
    int height = 1;

    /// Row a frequency is drawn at. Frequencies off the axis are *not*
    /// clamped: they return a row outside [top, top + height), and
    /// pitchRuns() breaks the contour there rather than ruling a line along
    /// the edge of the panel.
    [[nodiscard]] int yAtHz(double hz) const noexcept;

    [[nodiscard]] double hzAtY(int y) const noexcept;

    [[nodiscard]] bool holds(double hz) const noexcept { return hz >= lowHz && hz <= highHz; }
};

/// Labelled positions on a pitch axis, at the 1-2-5 places per decade.
///
/// The same ladder frequencyTicks() climbs, and a separate function rather than
/// a call into it because this axis is bounded at both ends by the tracker's
/// own range instead of running from 20 Hz to Nyquist. `fraction` is measured
/// from the bottom of the axis, as it is everywhere else in ViewGeometry.h.
///
/// Worth drawing at all because a contour with no scale beside it says only
/// that the pitch went up and down, which is the one thing about it a listener
/// already knew.
[[nodiscard]] std::vector<AxisTick> pitchTicks(const PitchAxis& axis);

struct ContourPoint {
    int x = 0;
    int y = 0;
};

/// The contour as runs of consecutive points, each drawn as one polyline.
///
/// The runs are the whole point. PitchTrack.h sets `hz` to zero where nothing
/// periodic was found precisely so that a drawing breaks there instead of
/// ruling a line through noise, and a caller that flattened the contour into
/// one polyline would throw that away -- joining the last note before a rest to
/// the first note after it with a straight line that reads as a glissando
/// nobody played.
///
/// A run also ends at a point the axis cannot hold, for the same reason.
///
/// Points outside the plot horizontally are kept, and the painter clips them.
/// Dropping them would start the visible line at the first on-screen frame
/// rather than at the edge, which moves the contour by up to one hop wherever
/// the view is scrolled.
[[nodiscard]] std::vector<std::vector<ContourPoint>>
pitchRuns(const std::vector<analysis::PitchPoint>& contour, SampleIndex startSample,
          SampleRate rate, const TimePlot& time, const PitchAxis& axis);

/// The columns the beats fall in, in order, and only those on screen.
///
/// `startSample` is where the analysed span begins in the document, because a
/// beat time is measured from the start of what was analysed and the axis is
/// drawn against the document.
[[nodiscard]] std::vector<int> beatColumns(const std::vector<double>& beatSeconds,
                                           SampleIndex startSample, SampleRate rate,
                                           const TimePlot& time);

/// One band's bar on the spectrum panel's axes.
struct BandBar {
    int left = 0;  ///< First column of the bar.
    int right = 0; ///< Last column of the bar, inclusive.
    int top = 0;   ///< Row of the band's level.
};

/// Where each band sits on the spectrum panel.
///
/// Drawn to the band's own edges rather than as an even share of the axis: the
/// edges are what was integrated, and a third-octave band is a fixed ratio, so
/// on the panel's logarithmic axis every bar comes out the same width except
/// where the bottom of the axis cuts one short. That is the picture that can be
/// read against the curve behind it.
///
/// Bands whose level is at or below the bottom of the axis are still returned,
/// with `top` on that bottom row, so that a caller counting bars gets the
/// layout and not a filtered version of it.
[[nodiscard]] std::vector<BandBar> bandBars(const std::vector<analysis::Band>& bands,
                                            const SpectrumPlot& plot);

} // namespace sa::ui
