#pragma once

#include <sa/core/Cancellation.h>
#include <sa/core/Types.h>
#include <sa/io/AudioSource.h>
#include <sa/ui/AnalysisReadout.h>

#include <QWidget>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class QLabel;

namespace sa::ui {

/// The musical panel: what key it is in, how fast it goes, what note is
/// sounding, and what the room did to it.
///
/// Everything here was already in the library and reachable only from the
/// command line, which is the wrong way round for a product whose claim is that
/// it analyses first. The panel's job is not to print those numbers -- that is
/// a morning's work -- but to refuse to print the ones that have not been
/// earned. See AnalysisReadout.h, where every such decision is made, away from
/// this window, so it can be checked by calling it.
///
/// Measuring happens on a worker thread. A new request cancels the one before
/// it and waits for it to stop, and each request carries a generation number so
/// that a result already in the post when it was superseded is dropped rather
/// than shown. The lifetime reasoning is LoudnessPanel's, deliberately copied
/// rather than reinvented: see the Session comment there for why the panel
/// pointer is read under a mutex and not through a flag.
///
/// What it does not claim. Nothing here is cancellable *inside* a stage: the
/// analysis functions take a buffer and run to completion, so a cancellation
/// lands between stages rather than part-way through one. What keeps a stage
/// short is the bounded read below, not responsiveness in the analysis.
class AnalysisPanel : public QWidget {
    Q_OBJECT

public:
    /// How much audio one run reads, at most.
    ///
    /// The tempo is the greediest of the answers and two minutes is well past
    /// the point where another bar changes it; the key settles inside one
    /// minute, and `keySeconds` below is the first minute of the same read
    /// rather than a second pass over the file. Both bounds are the ones
    /// auscult-cli already uses, so the window and the command line answer the
    /// same question about the same file.
    ///
    /// Bounded at all because this is the one place in the product that holds
    /// audio whole -- detectKey, trackTempo and trackPitch each take a buffer
    /// -- and an unbounded read of a two-hour file would be a gigabyte and
    /// several minutes. The panel says what it read whenever it read less than
    /// was asked for; see coverageNote().
    static constexpr double kMostSeconds = 120.0;
    static constexpr double kKeySeconds = 60.0;

    /// Which of the optional stages to run.
    ///
    /// Key, tempo and bands are not in here because they always run: they are
    /// questions worth asking of any file, and they are what the beat grid over
    /// the waveform is drawn from. The two that are optional are the two that
    /// are either expensive or meaningless unless the user says otherwise --
    /// a pitch contour over polyphony follows one note and cannot say which,
    /// and room acoustics are an answer about an impulse response and nonsense
    /// about anything else.
    struct Request {
        bool pitch = false;
        bool room = false;
    };

    explicit AnalysisPanel(QWidget* parent = nullptr);
    ~AnalysisPanel() override;

    /// Analyse [start, start + length) of `source`. Passing nullptr clears.
    void analyse(std::shared_ptr<const io::AudioSource> source, SampleIndex start,
                 SampleCount length, const QString& what, Request request);

    void clear();

    /// True while an analysis is outstanding. The headless paths wait on this
    /// for the same reason they wait on the meters: without it a batch run
    /// captures a panel of dashes.
    [[nodiscard]] bool busy() const noexcept { return busy_; }

    /// The last completed run, or nothing if none has completed.
    [[nodiscard]] const MusicalAnalysis* latest() const noexcept {
        return hasLatest_ ? &latest_ : nullptr;
    }

signals:
    void analysisFinished();

private:
    void buildLayout();
    void show(const MusicalAnalysis& result, const QString& what);
    void showKey(const MusicalAnalysis& result);
    void showTempo(const MusicalAnalysis& result);
    void showPitch(const MusicalAnalysis& result);
    void showRoom(const MusicalAnalysis& result);

    /// Put a reading in a row: the value, and the caveat under it in the
    /// colour that says it is one. A row whose reading has no answer shows the
    /// caveat in place of the number rather than beside it, because there is
    /// nothing for it to be beside.
    void setRow(QLabel* value, QLabel* caveat, const Reading& reading);

    QLabel* heading_ = nullptr;
    QLabel* coverage_ = nullptr;

    QLabel* key_ = nullptr;
    QLabel* keyCaveat_ = nullptr;
    QLabel* runnerUp_ = nullptr;
    QLabel* tuning_ = nullptr;

    QLabel* tempo_ = nullptr;
    QLabel* tempoCaveat_ = nullptr;
    QLabel* tempoConfidence_ = nullptr;
    QLabel* firstBeat_ = nullptr;
    QLabel* beatCount_ = nullptr;

    QLabel* pitch_ = nullptr;
    QLabel* pitchCaveat_ = nullptr;
    QLabel* voiced_ = nullptr;

    QLabel* earlyDecay_ = nullptr;
    QLabel* t20_ = nullptr;
    QLabel* t30_ = nullptr;
    QLabel* clarity50_ = nullptr;
    QLabel* clarity80_ = nullptr;
    QLabel* definition50_ = nullptr;
    QLabel* centreTime_ = nullptr;
    QLabel* usableRange_ = nullptr;
    QLabel* roomCaveat_ = nullptr;

    /// The pitch and room rows with their headings, so each group can be
    /// hidden entirely. Eight rows of "--" forever is eight rows of the panel
    /// spent saying nothing, which is LoudnessPanel's reasoning for the stereo
    /// section and holds here twice over.
    std::vector<QWidget*> pitchWidgets_;
    std::vector<QWidget*> roomWidgets_;

    MusicalAnalysis latest_;
    bool hasLatest_ = false;
    bool busy_ = false;

    void deliver(const MusicalAnalysis& result, const QString& what);

    /// What a worker shares with the panel that started it. LoudnessPanel's
    /// Session, for LoudnessPanel's reasons.
    struct Session {
        std::mutex mutex;
        std::atomic<std::uint64_t> generation{0};
        AnalysisPanel* panel = nullptr;
    };

    std::shared_ptr<Session> session_;

    CancellationToken cancellation_;
    std::thread worker_;

    void stopWorker();
};

} // namespace sa::ui
