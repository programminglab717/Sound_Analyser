#include <sa/ui/TimeAxisView.h>

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace sa::ui {

namespace {

constexpr QColor kSelectionFill{0x4d, 0x9d, 0xe0, 0x38};
constexpr QColor kSelectionEdge{0x9b, 0xd1, 0xf5, 0xcc};
constexpr QColor kPlayhead{0xff, 0x9f, 0x43};

} // namespace

TimeAxisView::TimeAxisView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
}

int TimeAxisView::plotWidth() const noexcept {
    return std::max(0, width() - kGutterWidth);
}

QRect TimeAxisView::plotRect() const noexcept {
    return QRect{kGutterWidth, 0, plotWidth(), height()};
}

SampleIndex TimeAxisView::sampleAtX(int x) const noexcept {
    const int plot = plotWidth();
    if (plot <= 0 || viewLength_ <= 0) {
        return viewStart_;
    }
    const double fraction = std::clamp(static_cast<double>(x - kGutterWidth) / plot, 0.0, 1.0);
    return viewStart_ + static_cast<SampleIndex>(static_cast<double>(viewLength_) * fraction);
}

int TimeAxisView::xForSample(SampleIndex sample) const noexcept {
    const int plot = plotWidth();
    if (plot <= 0 || viewLength_ <= 0) {
        return kGutterWidth;
    }
    const double fraction =
        static_cast<double>(sample - viewStart_) / static_cast<double>(viewLength_);
    return kGutterWidth + static_cast<int>(std::lround(fraction * plot));
}

void TimeAxisView::setTimeline(SampleRate rate, SampleCount totalFrames) {
    rate_ = rate;
    totalFrames_ = totalFrames;
    selection_ = {};
    playhead_ = -1;
    showAll();
}

void TimeAxisView::clampView() {
    if (totalFrames_ <= 0) {
        viewStart_ = 0;
        viewLength_ = 0;
        return;
    }
    // One sample per pixel is as far in as the data goes; past that the display
    // would be inventing detail it does not have.
    viewLength_ =
        std::clamp<SampleCount>(viewLength_, std::max<SampleCount>(1, plotWidth()), totalFrames_);
    viewStart_ = std::clamp<SampleIndex>(viewStart_, 0,
                                         std::max<SampleIndex>(0, totalFrames_ - viewLength_));
}

void TimeAxisView::setViewRange(SampleIndex start, SampleCount length) {
    const SampleIndex previousStart = viewStart_;
    const SampleCount previousLength = viewLength_;
    viewStart_ = start;
    viewLength_ = length;
    clampView();
    if (viewStart_ != previousStart || viewLength_ != previousLength) {
        viewInvalidated();
        update();
    }
}

void TimeAxisView::showAll() {
    viewStart_ = 0;
    viewLength_ = totalFrames_;
    clampView();
    viewInvalidated();
    update();
    emit viewRangeChanged(viewStart_, viewLength_);
}

void TimeAxisView::zoomToSelection() {
    if (selection_.isEmpty()) {
        return;
    }
    viewStart_ = selection_.start;
    viewLength_ = selection_.length();
    clampView();
    viewInvalidated();
    update();
    emit viewRangeChanged(viewStart_, viewLength_);
}

void TimeAxisView::zoom(double factor, double anchorFraction) {
    if (viewLength_ <= 0) {
        return;
    }
    // Keep the sample under the cursor stationary, so zooming feels like moving
    // a lens rather than jumping somewhere new.
    const auto anchorSample =
        viewStart_ + static_cast<SampleIndex>(static_cast<double>(viewLength_) * anchorFraction);
    viewLength_ = static_cast<SampleCount>(static_cast<double>(viewLength_) * factor);
    clampView();
    viewStart_ =
        anchorSample - static_cast<SampleIndex>(static_cast<double>(viewLength_) * anchorFraction);
    clampView();

    viewInvalidated();
    update();
    emit viewRangeChanged(viewStart_, viewLength_);
}

void TimeAxisView::scrollBySamples(SampleIndex delta) {
    viewStart_ += delta;
    clampView();
    viewInvalidated();
    update();
    emit viewRangeChanged(viewStart_, viewLength_);
}

void TimeAxisView::setSelection(TimeSelection selection) {
    if (selection.end < selection.start) {
        std::swap(selection.start, selection.end);
    }
    selection.start = std::clamp<SampleIndex>(selection.start, 0, totalFrames_);
    selection.end = std::clamp<SampleIndex>(selection.end, 0, totalFrames_);
    if (selection == selection_) {
        return;
    }
    selection_ = selection;
    update();
}

