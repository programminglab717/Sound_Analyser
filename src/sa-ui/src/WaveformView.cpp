#include <sa/ui/WaveformView.h>

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>

namespace sa::ui {

namespace {

constexpr QColor kBackground{0x14, 0x15, 0x1a};
constexpr QColor kCentreLine{0x2c, 0x2e, 0x38};
constexpr QColor kEnvelope{0x4d, 0x9d, 0xe0};
constexpr QColor kBody{0x9b, 0xd1, 0xf5};
constexpr QColor kText{0x8a, 0x8f, 0xa0};

} // namespace

WaveformView::WaveformView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(90);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setAutoFillBackground(false);
}

void WaveformView::setPyramid(std::shared_ptr<const io::PeakPyramid> pyramid, SampleRate rate) {
    pyramid_ = std::move(pyramid);
    rate_ = rate;
    showAll();
}

void WaveformView::showAll() {
    viewStart_ = 0;
    viewLength_ = pyramid_ ? pyramid_->sourceFrames() : 0;
    update();
    emit viewRangeChanged(viewStart_, viewLength_);
}

void WaveformView::setViewRange(SampleIndex start, SampleCount length) {
    viewStart_ = start;
    viewLength_ = length;
    clampView();
    update();
}

void WaveformView::clampView() {
    if (!pyramid_) {
        return;
    }
    const SampleCount total = pyramid_->sourceFrames();
    // One sample per pixel is as far in as the data goes; past that the display
    // would be inventing detail it does not have.
    viewLength_ = std::clamp<SampleCount>(viewLength_, std::max<SampleCount>(1, width()), total);
    viewStart_ = std::clamp<SampleIndex>(viewStart_, 0, std::max<SampleIndex>(0, total - viewLength_));
}

void WaveformView::zoom(double factor, double anchorFraction) {
    if (!pyramid_ || viewLength_ <= 0) {
        return;
    }
    // Keep the sample under the cursor stationary, so zooming feels like moving
    // a lens rather than jumping somewhere new.
    const auto anchorSample =
        viewStart_ + static_cast<SampleIndex>(static_cast<double>(viewLength_) * anchorFraction);
    const auto newLength = static_cast<SampleCount>(static_cast<double>(viewLength_) * factor);

    viewLength_ = newLength;
    clampView();
    viewStart_ = anchorSample -
                 static_cast<SampleIndex>(static_cast<double>(viewLength_) * anchorFraction);
    clampView();

    update();
    emit viewRangeChanged(viewStart_, viewLength_);
}

void WaveformView::scrollBySamples(SampleIndex delta) {
    viewStart_ += delta;
    clampView();
    update();
    emit viewRangeChanged(viewStart_, viewLength_);
}

void WaveformView::wheelEvent(QWheelEvent* event) {
    if (!pyramid_) {
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        return;
    }
    const double anchor = width() > 0 ? event->position().x() / width() : 0.5;
    zoom(std::pow(0.8, steps), anchor);
    event->accept();
}

void WaveformView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && pyramid_) {
        dragging_ = true;
        dragAnchorX_ = static_cast<int>(event->position().x());
        dragAnchorStart_ = viewStart_;
        setCursor(Qt::ClosedHandCursor);
    }
}

void WaveformView::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_ || width() <= 0) {
        return;
    }
    const int dx = static_cast<int>(event->position().x()) - dragAnchorX_;
    const auto perPixel = static_cast<double>(viewLength_) / width();
    viewStart_ = dragAnchorStart_ - static_cast<SampleIndex>(dx * perPixel);
    clampView();
    update();
    emit viewRangeChanged(viewStart_, viewLength_);
}

void WaveformView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
    }
}

void WaveformView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    const int w = width();
    const int h = height();
    const int midY = h / 2;

    painter.setPen(kCentreLine);
    painter.drawLine(0, midY, w, midY);

    if (!pyramid_ || pyramid_->isEmpty() || viewLength_ <= 0 || w <= 0) {
        painter.setPen(kText);
        painter.drawText(rect(), Qt::AlignCenter, tr("No audio loaded"));
        return;
    }

    columns_.resize(static_cast<std::size_t>(w));
    pyramid_->query(0, viewStart_, viewStart_ + viewLength_, columns_.data(), w);

    const auto halfHeight = static_cast<float>(midY - 2);

    // Envelope first, then the RMS body over it: the visible gap between the two
    // is crest factor, so heavily limited material reads as a solid block.
    painter.setPen(kEnvelope);
    for (int x = 0; x < w; ++x) {
        const io::PeakFrame& column = columns_[static_cast<std::size_t>(x)];
        const int top = midY - static_cast<int>(std::clamp(column.maximum, -1.0f, 1.0f) * halfHeight);
        const int bottom =
            midY - static_cast<int>(std::clamp(column.minimum, -1.0f, 1.0f) * halfHeight);
        painter.drawLine(x, top, x, bottom);
    }

    painter.setPen(kBody);
    for (int x = 0; x < w; ++x) {
        const float rms = std::clamp(columns_[static_cast<std::size_t>(x)].rms, 0.0f, 1.0f);
        const int extent = static_cast<int>(rms * halfHeight);
        painter.drawLine(x, midY - extent, x, midY + extent);
    }
}

} // namespace sa::ui
