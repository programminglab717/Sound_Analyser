#include <sa/ui/SpectrogramView.h>

#include <QPainter>
#include <algorithm>
#include <cmath>

namespace sa::ui {

namespace {

constexpr QColor kGutterBackground{0x16, 0x17, 0x1d};
constexpr QColor kGutterLine{0x4a, 0x4e, 0x5e};
constexpr QColor kGutterLabel{0xa8, 0xad, 0xbd};
constexpr QColor kGridLine{0xff, 0xff, 0xff};
constexpr QColor kEmpty{0x0d, 0x0d, 0x12};
constexpr QColor kText{0x8a, 0x8f, 0xa0};

} // namespace

SpectrogramView::SpectrogramView(QWidget* parent) : TimeAxisView(parent) {
    setMinimumHeight(140);
}

void SpectrogramView::setPyramid(std::shared_ptr<const spectral::SpectrogramPyramid> pyramid,
                                 SampleRate rate, SampleCount totalFrames) {
    pyramid_ = std::move(pyramid);
    imageDirty_ = true;
    lowHz_ = 0.0;
    highHz_ = rate.hz() * 0.5;
    // The document's length, not the pyramid's: a file too long to analyse
    // still has a timeline, and the two views must agree on how long it is or
    // they will not scroll together.
    setTimeline(rate, totalFrames);
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

void SpectrogramView::setFrequencyScale(FrequencyScale scale) {
    if (scale == scale_) {
        return;
    }
    scale_ = scale;
    imageDirty_ = true;
    update();
}

void SpectrogramView::setFloorDecibels(float decibels) {
    floorDb_ = std::clamp(decibels, -160.0f, -6.0f);
    imageDirty_ = true;
    update();
}

void SpectrogramView::setFrequencySelection(double lowHz, double highHz) {
    if (lowHz > highHz) {
        std::swap(lowHz, highHz);
    }
    if (lowHz == lowHz_ && highHz == highHz_) {
        return;
    }
    lowHz_ = lowHz;
    highHz_ = highHz;
    update();
}

void SpectrogramView::verticalSelectionChanged(double lowFraction, double highFraction) {
    const double nyquist = rate_.hz() * 0.5;
    setFrequencySelection(frequencyAtFraction(scale_, lowFraction, nyquist),
                          frequencyAtFraction(scale_, highFraction, nyquist));
    emit frequencySelectionChanged(lowHz_, highHz_);
}

QRect SpectrogramView::selectionRect(const QRect& plot, int left, int right) const {
    const double nyquist = rate_.hz() * 0.5;
    if (nyquist <= 0.0 || highHz_ <= lowHz_) {
        return QRect{left, 0, right - left, height()};
    }
    const auto yFor = [&](double hz) {
        const double fraction = fractionAtFrequency(scale_, hz, nyquist);
        return height() - 1 - static_cast<int>(fraction * (height() - 1));
    };
    const int top = std::clamp(yFor(highHz_), 0, height() - 1);
    const int bottom = std::clamp(yFor(lowHz_), 0, height() - 1);
    return QRect{left, top, right - left, std::max(1, bottom - top + 1)};
}

void SpectrogramView::viewInvalidated() {
    imageDirty_ = true;
}

void SpectrogramView::hover(const QPoint& position) {
    const QRect plot = plotRect();
    const int x = position.x() - kGutterWidth;
    const int y = position.y();

    if (!pyramid_ || viewLength_ <= 0 || plot.width() <= 0 || x < 0 || x >= plot.width() || y < 0 ||
        y >= height()) {
        emit cursorMoved(0.0, -1.0, 0.0);
        return;
    }

    const double seconds =
        rate_.hz() > 0.0 ? static_cast<double>(sampleAtX(position.x())) / rate_.hz() : 0.0;
    const double fraction = 1.0 - static_cast<double>(y) / height();
    const double nyquist = rate_.hz() * 0.5;
    const double hz = frequencyAtFraction(scale_, fraction, nyquist);

    // Read the level straight off the rendered tile rather than re-querying the
    // pyramid: the tile is what the user is looking at, so the number and the
    // colour under the pointer can never disagree.
    double decibels = pyramid_->config().minimumDecibels;
    const int sourceRow = height() - 1 - y;
    const auto index =
        static_cast<std::size_t>(sourceRow) * static_cast<std::size_t>(plot.width()) +
        static_cast<std::size_t>(x);
    if (index < tile_.size()) {
        decibels = pyramid_->toDecibels(tile_[index]);
    }
    emit cursorMoved(seconds, hz, decibels);
}

void SpectrogramView::leaveEvent(QEvent*) {
    emit cursorMoved(0.0, -1.0, 0.0);
}

void SpectrogramView::rebuildImage() {
    const int w = plotWidth();
    const int h = height();
    if (!pyramid_ || pyramid_->isEmpty() || w <= 0 || h <= 0 || viewLength_ <= 0) {
        image_ = QImage{};
        tile_.clear();
        imageDirty_ = false;
        return;
    }

    // One edge per row boundary, bottom row first. The pyramid combines
    // whatever bins fall inside each row by maximum, so a narrow peak survives
    // the squeeze at the top of a log axis.
    const auto bins = static_cast<double>(pyramid_->binCount());
    const double nyquist = rate_.hz() * 0.5;
    rowBinEdges_.resize(static_cast<std::size_t>(h) + 1);
    for (int row = 0; row <= h; ++row) {
        const double fraction = static_cast<double>(row) / h;
        const double hz = frequencyAtFraction(scale_, fraction, nyquist);
        const double bin = nyquist > 0.0 ? (hz / nyquist) * (bins - 1.0) : 0.0;
        rowBinEdges_[static_cast<std::size_t>(row)] = static_cast<float>(bin);
    }

    tile_.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    pyramid_->render(viewStart_, viewStart_ + viewLength_, rowBinEdges_.data(), h, w, tile_.data());

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
            const float decibels = pyramidFloor + pyramidSpan * static_cast<float>(stored) / 255.0f;
            const float normalised =
                displaySpan > 0.0f ? (decibels - floorDb_) / displaySpan : 0.0f;
            const auto index =
                static_cast<std::size_t>(std::clamp(normalised, 0.0f, 1.0f) * 255.0f + 0.5f);
            scanline[x] = table[index];
        }
    }
    imageDirty_ = false;
}

