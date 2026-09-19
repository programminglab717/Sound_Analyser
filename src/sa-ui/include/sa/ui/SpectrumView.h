#pragma once

#include <sa/core/Types.h>

#include <QString>
#include <QWidget>
#include <vector>

namespace sa::ui {

/// The average spectrum of what is selected, drawn on a log frequency axis.
///
/// The spectrogram answers "when", and this answers "how much". Both come from
/// the same transform and neither substitutes for the other: judging a tonal
/// balance, finding a resonance or seeing where a noise floor sits are
/// questions about a whole passage, and reading them off a spectrogram means
/// averaging by eye.
///
/// Two curves, because the pair says more than either. The filled one is the
/// average -- what the passage is made of. The line above it is the loudest any
/// single frame reached at that frequency. A resonance that rings moves both; a
/// cymbal moves only the peak; mains hum shows as a spike in the average that
/// the peak barely exceeds, because it never varies. The gap between them is
/// how much a frequency comes and goes.
class SpectrumView : public QWidget {
    Q_OBJECT

public:
    explicit SpectrumView(QWidget* parent = nullptr);

    /// Replace the curves. Both hold one value per bin, in dBFS.
    void setSpectrum(std::vector<float> average, std::vector<float> peak, SampleRate rate,
                     int fftSize);

    void clear();

    /// A line shown instead of the curves: "measuring", "nothing selected", a
    /// failure. Empty means draw the curves.
    void setNote(QString note);

    [[nodiscard]] QSize sizeHint() const override { return {360, 180}; }

    [[nodiscard]] QSize minimumSizeHint() const override { return {200, 110}; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    /// Top and bottom of the level axis, in dBFS. Zero at the top because that
    /// is full scale and there is nothing above it to show; -108 at the bottom
    /// because it is a round eighteen decades of nothing and puts the noise
    /// floor of a 16-bit delivery comfortably on the display.
    static constexpr double kTopDb = 0.0;
    static constexpr double kBottomDb = -108.0;

    [[nodiscard]] QRect plotRect() const;
    [[nodiscard]] double frequencyAtX(int x) const;
    [[nodiscard]] int xAtFrequency(double hz) const;
    [[nodiscard]] int yAtLevel(double decibels) const;
    /// Loudest value of `curve` in the bins covering [from, to) Hz, or nothing
    /// below the floor. Taking the loudest rather than the mean is what keeps a
    /// narrow peak visible when a column spans many bins.
    [[nodiscard]] double loudestIn(const std::vector<float>& curve, double from, double to) const;

    std::vector<float> average_;
    std::vector<float> peak_;
    SampleRate rate_{48000.0};
    int fftSize_ = 0;
    QString note_;
    int cursorX_ = -1;
};

} // namespace sa::ui