void TimeAxisView::setPlayhead(SampleIndex position) {
    if (position == playhead_) {
        return;
    }
    playhead_ = position;
    update();
}

void TimeAxisView::emitSelection() {
    emit selectionChanged(selection_.start, selection_.end);
}

void TimeAxisView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    clampView();
    viewInvalidated();
}

void TimeAxisView::wheelEvent(QWheelEvent* event) {
    const int plot = plotWidth();
    if (plot <= 0 || viewLength_ <= 0) {
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        return;
    }
    // Shift turns the wheel into a scroll, which is the convention everywhere
    // and is what a trackpad user reaches for first.
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        scrollBySamples(static_cast<SampleIndex>(-steps * static_cast<double>(viewLength_) * 0.1));
    } else {
        const double anchor = std::clamp((event->position().x() - kGutterWidth) / plot, 0.0, 1.0);
        zoom(std::pow(0.8, steps), anchor);
    }
    event->accept();
}

void TimeAxisView::mousePressEvent(QMouseEvent* event) {
    if (totalFrames_ <= 0) {
        return;
    }
    const int x = static_cast<int>(event->position().x());

    // Middle button, or Alt with the left, pans. The left button on its own
    // selects, because selecting is what the user does hundreds of times an
    // hour and panning is what the wheel is for.
    const bool pan =
        event->button() == Qt::MiddleButton ||
        (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::AltModifier));

    if (pan) {
        drag_ = Drag::Panning;
        dragAnchorX_ = x;
        dragAnchorStart_ = viewStart_;
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (event->button() != Qt::LeftButton) {
        return;
    }

    drag_ = Drag::Selecting;
    // Shift extends the existing selection from whichever edge is further away,
    // rather than starting a new one.
    if (event->modifiers().testFlag(Qt::ShiftModifier) && !selection_.isEmpty()) {
        const SampleIndex here = sampleAtX(x);
        selectionAnchor_ = std::abs(here - selection_.start) > std::abs(here - selection_.end)
                               ? selection_.start
                               : selection_.end;
        setSelection(TimeSelection{selectionAnchor_, here});
    } else {
        selectionAnchor_ = sampleAtX(x);
        setSelection(TimeSelection{selectionAnchor_, selectionAnchor_});
    }
    emitSelection();
}

void TimeAxisView::mouseMoveEvent(QMouseEvent* event) {
    const int x = static_cast<int>(event->position().x());

    switch (drag_) {
    case Drag::Panning: {
        const int plot = plotWidth();
        if (plot <= 0) {
            return;
        }
        const auto perPixel = static_cast<double>(viewLength_) / plot;
        viewStart_ = dragAnchorStart_ - static_cast<SampleIndex>((x - dragAnchorX_) * perPixel);
        clampView();
        viewInvalidated();
        update();
        emit viewRangeChanged(viewStart_, viewLength_);
        return;
    }
    case Drag::Selecting:
        setSelection(TimeSelection{selectionAnchor_, sampleAtX(x)});
        emitSelection();
        return;
    case Drag::None:
        hover(event->position().toPoint());
        return;
    }
}

void TimeAxisView::mouseReleaseEvent(QMouseEvent*) {
    if (drag_ == Drag::Panning) {
        setCursor(Qt::ArrowCursor);
    }
    drag_ = Drag::None;
}

void TimeAxisView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || totalFrames_ <= 0) {
        return;
    }
    setSelection(TimeSelection{0, totalFrames_});
    emitSelection();
}

void TimeAxisView::hover(const QPoint&) {}

void TimeAxisView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QRect plot = plotRect();

    if (plot.width() > 0) {
        paintPlot(painter, plot);

        if (!selection_.isEmpty()) {
            const int left = std::max(plot.left(), xForSample(selection_.start));
            const int right = std::min(plot.right() + 1, xForSample(selection_.end));
            if (right > left) {
                painter.fillRect(QRect{left, 0, right - left, height()}, kSelectionFill);
                painter.setPen(kSelectionEdge);
                painter.drawLine(left, 0, left, height());
                painter.drawLine(right - 1, 0, right - 1, height());
            }
        } else if (selection_.start > 0 || playhead_ >= 0) {
            // An empty selection is still a caret: it says where a paste lands.
            const int x = xForSample(selection_.start);
            if (x >= plot.left() && x <= plot.right()) {
                painter.setPen(kSelectionEdge);
                painter.drawLine(x, 0, x, height());
            }
        }

        if (playhead_ >= viewStart_ && playhead_ < viewStart_ + viewLength_) {
            painter.setPen(kPlayhead);
            const int x = xForSample(playhead_);
            painter.drawLine(x, 0, x, height());
        }
    }

    paintGutter(painter);
}

} // namespace sa::ui
