#include <sa/ui/TimeRuler.h>

#include <QPainter>
#include <algorithm>
#include <utility>

namespace sa::ui {

namespace {

constexpr QColor kBackground{0x16, 0x17, 0x1d};
constexpr QColor kTick{0x4a, 0x4e, 0x5e};
constexpr QColor kLabel{0xa8, 0xad, 0xbd};
constexpr QColor kPlayhead{0xff, 0x9f, 0x43};
constexpr QColor kSelectionBand{0x4d, 0x9d, 0xe0, 0x4d};
constexpr QColor kMarker{0x8f, 0xd6, 0x94};
constexpr QColor kMarkerSpan{0x8f, 0xd6, 0x94, 0x38};
constexpr int kTickLength = 6;

} // namespace

TimeRuler::TimeRuler(QWidget* parent) : QWidget(parent) {
    setFixedHeight(22);
    setAutoFillBackground(false);
}

void TimeRuler::setSampleRate(SampleRate rate) {
    rate_ = rate;
    update();
}

void TimeRuler::setViewRange(SampleIndex start, SampleCount length) {
    if (start == viewStart_ && length == viewLength_) {
        return;
    }
    viewStart_ = start;
    viewLength_ = length;
    update();
}

void TimeRuler::setSelection(TimeSelection selection) {
    if (selection == selection_) {
        return;
    }
    selection_ = selection;
    update();
}

void TimeRuler::setPlayhead(SampleIndex position) {
    playhead_ = position;
    update();
}

void TimeRuler::setMarkers(std::vector<Mark> markers) {
    markers_ = std::move(markers);
    update();
}

void TimeRuler::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    const int plotWidth = width() - kGutterWidth;
    if (viewLength_ <= 0 || plotWidth <= 0 || rate_.hz() <= 0.0) {
        return;
    }

    const double startSeconds = static_cast<double>(viewStart_) / rate_.hz();
    const double spanSeconds = static_cast<double>(viewLength_) / rate_.hz();

    // The selected span, so the ruler answers "how much is that" without the
    // user having to read two numbers off it and subtract.
    if (!selection_.isEmpty()) {
        const auto toX = [&](SampleIndex sample) {
            const double fraction =
                static_cast<double>(sample - viewStart_) / static_cast<double>(viewLength_);
            return kGutterWidth + static_cast<int>(fraction * plotWidth);
        };
        const int left = std::max(kGutterWidth, toX(selection_.start));
        const int right = std::min(width(), toX(selection_.end));
        if (right > left) {
            painter.fillRect(QRect{left, 0, right - left, height()}, kSelectionBand);
        }
    }

    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.5));
    painter.setFont(small);

    for (const AxisTick& tick : timeTicks(startSeconds, spanSeconds, plotWidth)) {
        const int x = kGutterWidth + static_cast<int>(tick.fraction * plotWidth);
        painter.setPen(kTick);
        painter.drawLine(x, height() - kTickLength, x, height());
        painter.setPen(kLabel);
        painter.drawText(x + 3, height() - kTickLength - 2, QString::fromStdString(tick.label));
    }

    // Markers over the ticks, because a marker is a thing the user put there
    // and the grid is furniture.
    if (!markers_.empty()) {
        const auto toX = [&](SampleIndex sample) {
            const double fraction =
                static_cast<double>(sample - viewStart_) / static_cast<double>(viewLength_);
            return kGutterWidth + static_cast<int>(fraction * plotWidth);
        };
        for (const Mark& mark : markers_) {
            const SampleIndex end = mark.position + std::max<SampleCount>(mark.length, 0);
            if (end < viewStart_ || mark.position >= viewStart_ + viewLength_) {
                continue;
            }
            const int x = toX(mark.position);
            if (mark.length > 0) {
                const int left = std::max(kGutterWidth, x);
                const int right = std::min(width(), toX(end));
                if (right > left) {
                    painter.fillRect(QRect{left, 0, right - left, height() - 1}, kMarkerSpan);
                }
            }
            if (x >= kGutterWidth && x < width()) {
                painter.setPen(kMarker);
                painter.drawLine(x, 0, x, height() - 1);
                // A little flag, so a marker is findable at a glance rather
                // than being one more vertical line among the grid.
                painter.fillRect(QRect{x + 1, 1, 5, 5}, kMarker);
                if (!mark.label.isEmpty()) {
                    painter.drawText(x + 8, height() - kTickLength - 2, mark.label);
                }
            }
        }
    }

    painter.setPen(kTick);
    painter.drawLine(0, height() - 1, width(), height() - 1);

    if (playhead_ >= viewStart_ && playhead_ < viewStart_ + viewLength_) {
        const double fraction =
            static_cast<double>(playhead_ - viewStart_) / static_cast<double>(viewLength_);
        const int x = kGutterWidth + static_cast<int>(fraction * plotWidth);
        painter.setPen(kPlayhead);
        painter.drawLine(x, 0, x, height());
    }
}

} // namespace sa::ui
