#include <sa/ui/EqCurveView.h>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace sa::ui {

namespace {

/// The modifier that turns a drag into a Q adjustment.
///
/// Shift rather than Control or Alt, which are the two desktop environments
/// most often claim for themselves -- window dragging, menu access -- and a
/// modifier the application never receives is a gesture nobody can use. The
/// wheel is the primary control for Q; this is the way in for a pointing
/// device that has none.
constexpr Qt::KeyboardModifier kQModifier = Qt::ShiftModifier;

/// One notch of a wheel in Qt's units: angleDelta counts eighths of a degree
/// and a conventional mouse steps fifteen degrees. A trackpad sends less and
/// gets a proportionally smaller change rather than nothing.
constexpr double kWheelNotch = 120.0;

/// A violet that appears nowhere else in the panel. Not a fourth blue: the
/// spectrum's own curves are already blue and amber, and an EQ curve that
/// could be mistaken for the material it is shaping is the one mistake this
/// drawing must not make.
const QColor kCurveLine{226, 122, 232};
const QColor kCurveFill{226, 122, 232, 46};
/// The individual bells, deliberately faint. They are working-out, shown so
/// that an overlap can be traced back to the bands that caused it; the sum is
/// the answer and keeps the strong colour.
const QColor kBandLine{150, 96, 168, 140};
const QColor kZeroLine{104, 96, 116};
const QColor kHandle{240, 200, 250};
const QColor kHandleEdge{40, 24, 46};
const QColor kReadout{236, 238, 245};

/// Post an event as a pointing device would deliver it, rather than calling
/// the handler: a synthesised gesture that skipped Qt's delivery would not be
/// exercising the same path the mouse takes.
void postMouse(QWidget* widget, QEvent::Type type, const QPointF& at, Qt::MouseButton button,
               Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event{type, at, widget->mapToGlobal(at), button, buttons, modifiers};
    QApplication::sendEvent(widget, &event);
}

} // namespace

EqCurveView::EqCurveView(QWidget* parent) : SpectrumView{parent} {}

void EqCurveView::setEqVisible(bool visible) {
    if (visible == visible_) {
        return;
    }
    visible_ = visible;
    // Focus only while the EQ is up: the panel has no keyboard behaviour
    // otherwise, and a widget that takes focus for nothing steals Escape and
    // the space bar from the transport.
    setFocusPolicy(visible ? Qt::ClickFocus : Qt::NoFocus);
    if (!visible) {
        dragging_ = -1;
        draggingQ_ = false;
    }
    update();
}

void EqCurveView::setEqSampleRate(SampleRate rate) {
    if (!curve_.setSampleRate(rate)) {
        return;
    }
    update();
}

void EqCurveView::clearBands() {
    curve_.clear();
    selected_ = -1;
    dragging_ = -1;
    draggingQ_ = false;
    bandsEdited();
}

void EqCurveView::bandsEdited() {
    update();
    emit bandsChanged();
}

