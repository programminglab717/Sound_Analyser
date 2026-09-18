#pragma once

#include <sa/core/Types.h>
#include <sa/ui/ViewGeometry.h>

#include <QWidget>

namespace sa::ui {

/// The shared time axis, drawn once above the views it labels.
///
/// It reserves the same left gutter as the waveform and the spectrogram, so a
/// tick at 0:30 is over the sample at 0:30 in both of them without any of the
/// three coordinating.
class TimeRuler : public QWidget {
    Q_OBJECT

public:
    explicit TimeRuler(QWidget* parent = nullptr);

    void setSampleRate(SampleRate rate);
    void setViewRange(SampleIndex start, SampleCount length);

    void setSelection(TimeSelection selection);

    /// Where the transport is, in samples. Negative hides it.
    void setPlayhead(SampleIndex position);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    SampleRate rate_{48000.0};
    SampleIndex viewStart_ = 0;
    SampleCount viewLength_ = 0;
    TimeSelection selection_;
    SampleIndex playhead_ = -1;
};

} // namespace sa::ui
