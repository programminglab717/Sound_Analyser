#pragma once

#include <sa/core/Cancellation.h>
#include <sa/dsp/ChannelOps.h>
#include <sa/dsp/Deess.h>
#include <sa/dsp/Dither.h>
#include <sa/dsp/OfflineDynamics.h>
#include <sa/engine/Clip.h>
#include <sa/engine/Document.h>
#include <sa/engine/DocumentSource.h>
#include <sa/engine/UndoHistory.h>
#include <sa/spectral/Denoise.h>
#include <sa/transport/Player.h>
#include <sa/ui/Colourmap.h>
#include <sa/ui/LoudnessPanel.h>
#include <sa/ui/SpectrogramView.h>
#include <sa/ui/SpectrumView.h>
#include <sa/ui/TimeRuler.h>
#include <sa/ui/WaveformView.h>

#include <QMainWindow>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

class QAction;
class QLabel;
class QTimer;

namespace sa::ui {

/// The application window: a shared time ruler over a waveform over a
/// spectrogram, all on one time axis.
///
/// The views are locked together deliberately. The product's whole claim is
/// that analysis and editing are the same surface, and that falls apart the
/// moment the user has to reconcile two different scroll positions -- or two
/// different selections -- in their head.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

    /// Load an audio file, build its caches and display it. Returns false and
    /// reports on the status bar if the file cannot be opened.
    bool openFile(const std::filesystem::path& path);

    /// Save the arrangement, consolidating any audio that exists only in
    /// memory into a folder beside the session first.
    bool saveSession(const std::filesystem::path& path);

    /// Reopen a saved arrangement.
    bool openSession(const std::filesystem::path& path);

    /// Write the edited document to a WAV file.
    bool exportTo(const std::filesystem::path& path, bool selectionOnly);

    /// Select a span given in seconds. Used by the headless verification seam
    /// below, and by anything that wants to drive the window programmatically.
    void selectSeconds(double from, double to);

    /// Apply one editing operation by name: cut, copy, paste, delete, silence,
    /// trim, undo, redo, selectall or deselect.
    ///
    /// This exists so the editing path can be exercised without a mouse. A
    /// window that opens and draws but silently fails to cut would pass every
    /// other check in this project, because every other check stops at the
    /// picture.
    [[nodiscard]] bool applyOperation(const QString& name);

    /// Render the window to a PNG without needing a display. This is how the UI
    /// is checked in an environment with no screen, and it doubles as a CI smoke
    /// test that the whole load-analyse-draw path really runs.
    /// Spin the event loop until no analysis is outstanding, or `timeoutMs`
    /// passes. Returns false on timeout.
    ///
    /// Ten minutes, because the thing being waited for is real work on real
    /// material: a twenty-minute file takes about thirty seconds to analyse in
    /// release and several times that under a sanitiser. A batch run that gives
    /// up early writes a file that looks finished and is not.
    [[nodiscard]] bool waitForAnalysis(int timeoutMs = 600000);

    /// Play the current selection to its end, pumping the event loop so the
    /// playhead advances, then report where it got to. Returns false if
    /// playback could not start or did not finish within `timeoutMs`.
    ///
    /// This is how the transport is checked without a sound card: the null
    /// device runs a real thread on a real clock, so everything except the
    /// final hand-off to hardware is exercised.
    [[nodiscard]] bool playToEnd(int timeoutMs = 120000);

    /// Print the completed measurement as key=value lines on stdout, so a test
    /// can check the numbers rather than the pixels showing them.
    [[nodiscard]] bool printAnalysis() const;

    [[nodiscard]] bool saveScreenshot(const std::filesystem::path& path);

    /// Render just the spectrogram's plotting area, with no gutter and no
    /// window chrome. A test that checks pixels needs to know what it is
    /// looking at; grabbing the whole window makes it guess where the plot
    /// starts, and that guess is what breaks the next time the layout moves.
    [[nodiscard]] bool saveSpectrogramImage(const std::filesystem::path& path);

    /// The spectrum panel alone, without its gutter. As with the spectrogram,
    /// the point is that a test can say what the pixels should be rather than
    /// only that the program did not crash.
    [[nodiscard]] bool saveSpectrumImage(const std::filesystem::path& path);

private slots:
    void chooseFile();
    void chooseSaveSession();
    void chooseOpenSession();
    void chooseExport();
    void chooseExportSelection();

private:
    void buildMenus();
    void setColourmap(Colourmap map);
    void setFrequencyScale(FrequencyScale scale);
    void showWaveformCursor(double seconds, double peakDecibels);
    void showSpectrogramCursor(double seconds, double hz, double decibels);
    void selectionChanged(SampleIndex start, SampleIndex end);