void EqCurveView::paintEvent(QPaintEvent* event) {
    SpectrumView::paintEvent(event);
    if (!visible_) {
        return;
    }

    QPainter painter{this};
    const SpectrumPlot geometry = plot();
    const int zeroRow = geometry.yAtGain(0.0);

    // Where no change is. Without it the curve is a shape with no datum, and
    // "is that a cut or a small boost" is not a question a picture should
    // leave open. It needs no gain scale of its own: the panel's level grid
    // runs every 12 dB, so the lines either side of this one are already
    // exactly 6 dB of boost and 6 dB of cut.
    QPen zero{kZeroLine, 1.0};
    zero.setStyle(Qt::DashLine);
    painter.setPen(zero);
    painter.drawLine(geometry.left, zeroRow, geometry.left + geometry.width - 1, zeroRow);
    painter.setPen(kZeroLine);
    // Against the right-hand end, because the top of the audible band is the
    // emptiest corner of almost every curve. The band readout is drawn after
    // this and can land on top of it, which is the right way round: the label
    // says what a line already visible means, and the readout is the number
    // somebody is in the middle of setting.
    painter.drawText(QRect{geometry.left + geometry.width - 64, zeroRow - 16, 60, 14},
                     Qt::AlignRight | Qt::AlignBottom, tr("EQ 0 dB"));

    painter.setRenderHint(QPainter::Antialiasing, true);

    // The bells, when there is more than one. With a single band the bell and
    // the sum are the same line, and drawing it twice only thickens it.
    if (curve_.bandCount() > 1) {
        painter.setPen(QPen{kBandLine, 1.0});
        for (int index = 0; index < curve_.bandCount(); ++index) {
            QPainterPath bell;
            for (int column = 0; column < geometry.width; ++column) {
                const double hz = geometry.frequencyAtX(geometry.left + column);
                const QPointF point{
                    static_cast<double>(geometry.left + column),
                    static_cast<double>(geometry.yAtGain(curve_.bandResponseDbAt(index, hz)))};
                if (column == 0) {
                    bell.moveTo(point);
                } else {
                    bell.lineTo(point);
                }
            }
            painter.drawPath(bell);
        }
    }

    const std::vector<double> summed = curve_.responseAcross(geometry);
    QPainterPath curve;
    for (int column = 0; column < geometry.width; ++column) {
        const QPointF point{
            static_cast<double>(geometry.left + column),
            static_cast<double>(geometry.yAtGain(summed[static_cast<std::size_t>(column)]))};
        if (column == 0) {
            curve.moveTo(point);
        } else {
            curve.lineTo(point);
        }
    }

    // Filled back to the zero line, so that boost and cut read as areas rather
    // than as a wiggle somebody has to trace with a finger.
    QPainterPath filled = curve;
    filled.lineTo(geometry.left + geometry.width - 1, zeroRow);
    filled.lineTo(geometry.left, zeroRow);
    filled.closeSubpath();
    painter.fillPath(filled, kCurveFill);

    painter.setPen(QPen{kCurveLine, 2.0});
    painter.drawPath(curve);

    for (int index = 0; index < curve_.bandCount(); ++index) {
        const QPointF at{static_cast<double>(curve_.handleX(geometry, index)),
                         static_cast<double>(curve_.handleY(geometry, index))};
        const bool active = index == selected_ || index == dragging_;
        painter.setPen(QPen{kHandleEdge, 1.0});
        painter.setBrush(active ? QColor{255, 255, 255} : kHandle);
        painter.drawEllipse(at, active ? 5.0 : 3.5, active ? 5.0 : 3.5);
    }
    painter.setBrush(Qt::NoBrush);
    painter.setRenderHint(QPainter::Antialiasing, false);

    // The numbers. Shown while dragging, and left up afterwards for the band
    // still selected: the value just set is the one somebody wants to read,
    // and it is no use if it vanishes with the button.
    const int described = dragging_ >= 0 ? dragging_ : selected_;
    if (curve_.band(described) == nullptr) {
        return;
    }
    const QString text = QString::fromStdString(curve_.describe(described));
    const int textWidth = painter.fontMetrics().horizontalAdvance(text) + 4;
    const int handleColumn = curve_.handleX(geometry, described);
    const int right = geometry.left + geometry.width;

    // Beside the handle if the whole line fits there, on whichever side has
    // room; otherwise pinned to the top-left of the plot. The panel is narrow
    // enough that a band near either edge has no room beside it at all, and a
    // readout half off the panel is worse than one that is not where the eye
    // first looks -- the numbers have to be readable, not adjacent.
    int textLeft = handleColumn + 12;
    int textTop = std::clamp(curve_.handleY(geometry, described) - 8, geometry.top,
                             geometry.top + geometry.height - 16);
    Qt::Alignment alignment = Qt::AlignLeft;
    if (textLeft + textWidth > right) {
        textLeft = handleColumn - 12 - textWidth;
        alignment = Qt::AlignRight;
    }
    if (textLeft < geometry.left) {
        textLeft = geometry.left + 4;
        // The second row, because the spectrum's own cursor readout owns the
        // first -- and the cursor is by definition in the panel during a drag.
        textTop = geometry.top + 18;
        alignment = Qt::AlignLeft;
    }
    painter.setPen(kReadout);
    painter.drawText(QRect{textLeft, textTop, std::min(textWidth, right - textLeft), 16},
                     Qt::AlignVCenter | alignment, text);
}

