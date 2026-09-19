#include <sa/ui/SpectrumView.h>
#include <sa/ui/ViewGeometry.h>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <utility>

namespace sa::ui {

namespace {

/// Height of the strip below the plot that carries the frequency labels.
constexpr int kLabelStrip = 18;

const QColor kBackground{18, 18, 22};
const QColor kGrid{58, 58, 68};
const QColor kAxisText{150, 150, 162};
const QColor kAverageFill{86, 154, 214, 140};
const QColor kAverageLine{126, 194, 244};
const QColor kPeakLine{232, 186, 104};
const QColor kCursorLine{220, 220, 230, 120};

} // namespace

SpectrumView::SpectrumView(QWidget* parent) : QWidget{parent} {
    setMouseTracking(true);
    setAutoFillBackground(false);
}

void SpectrumView::setSpectrum(std::vector<float> average, std::vector<float> peak, SampleRate rate,
                               int fftSize) {
    average_ = std::move(average);
    peak_ = std::move(peak);
    rate_ = rate;
    fftSize_ = fftSize;
    note_.clear();
    update();
}

void SpectrumView::clear() {
    average_.clear();
    peak_.clear();
    fftSize_ = 0;
    update();
}

void SpectrumView::setNote(QString note) {
    note_ = std::move(note);
    update();
}

QRect SpectrumView::plotRect() const {
    return QRect{kGutterWidth, 0, std::max(1, width() - kGutterWidth),
                 std::max(1, height() - kLabelStrip)};
}

double SpectrumView::frequencyAtX(int x) const {
    const QRect plot = plotRect();
    const double fraction =
        plot.width() > 1 ? static_cast<double>(x - plot.left()) / static_cast<double>(plot.width())
                         : 0.0;
    return frequencyAtFraction(FrequencyScale::Logarithmic, std::clamp(fraction, 0.0, 1.0),
                               rate_.hz() * 0.5);
}

int SpectrumView::xAtFrequency(double hz) const {
    const QRect plot = plotRect();
    const double fraction = fractionAtFrequency(FrequencyScale::Logarithmic, hz, rate_.hz() * 0.5);
    return plot.left() + static_cast<int>(std::lround(fraction * plot.width()));
}

int SpectrumView::yAtLevel(double decibels) const {
    const QRect plot = plotRect();
    const double fraction = std::clamp((kTopDb - decibels) / (kTopDb - kBottomDb), 0.0, 1.0);
    return plot.top() + static_cast<int>(std::lround(fraction * (plot.height() - 1)));
}

double SpectrumView::loudestIn(const std::vector<float>& curve, double from, double to) const {
    if (curve.empty() || fftSize_ <= 0) {
        return kBottomDb;
    }
    const double perBin = rate_.hz() / static_cast<double>(fftSize_);
    if (!(perBin > 0.0)) {
        return kBottomDb;
    }
    const auto last = static_cast<int>(curve.size()) - 1;
    // At least one bin, always: at the left of a log axis a column spans far
    // less than a bin, and rounding both edges to the same index would draw
    // nothing.
    int first = std::clamp(static_cast<int>(std::floor(from / perBin)), 0, last);
    int stop = std::clamp(static_cast<int>(std::ceil(to / perBin)), first, last);

    double loudest = kBottomDb;
    for (int bin = first; bin <= stop; ++bin) {
        loudest = std::max(loudest, static_cast<double>(curve[static_cast<std::size_t>(bin)]));
    }
    return loudest;
}

void SpectrumView::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.fillRect(rect(), kBackground);

    const QRect plot = plotRect();
    const double nyquist = rate_.hz() * 0.5;

