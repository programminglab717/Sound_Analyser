#include <sa/ui/WaveformView.h>

#include <QPainter>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace sa::ui {

namespace {

constexpr QColor kBackground{0x14, 0x15, 0x1a};
constexpr QColor kCentreLine{0x2c, 0x2e, 0x38};
constexpr QColor kEnvelope{0x4d, 0x9d, 0xe0};
constexpr QColor kBody{0x9b, 0xd1, 0xf5};
constexpr QColor kText{0x8a, 0x8f, 0xa0};
constexpr QColor kGutterBackground{0x16, 0x17, 0x1d};
constexpr QColor kGutterLine{0x4a, 0x4e, 0x5e};
constexpr QColor kGutterLabel{0xa8, 0xad, 0xbd};

/// Two hues that nothing else in this view uses. The waveform is blue, the
/// playhead amber and the selection a pale blue, so a magenta beat line and a
/// green contour cannot be confused with any of them -- which matters, because
/// the whole purpose of the grid is to be told apart from the audio under it.
constexpr QColor kBeatLine{0xc7, 0x7d, 0xff};
constexpr QColor kContour{0x3f, 0xd0, 0x7f};
constexpr QColor kContourScale{0x5a, 0x80, 0x68};
constexpr QColor kBoundary{0x6d, 0x73, 0x82};

/// The waveform is drawn on a linear amplitude axis but labelled in dBFS, which
/// is the unit the levels actually get discussed in. -6 dB is half scale, so
/// the label sits halfway up; that non-uniform spacing is the honest picture.
constexpr std::array<double, 4> kLevelLabelsDb = {0.0, -6.0, -12.0, -20.0};

[[nodiscard]] double amplitudeToDecibels(double amplitude) noexcept {
    return amplitude > 1e-9 ? 20.0 * std::log10(amplitude) : -144.0;
}

} // namespace

WaveformView::WaveformView(QWidget* parent) : TimeAxisView(parent) {
    setMinimumHeight(90);
}

void WaveformView::setPyramid(std::shared_ptr<const io::PeakPyramid> pyramid, SampleRate rate) {
    pyramid_ = std::move(pyramid);
    setTimeline(rate, pyramid_ ? pyramid_->sourceFrames() : 0);
}

void WaveformView::setBeatGrid(std::vector<double> beatSeconds, SampleIndex startSample,
                               bool doubtful) {
    beatSeconds_ = std::move(beatSeconds);
    beatStart_ = startSample;
    beatsDoubtful_ = doubtful;
    update();
}

void WaveformView::setPitchContour(std::vector<analysis::PitchPoint> contour,
                                   SampleIndex startSample, double lowHz, double highHz) {
    contour_ = std::move(contour);
    contourStart_ = startSample;
    contourLowHz_ = lowHz;
    contourHighHz_ = highHz;
    update();
}

void WaveformView::setAnalysedEnd(SampleIndex end) {
    analysedEnd_ = end;
    update();
}

void WaveformView::clearOverlays() {
    beatSeconds_.clear();
    contour_.clear();
    analysedEnd_ = -1;
    update();
}

PitchAxis WaveformView::pitchAxis(const QRect& plot) const noexcept {
    // The whole height of the plot, with no inset. An inset would be a second
    // number a reader of the picture has to know about, and the axis is already
    // bounded by the tracker's range at both ends.
    return PitchAxis{contourLowHz_, contourHighHz_, plot.top(), plot.height()};
}

void WaveformView::hover(const QPoint& position) {
    const QRect plot = plotRect();
    const int x = position.x() - kGutterWidth;

    if (!pyramid_ || viewLength_ <= 0 || plot.width() <= 0 || x < 0 ||
        x >= static_cast<int>(columns_.size()) || rate_.hz() <= 0.0) {
        emit cursorMoved(-1.0, -144.0);
        return;
    }

    const io::PeakFrame& column = columns_[static_cast<std::size_t>(x)];
    const double peak = std::max(std::abs(static_cast<double>(column.maximum)),
                                 std::abs(static_cast<double>(column.minimum)));
    emit cursorMoved(static_cast<double>(sampleAtX(position.x())) / rate_.hz(),
                     amplitudeToDecibels(peak));
}

void WaveformView::leaveEvent(QEvent*) {
    emit cursorMoved(-1.0, -144.0);
}

void WaveformView::paintGutter(QPainter& painter) {
    painter.fillRect(QRect{0, 0, kGutterWidth, height()}, kGutterBackground);

    const int midY = height() / 2;
    const auto halfHeight = static_cast<double>(midY - 2);

    QFont small = painter.font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.5));
    painter.setFont(small);

    // Label the top half only. The axis is symmetric, so repeating -6 and -12
    // below the centre line doubles the ink and tells the reader nothing; the
    // tick marks still run both ways so the scale stays obvious.
    //
    // kLevelLabelsDb descends, so y increases as the loop runs and each label
    // is checked against the one above it.
    int lastLabelY = -100;
    for (const double decibels : kLevelLabelsDb) {
        const double amplitude = std::pow(10.0, decibels / 20.0);
        const int offset = static_cast<int>(amplitude * halfHeight);
        const int y = midY - offset;

        painter.setPen(kGutterLine);
        painter.drawLine(kGutterWidth - 5, y, kGutterWidth, y);
        painter.drawLine(kGutterWidth - 5, midY + offset, kGutterWidth, midY + offset);

        if (y - lastLabelY < 13) {
            continue; // Would collide with the label above it.
        }
        lastLabelY = y;
        painter.setPen(kGutterLabel);
        painter.drawText(QRect{0, std::clamp(y - 7, 0, height() - 14), kGutterWidth - 8, 14},
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(static_cast<int>(decibels)));
    }

    painter.setPen(kGutterLine);
    painter.drawLine(kGutterWidth, 0, kGutterWidth, height());
}

