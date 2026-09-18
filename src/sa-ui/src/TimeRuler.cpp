#include <sa/ui/TimeRuler.h>

#include <QPainter>

namespace sa::ui {

namespace {

constexpr QColor kBackground{0x16, 0x17, 0x1d};
constexpr QColor kTick{0x4a, 0x4e, 0x5e};
constexpr QColor kLabel{0xa8, 0xad, 0xbd};
constexpr QColor kPlayhead{0xff, 0x9f, 0x43};
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

void TimeRuler::setPlayhead(SampleIndex position) {
    playhead_ = position;
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