    // Level grid, every 12 dB: nine lines over the range, which is enough to
    // read a level off and few enough not to compete with the curves.
    painter.setPen(kGrid);
    QFont small = painter.font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 2.0));
    painter.setFont(small);
    for (double decibels = kTopDb; decibels >= kBottomDb; decibels -= 12.0) {
        const int y = yAtLevel(decibels);
        painter.setPen(kGrid);
        painter.drawLine(plot.left(), y, plot.right(), y);
        painter.setPen(kAxisText);
        painter.drawText(QRect{0, y - 8, kGutterWidth - 6, 16}, Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(static_cast<int>(decibels)));
    }

    // Frequency grid, at the 1-2-5 positions of a log axis.
    for (const AxisTick& tick :
         frequencyTicks(FrequencyScale::Logarithmic, nyquist, plot.width())) {
        const int x = plot.left() + static_cast<int>(std::lround(tick.fraction * plot.width()));
        painter.setPen(kGrid);
        painter.drawLine(x, plot.top(), x, plot.bottom());
        if (tick.major) {
            painter.setPen(kAxisText);
            painter.drawText(QRect{x - 34, plot.bottom() + 1, 68, kLabelStrip - 2},
                             Qt::AlignHCenter | Qt::AlignVCenter,
                             QString::fromStdString(tick.label));
        }
    }

    if (!note_.isEmpty() || average_.empty()) {
        painter.setPen(kAxisText);
        painter.drawText(plot, Qt::AlignCenter, note_.isEmpty() ? tr("No spectrum yet") : note_);
        return;
    }

    // One value per pixel column, taking the loudest bin that column covers so
    // a narrow peak cannot fall between two columns and vanish.
    const int columns = plot.width();
    std::vector<double> averageDb(static_cast<std::size_t>(columns));
    std::vector<double> peakDb(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        const double from = frequencyAtX(plot.left() + column);
        const double to = frequencyAtX(plot.left() + column + 1);
        averageDb[static_cast<std::size_t>(column)] = loudestIn(average_, from, to);
        peakDb[static_cast<std::size_t>(column)] =
            peak_.empty() ? kBottomDb : loudestIn(peak_, from, to);
    }

    QPainterPath filled;
    filled.moveTo(plot.left(), plot.bottom());
    for (int column = 0; column < columns; ++column) {
        filled.lineTo(plot.left() + column, yAtLevel(averageDb[static_cast<std::size_t>(column)]));
    }
    filled.lineTo(plot.left() + columns - 1, plot.bottom());
    filled.closeSubpath();
    painter.fillPath(filled, kAverageFill);

    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!peak_.empty()) {
        QPainterPath line;
        line.moveTo(plot.left(), yAtLevel(peakDb[0]));
        for (int column = 1; column < columns; ++column) {
            line.lineTo(plot.left() + column, yAtLevel(peakDb[static_cast<std::size_t>(column)]));
        }
        painter.setPen(QPen{kPeakLine, 1.0});
        painter.drawPath(line);
    }
    {
        QPainterPath line;
        line.moveTo(plot.left(), yAtLevel(averageDb[0]));
        for (int column = 1; column < columns; ++column) {
            line.lineTo(plot.left() + column,
                        yAtLevel(averageDb[static_cast<std::size_t>(column)]));
        }
        painter.setPen(QPen{kAverageLine, 1.4});
        painter.drawPath(line);
    }
    painter.setRenderHint(QPainter::Antialiasing, false);

    if (cursorX_ >= plot.left() && cursorX_ <= plot.right()) {
        painter.setPen(kCursorLine);
        painter.drawLine(cursorX_, plot.top(), cursorX_, plot.bottom());

        const int column = std::clamp(cursorX_ - plot.left(), 0, columns - 1);
        const double hz = frequencyAtX(cursorX_);
        const QString text = tr("%1  avg %2  peak %3")
                                 .arg(QString::fromStdString(formatFrequency(hz)))
                                 .arg(averageDb[static_cast<std::size_t>(column)], 0, 'f', 1)
                                 .arg(peakDb[static_cast<std::size_t>(column)], 0, 'f', 1);
        painter.setPen(kAxisText);
        painter.drawText(QRect{plot.left() + 6, plot.top() + 2, plot.width() - 12, 16},
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }
}

void SpectrumView::mouseMoveEvent(QMouseEvent* event) {
    cursorX_ = event->position().toPoint().x();
    update();
}

void SpectrumView::leaveEvent(QEvent* /*event*/) {
    cursorX_ = -1;
    update();
}

} // namespace sa::ui
