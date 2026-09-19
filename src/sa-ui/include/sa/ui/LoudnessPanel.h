#pragma once

#include <sa/analysis/ComplianceTarget.h>
#include <sa/analysis/ProgrammeAnalysis.h>
#include <sa/core/Cancellation.h>
#include <sa/core/Types.h>
#include <sa/io/AudioSource.h>

#include <QWidget>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

class QComboBox;
class QLabel;

namespace sa::ui {

/// The metering panel: loudness, peaks, and whether the programme meets a
/// platform's target.
///
/// Measuring happens on a worker thread and streams the audio in blocks, so a
/// two-hour programme costs one block of memory and never blocks a redraw.
///
/// A new request cancels the one before it and waits for it to stop, which it
/// does at its next block boundary. Each request also carries a generation
/// number, so a result that arrives anyway -- one already in the post when it
/// was superseded -- is dropped rather than shown.
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

    /// Gain in dB that brings the last measurement onto the selected target
    /// without pushing the peaks through its ceiling, or nothing when there is
    /// no usable measurement to work from.
    [[nodiscard]] std::optional<double> conformGainDb() const;

    /// Name of the selected target, for an undo label.
    [[nodiscard]] QString targetName() const;

    /// The last completed measurement, or nothing if none has completed.
    [[nodiscard]] const analysis::ProgrammeAnalysis* latest() const noexcept {
        return hasLatest_ ? &latest_ : nullptr;
    }

signals:
    void measurementFinished();

private:
    void buildLayout();
    void show(const analysis::ProgrammeAnalysis& result, const QString& what);
    void showStereo(const analysis::StereoField& field);
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
    QLabel* correlation_ = nullptr;
    QLabel* width_ = nullptr;
    QLabel* balance_ = nullptr;
    QLabel* monoLoss_ = nullptr;

    /// The stereo rows and their heading, so they can be hidden together.
    /// Mono and 5.1 have no stereo field, and four rows reading "--" forever
    /// is four rows of the panel spent saying nothing.
    std::vector<QWidget*> stereoWidgets_;
    QLabel* verdict_ = nullptr;
    QComboBox* target_ = nullptr;

    analysis::ProgrammeAnalysis latest_;
    bool hasLatest_ = false;
    bool busy_ = false;

    /// Show a finished measurement. Called on the main thread only, from the
    /// event a worker posts.
    void deliver(const analysis::ProgrammeAnalysis& result, const QString& what, bool ok);

    /// What a worker shares with the panel that started it.
    ///
    /// Two different questions live here, and conflating them was a
    /// use-after-free. The generation says whether a result is still *wanted*,
    /// which a superseded worker can answer by itself. Whether the panel is
    /// still *there* is the other question, and a flag cannot answer it,
    /// because the panel can be destroyed between the check and the use: a
    /// worker that had passed its generation check and was about to post to a
    /// window that no longer existed.
    ///
    /// Found by reading rather than by reproducing, and it is worth being
    /// exact about that. Headless batch runs segfaulted twice under heavy
    /// load, and a third run died leaving no output at all; none of it
    /// reproduced -- not in sixty isolated runs under load, not in twenty
    /// under AddressSanitizer, not in three full suites under gdb. So this is
    /// a real hazard that was definitely there, and it is *not* established
    /// that it is the one that crashed. The other hazard found in the same
    /// reading -- a detached worker still running as the process tore down --
    /// is fixed below, and is the likelier of the two.
    ///
    /// So `panel` is read and used under `mutex`, and the destructor clears it
    /// under the same mutex. A worker inside the lock cannot be overtaken by
    /// the destructor, and a worker that arrives after it sees a null panel and
    /// drops the result. Posting is all that happens inside the lock, so
    /// nothing is held for longer than it takes to queue an event, and a queued
    /// event to an object Qt then destroys is removed by Qt itself.
    ///
    /// The generation is here too: bumped on every request, copied by the
    /// worker, and checked again on delivery, so a burst of selection changes
    /// leaves exactly the last one on screen.
    struct Session {
        std::mutex mutex;
        std::atomic<std::uint64_t> generation{0};
        LoudnessPanel* panel = nullptr;
    };

    std::shared_ptr<Session> session_;

    /// The measurement in flight, and the token that stops it.
    ///
    /// Owned and joined rather than detached. An earlier version detached,
    /// reasoning that a generation check made a late result harmless -- which
    /// it does -- but a detached worker is still *running* when the process
    /// exits, allocating through this project's instrumented operator new
    /// while the runtime tears down the state that instrumentation uses. It
    /// also meant a five-step batch run had five full measurements of the same
    /// document racing each other, four of which would be thrown away.
    ///
    /// Joining is only bearable because the work is cancellable: a superseded
    /// measurement stops at its next block boundary, which is milliseconds,
    /// not at the end of a two-hour file.
    CancellationToken cancellation_;
    std::thread worker_;

    /// Cancel and join whatever is running. Safe to call with nothing running.
    void stopWorker();
};

} // namespace sa::ui
