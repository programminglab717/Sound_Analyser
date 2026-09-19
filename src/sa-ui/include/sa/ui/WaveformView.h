#pragma once

#include <sa/analysis/PitchTrack.h>
#include <sa/io/PeakPyramid.h>
#include <sa/ui/AnalysisOverlay.h>
#include <sa/ui/TimeAxisView.h>

#include <memory>
#include <vector>

namespace sa::ui {

/// Waveform display backed by the peak pyramid.
///
/// Draws one column per pixel from the pyramid rather than from samples, so the
/// cost tracks the width of the widget and not the length of the file. A
/// two-hour recording draws in the same time as a two-second one.
///
/// Min and max are drawn as the outer envelope and RMS as a brighter inner
/// body, which is the shape engineers read at a glance: the gap between them is
/// crest factor, so over-compressed material looks visibly solid.
///
/// Two analyses are drawn over it, for one reason each.
///
/// The beat grid, because a tempo is the one number in this product a reader
/// can check by eye. 119.8 and 239.6 BPM are both plausible answers to the same
/// record and a panel cannot tell them apart; a grid laid over the transients
/// can, in a second, and without it the figure has to be taken on trust.
///
/// The pitch contour, because a contour is a statement about time and belongs
/// on the axis time is read from. It gets its own logarithmic hertz axis over
/// the full height of the plot -- the waveform's amplitude axis is a different
/// thing and the two are not reconcilable -- with the scale drawn at the right
/// so the height of the line can be read rather than merely compared.
///
/// What is not drawn: bar lines. TempoTrack.h finds beats and not metre, so
/// every beat is drawn alike and none is marked as a downbeat.
class WaveformView : public TimeAxisView {
    Q_OBJECT

public:
    explicit WaveformView(QWidget* parent = nullptr);

    /// Takes a pyramid to display. Passing nullptr clears the view.
    void setPyramid(std::shared_ptr<const io::PeakPyramid> pyramid, SampleRate rate);

    /// Lay a beat grid over the waveform. `startSample` is where the analysed
    /// span begins in the document, because the times are measured from there.
    ///
    /// `doubtful` draws the grid dashed instead of solid. A grid fitted to an
    /// onset envelope that barely repeats is a proposal rather than a
    /// measurement, and it should not look like the other kind.
    void setBeatGrid(std::vector<double> beatSeconds, SampleIndex startSample, bool doubtful);

    /// Draw a pitch contour on a logarithmic axis running from `lowHz` to
    /// `highHz` -- the tracker's own range, so that no reading can fall off it.
    void setPitchContour(std::vector<analysis::PitchPoint> contour, SampleIndex startSample,
                         double lowHz, double highHz);

    /// Where the analysed span ends, in samples, or a negative value when the
    /// whole of what was asked about was analysed.
    ///
    /// Drawn as a line across the plot when anything above is on screen. A
    /// contour and a beat grid that stop two minutes into a five-minute file
    /// otherwise look like an analysis that failed rather than one that was
    /// bounded, and the picture has to say which.
    void setAnalysedEnd(SampleIndex end);

    void clearOverlays();

signals:
    /// Time and peak level under the pointer. `seconds` is negative when the
    /// pointer is outside the plot.
    void cursorMoved(double seconds, double peakDecibels);

protected:
    void paintPlot(QPainter& painter, const QRect& plot) override;
    void paintGutter(QPainter& painter) override;
    void hover(const QPoint& position) override;
    void leaveEvent(QEvent* event) override;

private:
    /// The axis the contour is drawn on, over the whole height of the plot.
    [[nodiscard]] PitchAxis pitchAxis(const QRect& plot) const noexcept;

    void paintBeats(QPainter& painter, const QRect& plot);
    void paintContour(QPainter& painter, const QRect& plot);
    void paintAnalysedEnd(QPainter& painter, const QRect& plot);

    std::shared_ptr<const io::PeakPyramid> pyramid_;
    std::vector<io::PeakFrame> columns_;

    std::vector<double> beatSeconds_;
    SampleIndex beatStart_ = 0;
    bool beatsDoubtful_ = false;

    std::vector<analysis::PitchPoint> contour_;
    SampleIndex contourStart_ = 0;
    double contourLowHz_ = 50.0;
    double contourHighHz_ = 1000.0;

    SampleIndex analysedEnd_ = -1;
};

} // namespace sa::ui
