#pragma once

#include <sa/analysis/OctaveBands.h>
#include <sa/core/Types.h>
#include <sa/ui/ViewGeometry.h>

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

    /// The rate the frequency axis is drawn against.
    ///
    /// Settable on its own as well as through setSpectrum, because anything
    /// drawing over this panel has to be put on the document's axis the moment
    /// the document is opened -- which is before the first transform has
    /// finished and before there is any spectrum to hand.
    void setSampleRate(SampleRate rate);

    [[nodiscard]] SampleRate sampleRate() const noexcept { return rate_; }

    /// The plot area and its axis mapping, as a value. This is what a subclass
    /// drawing over the spectrum uses, so that it cannot land a curve anywhere
    /// but where the spectrum under it already is.
    [[nodiscard]] SpectrumPlot plot() const;

    /// Freeze the average currently shown, to compare later ones against.
    /// Returns false when there is nothing to freeze.
    ///
    /// The comparison a spectrum analyser is for. Judging a tonal balance
    /// means asking "compared with what", and the honest answer is usually
    /// another passage: the chorus against the verse, this take against the
    /// last one, a master against the reference it is chasing. Remembering a
    /// curve well enough to compare it with the next one is not a thing anyone
    /// can do by eye, so the panel remembers it instead.
    ///
    /// Only the average is kept. The peak curve says how much a frequency
    /// comes and goes, which is a question about one passage rather than a
    /// comparison between two.
    bool captureReference();

    void clearReference();

    [[nodiscard]] bool hasReference() const noexcept { return !reference_.empty(); }

    /// Lay octave or third-octave bands over the curves.
    ///
    /// The oldest way of describing a spectrum and the one people talk in: a
    /// thirty-one band display is the language of room correction and of every
    /// conversation containing the phrase "too much at 200". It goes on this
    /// panel rather than on one of its own because it is the same measurement
    /// of the same passage on the same two axes, and reading a bar chart
    /// against the curve it came from is the point of having both.
    ///
    /// Bars are drawn to the band edges that were actually integrated, so on
    /// the panel's logarithmic axis they come out evenly wide -- which is what
    /// makes the chart readable where the FFT curve slopes.
    ///
    /// Passing an empty vector takes them away.
    void setOctaveBands(std::vector<analysis::Band> bands);

    [[nodiscard]] bool hasOctaveBands() const noexcept { return !octaveBands_.empty(); }

    /// A line shown instead of the curves: "measuring", "nothing selected", a
    /// failure. Empty means draw the curves.
    void setNote(QString note);

    [[nodiscard]] QSize sizeHint() const override { return {360, 180}; }

    [[nodiscard]] QSize minimumSizeHint() const override { return {200, 110}; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

    [[nodiscard]] QRect plotRect() const;

private:
    [[nodiscard]] double frequencyAtX(int x) const;
    [[nodiscard]] int xAtFrequency(double hz) const;
    [[nodiscard]] int yAtLevel(double decibels) const;
    /// Loudest value of `curve` in the bins covering [from, to) Hz, or nothing
    /// below the floor. Taking the loudest rather than the mean is what keeps a
    /// narrow peak visible when a column spans many bins.
    /// `rate` and `fftSize` are passed rather than read from the members
    /// because the reference curve carries its own: it can have been captured
    /// from a different file at a different sample rate, and mapping its bins
    /// with the current file's spacing would draw it at the wrong frequencies.
    [[nodiscard]] double loudestIn(const std::vector<float>& curve, double from, double to,
                                   SampleRate rate, int fftSize) const;

    std::vector<float> average_;
    std::vector<float> peak_;
    SampleRate rate_{48000.0};
    int fftSize_ = 0;

    std::vector<float> reference_;
    SampleRate referenceRate_{48000.0};
    int referenceFftSize_ = 0;
    std::vector<analysis::Band> octaveBands_;
    QString note_;
    int cursorX_ = -1;
};

} // namespace sa::ui
