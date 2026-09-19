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
/// Measuring happens on a worker thread. Each request carries a generation
/// number, so a result already in the post when it was superseded is dropped
/// rather than shown. The reasoning about who may touch the panel is
/// LoudnessPanel's, deliberately copied rather than reinvented: see the Session
/// comment there for why the panel pointer is read under a mutex and not
/// through a flag.
///
/// Where this departs from that panel is in what happens to a superseded
/// worker, and the difference is forced. LoudnessPanel cancels and *waits*,
/// which it can afford because its work stops at the next block boundary --
/// milliseconds. Nothing here is cancellable inside a stage: detectKey,
/// trackTempo and measureBands each take a buffer and run to the end of it, and
/// on two minutes of audio the longest of them is a second of work in a release
/// build and several in a debug one. Waiting for that on the main thread is a
/// window that stops repainting every time somebody moves a selection, which is
/// the one thing this must not do.
///
/// So a superseded worker is cancelled and set aside rather than waited for.
/// It stops at its next stage boundary -- or, for the contour, within a second
/// of audio -- and is joined the next time one is swept up, or in the
/// destructor, which is the one place where waiting is the right thing. It
/// still holds its own generation, so it cannot write anything into the panel
/// on its way out.
class AnalysisPanel : public QWidget {
    Q_OBJECT

public:
    /// How much audio one run reads, at most.
    ///
    /// The tempo is the greediest of the answers and two minutes is well past
    /// the point where another bar changes it; the key settles inside one
    /// minute, and `keySeconds` below is the first minute of the same read
    /// rather than a second pass over the file. Both bounds are the ones
    /// auscultate-cli already uses, so the window and the command line answer the
    /// same question about the same file.
    ///
    /// Bounded at all because this is the one place in the product that holds
    /// audio whole -- detectKey, trackTempo and trackPitch each take a buffer
    /// -- and an unbounded read of a two-hour file would be a gigabyte and
    /// several minutes. The panel says what it read whenever it read less than
    /// was asked for; see coverageNote().
    ///
    /// Both are the defaults rather than the law: a preference can move them,
    /// and somebody working on hour-long concert recordings has a real reason
    /// to. See setBounds.
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

    /// Change how much audio a run reads and how much of it the key comes
    /// from. Takes effect on the next analysis, not on the one in flight.
    ///
    /// No validation here: sa::ui::validated() holds both bounds to the same
    /// rule for the file, the dialog and this, and a second opinion in the
    /// panel is a second place for them to disagree.
    void setBounds(double mostSeconds, double keySeconds) noexcept;

    [[nodiscard]] double mostSeconds() const noexcept { return mostSeconds_; }

    [[nodiscard]] double keySeconds() const noexcept { return keySeconds_; }

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

    /// One row of the panel as it currently stands.
    struct PanelRow {
        QString name;
        QString text;
        /// False for a row in a section nobody asked for. A hidden row's text
        /// is whatever it was last set to and means nothing.
        bool visible = true;
    };

    /// What the panel is showing, row by row.
    ///
    /// Exists so that a headless driver reads the labels rather than
    /// recomputing what they ought to say. A panel that decided correctly and
    /// then wrote the right text into the wrong row would pass every check
    /// made against a recomputation, and this is the only thing that catches
    /// it.
    [[nodiscard]] std::vector<PanelRow> shownRows() const;

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
    /// The tuning row and its label, hidden together. A row reading "+0 cents"
    /// on every well-made recording is a row spent saying nothing, and the
    /// side column has three panels to fit into 760 pixels.
    std::vector<QWidget*> tuningWidgets_;

    QLabel* tempo_ = nullptr;
    QLabel* tempoCaveat_ = nullptr;
    QLabel* tempoConfidence_ = nullptr;
    QLabel* beatGrid_ = nullptr;

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

    double mostSeconds_ = kMostSeconds;
    double keySeconds_ = kKeySeconds;

    void deliver(const MusicalAnalysis& result, const QString& what);

    /// What a worker shares with the panel that started it. LoudnessPanel's
    /// Session, for LoudnessPanel's reasons.
    struct Session {
        std::mutex mutex;
        std::atomic<std::uint64_t> generation{0};
        AnalysisPanel* panel = nullptr;
    };

    std::shared_ptr<Session> session_;

    /// The analysis in flight, the token that stops it, and the flag it sets
    /// as its last act.
    ///
    /// The token is shared rather than a member the worker points at, because
    /// a set-aside worker has to stay cancelled while the next one runs
    /// uncancelled -- one token between them would un-cancel the first.
    ///
    /// The flag is the only way to ask a std::thread whether it has stopped
    /// without blocking on it, which is exactly the question that decides
    /// whether a worker can be joined here or has to be set aside.
    std::shared_ptr<CancellationToken> cancellation_;
    std::shared_ptr<std::atomic<bool>> finished_;
    std::thread worker_;

    /// A worker that has been superseded and has not stopped yet.
    struct Retired {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    /// Set aside, never left to run loose. Each one has been cancelled, so the
    /// list drains itself: a sweep at the start of the next request joins
    /// whatever has stopped since. Bounded in practice by the quarter-second
    /// the window waits before re-analysing at all, which is longer than a
    /// cancelled worker takes to notice.
    std::vector<Retired> retired_;

    /// Cancel whatever is running and either join it, if it has already
    /// stopped, or set it aside.
    void retireWorker();

    /// Join and drop the set-aside workers that have stopped.
    void sweepRetired();
};

} // namespace sa::ui