void SpectrogramView::paintGutter(QPainter& painter) {
    painter.fillRect(QRect{0, 0, kGutterWidth, height()}, kGutterBackground);

    const double nyquist = rate_.hz() * 0.5;
    if (nyquist <= 0.0) {
        return;
    }

    QFont small = painter.font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.5));
    painter.setFont(small);

    for (const AxisTick& tick : frequencyTicks(scale_, nyquist, height())) {
        const int y = height() - 1 - static_cast<int>(tick.fraction * (height() - 1));

        painter.setPen(kGutterLine);
        painter.drawLine(kGutterWidth - (tick.major ? 6 : 3), y, kGutterWidth, y);

        if (tick.major) {
            // Nudge the first and last labels inwards rather than letting them
            // hang off the widget and get clipped in half.
            const int labelY = std::clamp(y - 7, 0, height() - 14);
            painter.setPen(kGutterLabel);
            painter.drawText(QRect{0, labelY, kGutterWidth - 8, 14},
                             Qt::AlignRight | Qt::AlignVCenter, QString::fromStdString(tick.label));

            // A faint line across the plot: without it the eye cannot carry a
            // frequency from the gutter to a partial halfway across a wide view.
            if (plotWidth() > 0) {
                QColor grid = kGridLine;
                grid.setAlpha(18);
                painter.setPen(grid);
                painter.drawLine(kGutterWidth, y, width(), y);
            }
        }
    }

    painter.setPen(kGutterLine);
    painter.drawLine(kGutterWidth, 0, kGutterWidth, height());
}

void SpectrogramView::paintPlot(QPainter& painter, const QRect& plot) {
    if (imageDirty_ || image_.width() != plot.width() || image_.height() != height()) {
        rebuildImage();
    }

    if (image_.isNull()) {
        painter.fillRect(rect(), kEmpty);
        painter.setPen(kText);
        painter.drawText(rect(), Qt::AlignCenter,
                         pyramid_ ? tr("No audio loaded") : tr("Spectrogram not available"));
        return;
    }
    painter.drawImage(plot.left(), 0, image_);
}

} // namespace sa::ui
