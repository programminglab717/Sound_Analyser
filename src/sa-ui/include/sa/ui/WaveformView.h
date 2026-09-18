#pragma once

#include <sa/io/PeakPyramid.h>
#include <sa/ui/ViewGeometry.h>

#include <QWidget>
#include <memory>

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
class WaveformView : public QWidget {
    Q_OBJECT

public:
    explicit WaveformView(QWidget* parent = nullptr);

    /// Takes a pyramid to display. Passing nullptr clears the view.
    void setPyramid(std::shared_ptr<const io::PeakPyramid> pyramid, SampleRate rate);

    /// Visible range, in samples.
    void setViewRange(SampleIndex start, SampleCount length);

    [[nodiscard]] SampleIndex viewStart() const noexcept { return viewStart_; }

    [[nodiscard]] SampleCount viewLength() const noexcept { return viewLength_; }

    void zoom(double factor, double anchorFraction);
    void scrollBySamples(SampleIndex delta);
    void showAll();

signals:
    void viewRangeChanged(SampleIndex start, SampleCount length);

    /// Time and peak level under the pointer. `seconds` is negative when the
    /// pointer is outside the plot.
    void cursorMoved(double seconds, double peakDecibels);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void clampView();
    void paintGutter(QPainter& painter);
    [[nodiscard]] int plotWidth() const noexcept;

    std::shared_ptr<const io::PeakPyramid> pyramid_;
    SampleRate rate_{48000.0};
    SampleIndex viewStart_ = 0;
    SampleCount viewLength_ = 0;

    std::vector<io::PeakFrame> columns_;
    bool dragging_ = false;
    int dragAnchorX_ = 0;
    SampleIndex dragAnchorStart_ = 0;
};

} // namespace sa::ui