void EqCurveView::mousePressEvent(QMouseEvent* event) {
    if (!visible_) {
        SpectrumView::mousePressEvent(event);
        return;
    }

    const QPoint at = event->position().toPoint();
    const int index = curve_.handleAt(plot(), at.x(), at.y());

    if (event->button() == Qt::RightButton) {
        if (index >= 0 && curve_.removeBand(index)) {
            selected_ = -1;
            dragging_ = -1;
            bandsEdited();
            event->accept();
            return;
        }
        SpectrumView::mousePressEvent(event);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        selected_ = index;
        dragging_ = index;
        draggingQ_ = index >= 0 && event->modifiers().testFlag(kQModifier);
        if (draggingQ_) {
            dragStartY_ = at.y();
            dragStartQ_ = curve_.band(index)->filter.q;
        }
        update();
        if (index >= 0) {
            event->accept();
            return;
        }
    }
    SpectrumView::mousePressEvent(event);
}

void EqCurveView::mouseMoveEvent(QMouseEvent* event) {
    if (visible_ && dragging_ >= 0) {
        const QPoint at = event->position().toPoint();
        const SpectrumPlot geometry = plot();
        if (draggingQ_) {
            // The frequency still follows the pointer, so a band can be tuned
            // and narrowed onto a resonance in one gesture; only the vertical
            // axis changes meaning. Two calls rather than one because the Q
            // arithmetic belongs with the rest of the curve's maths, where it
            // is tested, rather than spelt out a second time here.
            const dsp::EqBand* held = curve_.band(dragging_);
            (void)curve_.setBand(dragging_, geometry.frequencyAtX(at.x()), held->filter.gainDb,
                                 held->filter.q);
            (void)curve_.setQFromDrag(dragging_, dragStartQ_, at.y() - dragStartY_);
        } else {
            (void)curve_.moveHandleTo(geometry, dragging_, at.x(), at.y());
        }
        bandsEdited();
    }
    // Always, so the spectrum's own cursor readout keeps working under the EQ.
    SpectrumView::mouseMoveEvent(event);
}

void EqCurveView::mouseReleaseEvent(QMouseEvent* event) {
    if (visible_ && dragging_ >= 0) {
        dragging_ = -1;
        draggingQ_ = false;
        update();
        event->accept();
        return;
    }
    SpectrumView::mouseReleaseEvent(event);
}

void EqCurveView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!visible_ || event->button() != Qt::LeftButton) {
        SpectrumView::mouseDoubleClickEvent(event);
        return;
    }

    const QPoint at = event->position().toPoint();
    const SpectrumPlot geometry = plot();
    const int index = curve_.handleAt(geometry, at.x(), at.y());
    if (index >= 0) {
        // On a handle it is a way back to flat that keeps the band's frequency
        // and width, which is what hearing a band out asks for.
        const dsp::EqBand* existing = curve_.band(index);
        (void)curve_.setBand(index, existing->filter.frequency, 0.0, existing->filter.q);
        selected_ = index;
    } else {
        const int added = curve_.addBand(geometry.frequencyAtX(at.x()), geometry.gainAtY(at.y()),
                                         EqCurve::kDefaultQ);
        if (added < 0) {
            SpectrumView::mouseDoubleClickEvent(event);
            return;
        }
        selected_ = added;
    }
    dragging_ = -1;
    draggingQ_ = false;
    bandsEdited();
    event->accept();
}

void EqCurveView::wheelEvent(QWheelEvent* event) {
    if (!visible_) {
        SpectrumView::wheelEvent(event);
        return;
    }

    const QPoint at = event->position().toPoint();
    int index = curve_.handleAt(plot(), at.x(), at.y());
    if (index < 0) {
        // Falling back to the selection is what makes the wheel usable at all
        // on a small panel, where landing inside ten pixels of a handle while
        // also turning a wheel is more dexterity than the gesture deserves.
        index = selected_;
    }
    const double notches = static_cast<double>(event->angleDelta().y()) / kWheelNotch;
    if (index < 0 || notches == 0.0 || !curve_.adjustQByNotches(index, notches)) {
        SpectrumView::wheelEvent(event);
        return;
    }
    selected_ = index;
    bandsEdited();
    event->accept();
}