    /// Re-measure whatever the panel should be showing: the selection when
    /// there is one, the whole document otherwise.
    void remeasure();
    /// Re-measure and redraw the spectrum, after a short pause.
    ///
    /// Deferred rather than immediate because the selection changes on every
    /// mouse move of a drag. The spectrum reads several seconds of audio and
    /// transforms it; the meters start a worker, and starting one now means
    /// stopping the one before it, which means waiting for it. Neither is
    /// something to do sixty times a second for answers nobody will read until
    /// the mouse stops.
    void reanalyseSoon();

    /// Do it now. For the headless paths, which have no drag and no patience.
    void reanalyseNow();
    void respectrum();

    /// Rebuild the peak cache from the *document*, not the file. After the
    /// first edit those are different things, and showing the file is showing
    /// the user something they did not ask for.
    ///
    /// The peak pyramid is fast -- under a second for twenty minutes -- so it
    /// is built here and the waveform appears at once. The spectrogram takes
    /// several seconds on the same material and is built on a worker.
    void rebuildCaches();

    /// Start a background spectrogram build, cancelling any already running.
    void startSpectrogramBuild();

    /// The analysis settings the spectrogram is actually being built with.
    ///
    /// Not always displayConfig(): a long file gets a coarser hop so that its
    /// cache fits, and the build and the note both have to agree on which one
    /// was chosen.
    spectral::SpectrogramConfig spectrogramConfig_;

    /// What to say about the spectrogram once it has built, if anything.
    ///
    /// Separate from the status note because that one is also used for
    /// "building…" and for failures, and the completion handler clears it. A
    /// long file's "coarser than usual" message has to survive that.
    QString spectrogramResolutionNote_;

    /// Stop a running spectrogram build and wait for its thread.
    ///
    /// Called before anything that replaces the document source the worker is
    /// reading from. That is a lifetime rule, not politeness.
    void cancelSpectrogramBuild();
    void refreshViews();
    void refreshActions();
    void updateStatus();

    /// Run an edit, commit it to the history under `label`, and bring the views
    /// back in step. An edit that fails leaves the document and the history
    /// untouched, which is why the verbs return a Status rather than throwing.
    template <typename Edit>
    [[nodiscard]] bool applyEdit(const QString& label, Edit&& edit);

    /// The range an operation acts on: the selection if there is one, otherwise
    /// the whole document. "No selection" means "all of it" everywhere in this
    /// window, and having each caller decide that separately is how they drift.
    [[nodiscard]] TimeSelection targetRange() const noexcept;

    /// Render the range plus the context a spectral edit needs, run `edit` over
    /// it, and put the result back. The context matters: an edit spreads by up
    /// to one analysis window either side, and writing back only the selection
    /// would clip that spread into a click at each seam.
    template <typename Edit>
    void applySpectralEdit(const QString& label, Edit&& edit);

    /// Play from the selection, or from the caret to the end when there is no
    /// selection. Pressing it again stops.
    void togglePlayback();
    void stopPlayback();
    void followPlayhead();

    /// The four editing verbs that need no settings and no dialog. The
    /// arithmetic lives in sa-dsp, because the headless driver needs the same
    /// four; this applies one to the selection and makes it undoable.
    bool applyChannelOp(dsp::ChannelOp operation, const QString& label);

    /// Markers. The engine has carried them since the document model was
    /// written -- sessions save them, and a ripple delete moves them with the
    /// audio -- but nothing reached them until now.
    bool addMarker(const QString& label);
    void chooseAddMarker();
    void renameMarker();
    bool deleteNearestMarker();
    bool clearMarkers();
    void goToMarker(bool forwards);
    /// Index of the marker nearest the caret, or -1 when there are none.
    [[nodiscard]] int nearestMarker() const noexcept;

    /// Freeze the spectrum on screen to compare later selections against, and
    /// forget it again.
    void captureSpectrumReference();
    void clearSpectrumReference();

    void learnNoiseProfile();
    void chooseDenoise();
    void chooseAttenuate();
    void healSelection();
    void selectFrequencyBand(double lowHz, double highHz);

