#pragma once

#include <sa/analysis/ComplianceTarget.h>
#include <sa/analysis/ProgrammeAnalysis.h>
#include <sa/core/Types.h>
#include <sa/io/AudioSource.h>

#include <QWidget>
#include <atomic>
#include <memory>

class QComboBox;
class QLabel;

namespace sa::ui {

/// The metering panel: loudness, peaks, and whether the programme meets a
/// platform's target.
///
/// Measuring happens on a worker thread and streams the audio in blocks, so a
/// two-hour programme costs one block of memory and never blocks a redraw. Each
/// request carries a generation number; a result from a superseded generation is
/// dropped rather than cancelled, because the meters have no cancellation point
/// and a stale number on screen is worse than a wasted pass.
class LoudnessPanel : public QWidget {
    Q_OBJECT

public:
    explicit LoudnessPanel(QWidget* parent = nullptr);
    ~LoudnessPanel() override;

    /// Measure [start, start + length) of `source`. Passing nullptr clears.
    void measure(std::shared_ptr<const io::AudioSource> source, SampleIndex start,
                 SampleCount length, const QString& what);

    void clear();

    /// True while a measurement is outstanding. The headless screenshot path
    /// waits on this; without it, a batch run captures the panel mid-measure
    /// and every number reads "--".
    [[nodiscard]] bool busy() const noexcept { return busy_; }

    /// The last completed measurement, or nothing if none has completed.
    [[nodiscard]] const analysis::ProgrammeAnalysis* latest() const noexcept {
        return hasLatest_ ? &latest_ : nullptr;
    }

signals:
    void measurementFinished();

private:
    void buildLayout();
    void show(const analysis::ProgrammeAnalysis& result, const QString& what);
    void showCompliance(const analysis::ProgrammeAnalysis& result);
    void setPending(const QString& what);

    QLabel* heading_ = nullptr;
    QLabel* integrated_ = nullptr;
    QLabel* range_ = nullptr;
    QLabel* shortTerm_ = nullptr;
    QLabel* momentary_ = nullptr;
    QLabel* maximumShortTerm_ = nullptr;
    QLabel* truePeak_ = nullptr;
    QLabel* samplePeak_ = nullptr;
    QLabel* rms_ = nullptr;
    QLabel* crest_ = nullptr;
    QLabel* dcOffset_ = nullptr;
    QLabel* peakToLoudness_ = nullptr;
    QLabel* verdict_ = nullptr;
    QComboBox* target_ = nullptr;

    analysis::ProgrammeAnalysis latest_;
    bool hasLatest_ = false;
    bool busy_ = false;

    /// Bumped on every request. The worker copies it and its result is accepted
    /// only if it still matches, so a burst of selection changes leaves exactly
    /// the last one on screen.
    std::shared_ptr<std::atomic<std::uint64_t>> generation_;
};

} // namespace sa::ui
