#pragma once

#include <sa/core/Types.h>
#include <sa/ui/ViewGeometry.h>

#include <QString>
#include <QWidget>
#include <vector>

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

    /// A marker as the ruler needs to draw it.
    ///
    /// Deliberately not engine::Marker. The ruler is a view of a time axis and
    /// knows nothing about documents; copying the three fields it draws keeps
    /// it that way, and the copy is three fields.
    struct Mark {
        SampleIndex position = 0;
        SampleCount length = 0; ///< Zero for a point marker.
        QString label;
    };

    void setMarkers(std::vector<Mark> markers);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    SampleRate rate_{48000.0};
    SampleIndex viewStart_ = 0;
    SampleCount viewLength_ = 0;
    TimeSelection selection_;
    SampleIndex playhead_ = -1;
    std::vector<Mark> markers_;
};

} // namespace sa::ui
