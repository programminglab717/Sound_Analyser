#pragma once

#include <sa/core/Types.h>
#include <sa/ui/ViewGeometry.h>

#include <QWidget>

namespace sa::ui {

/// Everything a view sharing the document's time axis does, minus the drawing.
///
/// The waveform and the spectrogram must zoom, pan, select and scroll
/// identically -- they are two renderings of one timeline, and a user who
/// selects in one and finds a different selection in the other has been lied
/// to. Writing that behaviour twice guarantees it drifts, so it lives here once
/// and each view supplies only its own plot and its own gutter.
class TimeAxisView : public QWidget {
    Q_OBJECT

public:
    explicit TimeAxisView(QWidget* parent = nullptr);

    /// Tell the view how long the document is and how fast it runs. Resets the
    /// visible range to the whole thing.
    void setTimeline(SampleRate rate, SampleCount totalFrames);

    void setViewRange(SampleIndex start, SampleCount length);

    [[nodiscard]] SampleIndex viewStart() const noexcept { return viewStart_; }

    [[nodiscard]] SampleCount viewLength() const noexcept { return viewLength_; }

    void setSelection(TimeSelection selection);

    [[nodiscard]] const TimeSelection& selection() const noexcept { return selection_; }

    /// Where the transport is, in samples. Negative hides it.
    void setPlayhead(SampleIndex position);

    [[nodiscard]] SampleIndex playhead() const noexcept { return playhead_; }

    void zoom(double factor, double anchorFraction);
    void scrollBySamples(SampleIndex delta);
    void showAll();
    void zoomToSelection();

    /// The plot area and its time mapping, as a value.
    ///
    /// Public for the same reason SpectrumView::plot() is: anything drawn over
    /// this view has to land on the axis the view itself used, and handing out
    /// the mapping is cheaper than every caller being trusted to rebuild it.
    [[nodiscard]] TimePlot timePlot() const noexcept;

signals:
    void viewRangeChanged(SampleIndex start, SampleCount length);
    void selectionChanged(SampleIndex start, SampleIndex end);

protected:
    /// Draw the data. `plot` excludes the gutter; nothing else may be drawn
    /// outside it, because the gutter is painted over afterwards.
    virtual void paintPlot(QPainter& painter, const QRect& plot) = 0;

    /// Draw the vertical scale into the left gutter, and any grid lines that
    /// belong with it.
    virtual void paintGutter(QPainter& painter) = 0;

    /// Called whenever the visible range or the widget size changes, before the
    /// next paint. A view caching a rendered image invalidates it here.
    virtual void viewInvalidated() {}

    [[nodiscard]] QRect plotRect() const noexcept;
    [[nodiscard]] int plotWidth() const noexcept;
    [[nodiscard]] SampleIndex sampleAtX(int x) const noexcept;
    /// Widget x for a sample, which may fall outside the plot.
    [[nodiscard]] int xForSample(SampleIndex sample) const noexcept;

    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

    /// Called on every mouse move that is not a drag, with the widget position.
    /// Views override it to publish a readout.
    virtual void hover(const QPoint& position);

    /// True for a view whose vertical axis is selectable too -- the
    /// spectrogram, where a drag picks a frequency band as well as a time span.
    [[nodiscard]] virtual bool selectsVertically() const noexcept { return false; }

    /// Vertical extent of the current drag, as fractions from the bottom of the
    /// plot. Only called when selectsVertically() is true. A drag with no
    /// vertical extent -- a click, or a drag along the axis -- reports the full
    /// range, which reads as "all frequencies" and is what the user means.
    virtual void verticalSelectionChanged(double lowFraction, double highFraction);

    /// The rectangle the selection overlay shades. Full height by default; a
    /// view that also selects vertically narrows it.
    [[nodiscard]] virtual QRect selectionRect(const QRect& plot, int left, int right) const;

    SampleRate rate_{48000.0};
    SampleCount totalFrames_ = 0;
    SampleIndex viewStart_ = 0;
    SampleCount viewLength_ = 0;
    TimeSelection selection_;
    SampleIndex playhead_ = -1;

private:
    enum class Drag { None, Panning, Selecting };

    void clampView();
    void emitSelection();
    void reportVerticalSelection(int fromY, int toY);

    Drag drag_ = Drag::None;
    int dragAnchorX_ = 0;
    int dragAnchorY_ = 0;
    SampleIndex dragAnchorStart_ = 0;
    SampleIndex selectionAnchor_ = 0;
};

} // namespace sa::ui