void EqCurveView::keyPressEvent(QKeyEvent* event) {
    const bool removing = event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace;
    if (visible_ && removing && curve_.removeBand(selected_)) {
        selected_ = -1;
        dragging_ = -1;
        bandsEdited();
        event->accept();
        return;
    }
    SpectrumView::keyPressEvent(event);
}

bool EqCurveView::addBandAt(double frequencyHz, double gainDb) {
    const SpectrumPlot geometry = plot();
    const QPointF at{static_cast<double>(geometry.xAtFrequency(frequencyHz)),
                     static_cast<double>(geometry.yAtGain(gainDb))};
    const int before = curve_.bandCount();
    postMouse(this, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    postMouse(this, QEvent::MouseButtonDblClick, at, Qt::LeftButton, Qt::LeftButton);
    postMouse(this, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    return curve_.bandCount() == before + 1;
}

bool EqCurveView::dragBandTo(int index, double frequencyHz, double gainDb) {
    if (curve_.band(index) == nullptr) {
        return false;
    }
    const SpectrumPlot geometry = plot();
    const QPointF from{static_cast<double>(curve_.handleX(geometry, index)),
                       static_cast<double>(curve_.handleY(geometry, index))};
    const QPointF to{static_cast<double>(geometry.xAtFrequency(frequencyHz)),
                     static_cast<double>(geometry.yAtGain(gainDb))};
    postMouse(this, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    // Whether the press found the handle it was aimed at, captured before the
    // release clears it. A drag that grabbed nothing would otherwise move the
    // pointer about and report success.
    const bool grabbed = dragging_ == index;
    postMouse(this, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
    postMouse(this, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
    return grabbed;
}

bool EqCurveView::shiftDragBandBy(int index, int dy) {
    if (curve_.band(index) == nullptr) {
        return false;
    }
    const SpectrumPlot geometry = plot();
    const QPointF from{static_cast<double>(curve_.handleX(geometry, index)),
                       static_cast<double>(curve_.handleY(geometry, index))};
    const QPointF to{from.x(), from.y() + dy};
    postMouse(this, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, kQModifier);
    // That the press started a Q drag rather than an ordinary one, which is
    // the whole thing the modifier is meant to do.
    const bool grabbed = dragging_ == index && draggingQ_;
    postMouse(this, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, kQModifier);
    postMouse(this, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, kQModifier);
    return grabbed;
}

bool EqCurveView::turnWheelOverBand(int index, double notches) {
    if (curve_.band(index) == nullptr) {
        return false;
    }
    const SpectrumPlot geometry = plot();
    const QPointF at{static_cast<double>(curve_.handleX(geometry, index)),
                     static_cast<double>(curve_.handleY(geometry, index))};
    const QPoint angle{0, static_cast<int>(std::lround(notches * kWheelNotch))};
    QWheelEvent event{at,           mapToGlobal(at), QPoint{0, 0},      angle,
                      Qt::NoButton, Qt::NoModifier,  Qt::NoScrollPhase, false};
    QApplication::sendEvent(this, &event);
    // wheelEvent selects whatever it acted on, so this says the turn landed on
    // the band rather than falling through to the widget below.
    return selected_ == index;
}

bool EqCurveView::clickAwayBand(int index) {
    if (curve_.band(index) == nullptr) {
        return false;
    }
    const SpectrumPlot geometry = plot();
    const QPointF at{static_cast<double>(curve_.handleX(geometry, index)),
                     static_cast<double>(curve_.handleY(geometry, index))};
    const int before = curve_.bandCount();
    postMouse(this, QEvent::MouseButtonPress, at, Qt::RightButton, Qt::RightButton);
    postMouse(this, QEvent::MouseButtonRelease, at, Qt::RightButton, Qt::NoButton);
    return curve_.bandCount() == before - 1;
}

} // namespace sa::ui
