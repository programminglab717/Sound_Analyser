#pragma once

#include <sa/io/PeakPyramid.h>
#include <sa/ui/TimeAxisView.h>

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
class WaveformView : public TimeAxisView {
    Q_OBJECT

public:
    explicit WaveformView(QWidget* parent = nullptr);

    /// Takes a pyramid to display. Passing nullptr clears the view.
    void setPyramid(std::shared_ptr<const io::PeakPyramid> pyramid, SampleRate rate);

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
    std::shared_ptr<const io::PeakPyramid> pyramid_;
    std::vector<io::PeakFrame> columns_;
};

} // namespace sa::ui