    void applyGainDecibels(double decibels, const QString& label);
    void chooseGain();
    void chooseLimiter();
    void chooseFilter();
    void applyFilter(int filterType, double frequency, double q, double gainDb,
                     const QString& label);
    /// The two dynamics processors. Both are asked for through one form
    /// rather than a chain of prompts, because a threshold without its ratio
    /// beside it is not a setting anyone can judge.
    void chooseCompressor();
    void chooseGate();

    /// Run `apply` over the selection with a run-up before it and a blend at
    /// each end, and commit the result under `label`.
    ///
    /// Shared by both because the run-up and the blend are the whole
    /// difference between a processor and an edit, and neither should be
    /// written twice.
    bool
    applyOverRange(const QString& label, double attackSeconds, double releaseSeconds,
                   const std::function<Status(AudioBufferView, SampleCount, SampleCount)>& apply);

    void chooseDeess();
    void chooseDeclick();
    void restoreClipping();
    bool removeHum();
    bool applyDeclick(double threshold, const QString& label);
    void chooseTimeStretch();
    void choosePitchShift();
    bool applyTimeStretch(double factor, const QString& label);
    bool applyPitchShift(double semitones, const QString& label);
    void limitTo(double ceilingDb);
    void normaliseToTarget();
    /// Fade the selection in or out with `shape`.
    ///
    /// The shape is a parameter rather than a member read inside, so a batch
    /// verb can name one without disturbing what the menu has selected.
    void applyFade(bool fadingIn, engine::FadeShape shape);
    void flattenRange();

    void copySelection();
    void cutSelection();
    void pasteClipboard();
    void deleteSelection(bool ripple);
    void trimToSelection();
    void undo();
    void redo();

    [[nodiscard]] TimeSelection selection() const noexcept;
    [[nodiscard]] bool hasDocument() const noexcept;

    TimeRuler* ruler_ = nullptr;
    LoudnessPanel* meters_ = nullptr;
    SpectrumView* spectrum_ = nullptr;
    QTimer* analysisTimer_ = nullptr;
    WaveformView* waveform_ = nullptr;
    SpectrogramView* spectrogram_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* readout_ = nullptr;

    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* cutAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* pasteAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* silenceAction_ = nullptr;
    QAction* trimAction_ = nullptr;
    QAction* normaliseAction_ = nullptr;

    /// Which curve Fade in and Fade out use. Linear by default: it is what
    /// people mean by a fade, and equal power is for crossfades, where two of
    /// them have to sum to a constant.
    engine::FadeShape fadeShape_ = engine::FadeShape::Linear;

    /// What an export is written as, and what is done about the bits it drops.
    ///
    /// 24-bit by default because it is the safe delivery depth and the one
    /// that needs nothing done to it. The dither setting is a standing policy
    /// and applies only to a 16-bit export, where it is the difference between
    /// a noise floor and a distortion floor; see exportTo for why it stops
    /// there.
    io::SampleFormat exportFormat_ = io::SampleFormat::PcmInt24;
    dsp::DitherType ditherType_ = dsp::DitherType::Tpdf;
    QAction* attenuateAction_ = nullptr;
    QAction* healAction_ = nullptr;
    QAction* denoiseAction_ = nullptr;
    QAction* exportSelectionAction_ = nullptr;
    QAction* clearReferenceAction_ = nullptr;

    QAction* playAction_ = nullptr;
    QTimer* playheadTimer_ = nullptr;
    std::optional<transport::Player> player_;

    engine::Document document_;
    std::optional<engine::UndoHistory> history_;
    std::shared_ptr<const engine::DocumentSource> documentSource_;
    std::shared_ptr<const io::PeakPyramid> peaks_;
    std::shared_ptr<const spectral::SpectrogramPyramid> spectra_;

    std::thread spectrogramWorker_;
    /// True from the moment a build is requested until its result has been
    /// applied or abandoned. waitForAnalysis() holds on it, which is what stops
    /// a headless run screenshotting an empty spectrogram.
    bool spectrogramBusy_ = false;
    CancellationToken spectrogramCancellation_;
    /// Bumped per request; a result from a superseded generation is dropped.
    std::shared_ptr<std::atomic<std::uint64_t>> spectrogramGeneration_ =
        std::make_shared<std::atomic<std::uint64_t>>(0);

    AudioBuffer clipboard_;
    spectral::NoiseProfile noiseProfile_;
    std::filesystem::path openedPath_;
    std::filesystem::path sessionPath_;
    QString spectrogramNote_;
};

} // namespace sa::ui
