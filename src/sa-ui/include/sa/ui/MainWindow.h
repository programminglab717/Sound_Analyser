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
#include <sa/spectral/SpectrogramTiles.h>
#include <sa/transport/Player.h>
#include <sa/ui/AnalysisPanel.h>
#include <sa/ui/Colourmap.h>
#include <sa/ui/EqCurveView.h>
#include <sa/ui/LoudnessPanel.h>
#include <sa/ui/Settings.h>
#include <sa/ui/SettingsStore.h>
#include <sa/ui/SpectrogramView.h>
#include <sa/ui/TimeRuler.h>
#include <sa/ui/WaveformView.h>

#include <QMainWindow>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

class QAction;
class QCloseEvent;
class QLabel;
class QMenu;
class QSplitter;
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
    /// Where the settings are kept.
    ///
    /// Nothing -- the usual case -- means the file chooseSettingsFile() picks.
    /// A path means that file, which is what makes persistence checkable from
    /// a driver without standing on whatever the person running it has saved.
    /// An *empty* path means no settings at all: nothing is read and nothing
    /// is written, which is what a batch run gets unless it asks otherwise.
    /// A script is not a person, and should neither inherit somebody's window
    /// position nor leave its own behind in their recent file list.
    explicit MainWindow(std::optional<std::filesystem::path> settingsFile = std::nullopt);
    ~MainWindow() override;

    /// Load an audio file, build its caches and display it. Returns false and
    /// reports on the status bar if the file cannot be opened.
    bool openFile(const std::filesystem::path& path);

    /// Save the arrangement, consolidating any audio that exists only in
    /// memory into a folder beside the session first.
    bool saveSession(const std::filesystem::path& path);

    /// Reopen a saved arrangement.
    bool openSession(const std::filesystem::path& path);

    /// Open audio or a session, deciding which by the extension.
    ///
    /// A .sa argument is an arrangement, not audio. Sniffing by extension is
    /// right here: the user chose the name, and a session is ours to define.
    /// One copy of that rule, because the command line, the recent list and
    /// anything else that is handed a path all have to make the same call.
    bool openPath(const std::filesystem::path& path);

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

    /// The EQ bands as key=value lines on stdout, in the same spirit: a drag
    /// that lands a band an octave out draws a curve that still looks like an
    /// EQ, and only the numbers say which one.
    [[nodiscard]] bool printEqBands() const;

    /// The key, the tempo, the contour and the room figures as key=value
    /// lines, together with the text the panel decided to show for each.
    ///
    /// The text as well as the numbers, because the text is where the
    /// judgement is: a driver that only saw `key_strength=0.11` could not tell
    /// a panel that refused to name a key from one that named it anyway.
    [[nodiscard]] bool printMusicalAnalysis() const;

    [[nodiscard]] bool saveScreenshot(const std::filesystem::path& path);

    /// The settings as restored and now in force, as key=value lines on
    /// stdout: the window the confining rule settled on, the splitter sizes
    /// that were accepted, every preference, and the recent list.
    ///
    /// The live values, read back out of the widgets rather than out of the
    /// file, because the claim being checked is that the file reached them.
    [[nodiscard]] bool printSettings() const;

    /// True when a stored window geometry was found and applied.
    ///
    /// The batch paths ask, because a run with nothing saved has to be a
    /// predictable size for a screenshot to be comparable, and a run that did
    /// restore something must not then be resized out from under it.
    [[nodiscard]] bool restoredWindow() const noexcept { return restoredWindow_; }

    /// Write the current state to the settings file now.
    ///
    /// Called on close, and from the destructor for the ways out that deliver
    /// no close event -- a batch run ends by returning from main, and the
    /// settings a batch run was given are the point of giving it one.
    void saveSettings();

    /// Render just the spectrogram's plotting area, with no gutter and no
    /// window chrome. A test that checks pixels needs to know what it is
    /// looking at; grabbing the whole window makes it guess where the plot
    /// starts, and that guess is what breaks the next time the layout moves.
    [[nodiscard]] bool saveSpectrogramImage(const std::filesystem::path& path);

    /// The spectrum panel alone, without its gutter. As with the spectrogram,
    /// the point is that a test can say what the pixels should be rather than
    /// only that the program did not crash.
    [[nodiscard]] bool saveSpectrumImage(const std::filesystem::path& path);

    /// The waveform's plotting area alone, for the same reason: the beat grid
    /// and the pitch contour are drawn there, and where they fall is the whole
    /// claim being made about them.
    [[nodiscard]] bool saveWaveformImage(const std::filesystem::path& path);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void chooseFile();
    void chooseSaveSession();
    void chooseOpenSession();
    void chooseExport();
    void chooseExportSelection();