void WaveformView::paintPlot(QPainter& painter, const QRect& plot) {
    painter.fillRect(rect(), kBackground);

    const int midY = height() / 2;
    painter.setPen(kCentreLine);
    painter.drawLine(plot.left(), midY, plot.right() + 1, midY);

    if (!pyramid_ || pyramid_->isEmpty() || viewLength_ <= 0) {
        columns_.clear();
        painter.setPen(kText);
        painter.drawText(rect(), Qt::AlignCenter, tr("No audio loaded"));
        return;
    }

    columns_.resize(static_cast<std::size_t>(plot.width()));
    pyramid_->query(0, viewStart_, viewStart_ + viewLength_, columns_.data(), plot.width());

    const auto halfHeight = static_cast<float>(midY - 2);

    // Envelope first, then the RMS body over it: the visible gap between the two
    // is crest factor, so heavily limited material reads as a solid block.
    painter.setPen(kEnvelope);
    for (int x = 0; x < plot.width(); ++x) {
        const io::PeakFrame& column = columns_[static_cast<std::size_t>(x)];
        const int top =
            midY - static_cast<int>(std::clamp(column.maximum, -1.0f, 1.0f) * halfHeight);
        const int bottom =
            midY - static_cast<int>(std::clamp(column.minimum, -1.0f, 1.0f) * halfHeight);
        painter.drawLine(plot.left() + x, top, plot.left() + x, bottom);
    }

    painter.setPen(kBody);
    for (int x = 0; x < plot.width(); ++x) {
        const float rms = std::clamp(columns_[static_cast<std::size_t>(x)].rms, 0.0f, 1.0f);
        const int extent = static_cast<int>(rms * halfHeight);
        painter.drawLine(plot.left() + x, midY - extent, plot.left() + x, midY + extent);
    }

    // Over the audio rather than under it. A grid hidden behind a loud passage
    // is a grid that cannot be checked against exactly the transients it most
    // needs to be checked against.
    paintBeats(painter, plot);
    paintContour(painter, plot);
    paintAnalysedEnd(painter, plot);
}

void WaveformView::paintBeats(QPainter& painter, const QRect& plot) {
    if (beatSeconds_.empty() || !rate_.isValid()) {
        return;
    }
    QPen pen{kBeatLine, 1.0};
    if (beatsDoubtful_) {
        pen.setStyle(Qt::DashLine);
    }
    painter.setPen(pen);
    // Every beat drawn alike: there are no bar lines to mark, because
    // TempoTrack.h finds beats and not metre, and a heavier line every four
    // would be a downbeat nothing measured.
    for (const int x : beatColumns(beatSeconds_, beatStart_, rate_, timePlot())) {
        painter.drawLine(x, plot.top(), x, plot.bottom());
    }
}

void WaveformView::paintContour(QPainter& painter, const QRect& plot) {
    if (contour_.empty() || !rate_.isValid() || plot.height() <= 1) {
        return;
    }
    const PitchAxis axis = pitchAxis(plot);

    QFont small = painter.font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.5));
    painter.setFont(small);
    for (const AxisTick& tick : pitchTicks(axis)) {
        const int y = axis.yAtHz(tick.value);
        painter.setPen(kContourScale);
        painter.drawLine(plot.right() - 28, y, plot.right(), y);
        painter.drawText(QRect{plot.right() - 92, std::clamp(y - 7, 0, height() - 14), 60, 14},
                         Qt::AlignRight | Qt::AlignVCenter, QString::fromStdString(tick.label));
    }

    // Deliberately not antialiased. The contour's height is the reading, so a
    // sustained note has to land on exactly the row its frequency maps to
    // rather than being spread over the two either side of it.
    painter.setPen(QPen{kContour, 1.0});
    for (const std::vector<ContourPoint>& run :
         pitchRuns(contour_, contourStart_, rate_, timePlot(), axis)) {
        if (run.size() == 1) {
            // One voiced frame between two unvoiced ones. Drawn as a dot,
            // because it is the shortest thing the tracker can report and
            // dropping it would hide exactly the material a contour is least
            // sure about.
            painter.drawPoint(run.front().x, run.front().y);
            continue;
        }
        for (std::size_t i = 1; i < run.size(); ++i) {
            painter.drawLine(run[i - 1].x, run[i - 1].y, run[i].x, run[i].y);
        }
    }
}

void WaveformView::paintAnalysedEnd(QPainter& painter, const QRect& plot) {
    if (analysedEnd_ < 0 || (beatSeconds_.empty() && contour_.empty())) {
        return;
    }
    const int x = timePlot().xAtSample(analysedEnd_);
    if (!timePlot().holds(x)) {
        return;
    }
    QPen pen{kBoundary, 1.0};
    pen.setStyle(Qt::DotLine);
    painter.setPen(pen);
    painter.drawLine(x, plot.top(), x, plot.bottom());
    painter.drawText(QRect{x + 4, plot.top() + 2, 150, 14}, Qt::AlignLeft | Qt::AlignVCenter,
                     tr("analysis ends"));
}

} // namespace sa::ui
