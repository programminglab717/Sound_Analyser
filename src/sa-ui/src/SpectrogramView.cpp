#include <sa/ui/SpectrogramView.h>

#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sa::ui {

SpectrogramView::SpectrogramView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(140);
    setAutoFillBackground(false);
}

void SpectrogramView::setPyramid(std::shared_ptr<const spectral::SpectrogramPyramid> pyramid,
                                 SampleRate rate) {
    pyramid_ = std::move(pyramid);
    rate_ = rate;
    viewStart_ = 0;
    viewLength_ = pyramid_ ? pyramid_->sourceFrames() : 0;
    imageDirty_ = true;
    update();
}

void SpectrogramView::setViewRange(SampleIndex start, SampleCount length) {
    if (start == viewStart_ && length == viewLength_) {
        return;
    }
    viewStart_ = start;
    viewLength_ = length;
    imageDirty_ = true;
    update();
}

void SpectrogramView::setColourmap(Colourmap map) {
    if (map == colourmap_) {
        return;
    }
    colourmap_ = map;
    // Only the mapping changed, not the data -- but the CPU path bakes colour
    // into the image, so it has to be rebuilt. A shader would not need this.
    imageDirty_ = true;
    update();
}

void SpectrogramView::setFloorDecibels(float decibels) {
    floorDb_ = std::clamp(decibels, -160.0f, -6.0f);
    imageDirty_ = true;
    update();
}

void SpectrogramView::wheelEvent(QWheelEvent* event) {
    if (!pyramid_ || viewLength_ <= 0 || width() <= 0) {
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        return;
    }
    const double anchor = event->position().x() / width();
    const auto anchorSample =
        viewStart_ + static_cast<SampleIndex>(static_cast<double>(viewLength_) * anchor);

    const SampleCount total = pyramid_->sourceFrames();
    auto length = static_cast<SampleCount>(static_cast<double>(viewLength_) * std::pow(0.8, steps));
    length = std::clamp<SampleCount>(length, std::max<SampleCount>(1, width()), total);
    auto start = anchorSample - static_cast<SampleIndex>(static_cast<double>(length) * anchor);
    start = std::clamp<SampleIndex>(start, 0, std::max<SampleIndex>(0, total - length));

    setViewRange(start, length);
    emit viewRangeChanged(start, length);
    event->accept();
}

void SpectrogramView::rebuildImage() {
    const int w = width();
    const int h = height();
    if (!pyramid_ || pyramid_->isEmpty() || w <= 0 || h <= 0 || viewLength_ <= 0) {
        image_ = QImage{};
        imageDirty_ = false;
        return;
    }

    tile_.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    pyramid_->render(viewStart_, viewStart_ + viewLength_, 0, h, w, tile_.data());

    image_ = QImage{w, h, QImage::Format_RGB32};
    const auto& table = colourmapTable(colourmap_);

    // The pyramid stores magnitude quantised across its own configured dB
    // range. The display floor is applied here rather than at analysis time, so
    // dragging the contrast does not require re-running any STFT.
    const float pyramidFloor = pyramid_->config().minimumDecibels;
    const float pyramidCeiling = pyramid_->config().maximumDecibels;
    const float pyramidSpan = pyramidCeiling - pyramidFloor;
    const float displaySpan = pyramidCeiling - floorDb_;

    for (int row = 0; row < h; ++row) {
        // Row 0 is the top of the widget and the highest frequency, which is
        // how every spectrogram in the field is drawn.
        auto* scanline = reinterpret_cast<QRgb*>(image_.scanLine(row));
        const int sourceRow = h - 1 - row;

        for (int x = 0; x < w; ++x) {
            const std::uint8_t stored =
                tile_[static_cast<std::size_t>(sourceRow) * static_cast<std::size_t>(w) +
                      static_cast<std::size_t>(x)];
            const float decibels =
                pyramidFloor + pyramidSpan * static_cast<float>(stored) / 255.0f;
            const float normalised =
                displaySpan > 0.0f ? (decibels - floorDb_) / displaySpan : 0.0f;
            const auto index =
                static_cast<std::size_t>(std::clamp(normalised, 0.0f, 1.0f) * 255.0f + 0.5f);
            scanline[x] = table[index];
        }
    }
    imageDirty_ = false;
}

void SpectrogramView::paintEvent(QPaintEvent*) {
    QPainter painter(this);

    if (imageDirty_ || image_.width() != width() || image_.height() != height()) {
        rebuildImage();
    }

    if (image_.isNull()) {
        painter.fillRect(rect(), QColor{0x0d, 0x0d, 0x12});
        painter.setPen(QColor{0x8a, 0x8f, 0xa0});
        painter.drawText(rect(), Qt::AlignCenter, tr("No audio loaded"));
        return;
    }
    painter.drawImage(0, 0, image_);
}

} // namespace sa::ui