private:
    void buildMenus();

    /// Read the settings file and put what it holds into the window.
    ///
    /// Called from the constructor, before the widgets exist, because the
    /// preferences decide what several of them are built with. The parts that
    /// need a widget -- the geometry, the splitters -- are applied by
    /// applySavedLayout once there is one.
    [[nodiscard]] SavedSettings loadSettings();
    void applyPreferences();
    void applySavedLayout(const SavedSettings& saved);

    /// The available area of every attached screen, in desktop coordinates.
    [[nodiscard]] std::vector<Rect> attachedScreens() const;

    /// The rectangle to save: where the window would be if it were not
    /// maximised.
    ///
    /// QWidget::normalGeometry() answers that, but only once the window has
    /// been shown as a normal window at least once. A window restored straight
    /// into a maximised state never has been, and it answers with an invalid
    /// rectangle -- which is how "maximise, quit, reopen" loses the position
    /// as well as the maximised state. So the rectangle the restore applied is
    /// kept, and used when the widget has nothing better to say.
    [[nodiscard]] Rect normalFrame() const;

    void choosePreferences();

    /// Rebuild the recent files submenu, greying out what is not there.
    void rebuildRecentMenu();

    /// Put `path` at the top of the recent list and save, so the list survives
    /// a crash as well as a quit.
    void rememberRecent(const std::filesystem::path& path);

    void setColourmap(Colourmap map);
    void setFrequencyScale(FrequencyScale scale);
    void showWaveformCursor(double seconds, double peakDecibels);
    void showSpectrogramCursor(double seconds, double hz, double decibels);
    void selectionChanged(SampleIndex start, SampleIndex end);

    /// Re-measure whatever the panel should be showing: the selection when
    /// there is one, the whole document otherwise.
    void remeasure();
    /// Re-run the key, the tempo, the bands and whichever of the contour and
    /// the room figures have been asked for.
    void remeasureMusical();

    /// Put a finished musical analysis into the views that draw it.
    void showMusicalAnalysis();

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

    /// Ask the tiled spectrogram for detail covering what is on screen.
    ///
    /// Deferred behind a timer, because the view range changes on every step of
    /// a scroll and a fetch per step would queue hundreds of builds for ranges
    /// nobody is looking at any more.
    void requestSpectrogramDetailSoon();
    void requestSpectrogramDetail();
    void cancelDetailFetch();
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

    /// Run a set of EQ bands over the selection and commit the result.
    ///
    /// The one place a biquad meets the document, shared by the Filter dialog
    /// and by the draggable curve. The run-up that settles the filters and the
    /// blend that hides the step at each end of the selection are the whole
    /// difference between filtering audio and filtering *part* of some audio,
    /// and two copies of that reasoning is how only one of them gets fixed.
    bool applyEqBands(const std::vector<dsp::EqBand>& bands, const QString& label);

    /// Show or hide the draggable curve over the spectrum.
    void setEqCurveVisible(bool visible);

    /// Apply whatever the curve is currently drawing, through undo.
    ///
    /// The bands are left up afterwards rather than cleared. Clearing them
    /// would lose a setting somebody spent time on the moment they used it,
    /// and the same curve applied to a second selection is a real thing people
    /// do; the status line says the curve is still armed so that a second
    /// press is a choice rather than a surprise.
    bool applyEqCurve();

    void resetEqCurve();
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
    AnalysisPanel* analysis_ = nullptr;
    EqCurveView* spectrum_ = nullptr;
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

    /// Everything the window used to hard-code, and now remembers.
    ///
    /// One struct rather than a member per setting, because it is what is read
    /// from the file, what the dialog edits and what is written back: three
    /// copies of the same values kept in step by hand is how one of them gets
    /// forgotten. The menus write into this directly and re-tick themselves
    /// from it, so a batch verb and a menu press leave the window in the same
    /// state -- which is the rule refreshActions() was already following for
    /// the analysis toggles.
    ///
    /// What an export is written as: 24-bit by default because it is the safe
    /// delivery depth and the one that needs nothing done to it. The dither
    /// setting is a standing policy and applies only to a 16-bit export, where
    /// it is the difference between a noise floor and a distortion floor; see
    /// exportTo for why it stops there. Fades are linear by default because it
    /// is what people mean by a fade, and equal power is for crossfades, where
    /// two of them have to sum to a constant.
    Preferences preferences_;

    /// The File menu's list, and where it is drawn.
    RecentFiles recent_;
    QMenu* recentMenu_ = nullptr;

    /// Which file the settings live in, and whether it is the portable one.
    SettingsFile settingsFile_;

    /// False once a file written by a newer build has been read. See
    /// SavedSettings::writeBack: this build then reads and does not write.
    bool mayWriteSettings_ = true;

    /// Set when a stored geometry was applied, and when the settings have
    /// already been written by a close, so the destructor does not write them
    /// a second time.
    bool restoredWindow_ = false;
    bool settingsWritten_ = false;

    /// The layout as it was restored: the rectangle the confining rule
    /// settled on, whether it was maximised, and the two splitters' stored
    /// sizes.
    ///
    /// Kept because a run that never shows the window still saves -- opening a
    /// file from the command line saves the recent list, and that writes the
    /// whole file. Widgets that have never been laid out answer with nonsense
    /// when asked how big they are, and writing that nonsense back is how one
    /// launch that was killed before it drew anything loses the layout of
    /// every launch before it.
    std::optional<Rect> restoredFrame_;
    bool restoredMaximised_ = false;
    std::vector<int> loadedMainSplit_;
    std::vector<int> loadedSideSplit_;

    /// The two splitters, kept so their sizes can be saved and restored. The
    /// main one holds the waveform over the spectrogram; the side one holds
    /// the meters, the analysis panel and the spectrum.
    QSplitter* splitter_ = nullptr;
    QSplitter* sideSplitter_ = nullptr;

    /// Menu entries that tick according to a preference, with the question
    /// each one answers.
    ///
    /// The radio groups -- export format, dither, fade shape, frequency scale,
    /// dynamic range, colour map -- are ticked from here rather than each
    /// keeping its own state, so that a preference changed in the dialog
    /// re-ticks the menu that shows the same thing. Two controls for one
    /// setting is fine; two answers is not.
    std::vector<std::pair<QAction*, std::function<bool()>>> preferenceTicks_;

    QAction* attenuateAction_ = nullptr;
    QAction* healAction_ = nullptr;
    QAction* denoiseAction_ = nullptr;
    QAction* exportSelectionAction_ = nullptr;
    QAction* clearReferenceAction_ = nullptr;
    QAction* showEqAction_ = nullptr;
    QAction* applyEqAction_ = nullptr;
    QAction* resetEqAction_ = nullptr;

    /// The four analysis overlays, and whether each is wanted.
    ///
    /// The beat grid starts on because it is the evidence for a number the
    /// panel is already showing, and a tempo without it has to be taken on
    /// trust. The other three start off: bands are a second reading of the
    /// spectrum and would crowd it unasked, a contour is expensive and means
    /// one note at a time, and room acoustics are an answer about an impulse
    /// response and nonsense about anything else -- so the user says which of
    /// those three they meant. All four are remembered; the flags live in
    /// preferences_.
    QAction* beatGridAction_ = nullptr;
    QAction* pitchContourAction_ = nullptr;
    QAction* octaveBandsAction_ = nullptr;
    QAction* roomAction_ = nullptr;

    QAction* playAction_ = nullptr;
    QTimer* playheadTimer_ = nullptr;
    std::optional<transport::Player> player_;

    engine::Document document_;
    std::optional<engine::UndoHistory> history_;
    std::shared_ptr<const engine::DocumentSource> documentSource_;
    std::shared_ptr<const io::PeakPyramid> peaks_;
    /// The spectrogram, as a tiled cache rather than a whole pyramid.
    ///
    /// Non-const because the window is what asks it for detail as the view
    /// moves; the view is handed a const pointer to the same object and only
    /// reads. Both are safe concurrently -- see SpectrogramTiles.
    std::shared_ptr<spectral::SpectrogramTiles> spectra_;

    /// Fetches detail for wherever the view has scrolled to.
    std::thread detailWorker_;
    CancellationToken detailCancellation_;
    QTimer* detailTimer_ = nullptr;

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
