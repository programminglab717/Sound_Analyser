#include <sa/device/AudioDeviceManager.h>
#include <sa/device/NullAudioDevice.h>
#include <sa/dsp/Dynamics.h>
#include <sa/dsp/OfflineLimiter.h>
#include <sa/dsp/ParametricEq.h>
#include <sa/dsp/StereoLink.h>
#include <sa/dsp/TimeStretch.h>
#include <sa/engine/BufferSource.h>
#include <sa/engine/Consolidate.h>
#include <sa/engine/Edits.h>
#include <sa/engine/SessionFile.h>
#include <sa/io/AudioFile.h>
#include <sa/io/WavWriter.h>
#include <sa/spectral/SpectralEdit.h>
#include <sa/ui/MainWindow.h>
#include <sa/ui/ViewGeometry.h>

#include <QActionGroup>
#include <QApplication>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>

namespace sa::ui {

namespace {

/// Ceiling on the spectrogram cache itself.
///
/// The build streams now, so the decoded audio is no longer the constraint --
/// what is left is the pyramid, which is the picture and cannot be smaller
/// without being a worse picture. At the display settings below it costs about
/// four bytes per frame of the document, so this allows roughly an hour and a
/// half of stereo at 48 kHz.
///
/// Past that the waveform is still drawn and everything else still works; only
/// the spectrogram is withheld, and the status bar says so with the number. The
/// real fix is generating tiles on demand and evicting them, which is tracked
/// in docs/07-autonomous-queue.md.
constexpr std::size_t kMaximumPyramidBytes = 1'200'000'000;

/// Display analysis settings. 4096 at 48 kHz is an 11.7 Hz bin and a 21 ms hop.
/// A log axis stretches the bottom two octaves over half the display, and 2048
/// gives them four bins to fill it with; this gives them eight, at a time
/// resolution transients still survive.
[[nodiscard]] spectral::SpectrogramConfig displayConfig() {
    spectral::SpectrogramConfig config;
    config.fftSize = 4096;
    config.hopSize = 1024;
    return config;
}

/// The loudest sample in a buffer, as an absolute value.
[[nodiscard]] double peakOf(const AudioBuffer& audio) noexcept {
    double peak = 0.0;
    for (int channel = 0; channel < audio.channelCount(); ++channel) {
        const float* samples = audio.channel(channel);
        for (SampleCount i = 0; i < audio.frames(); ++i) {
            peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
        }
    }
    return peak;
}

} // namespace

MainWindow::MainWindow() {
    setWindowTitle(tr("Sound Analyser"));
    resize(1280, 760);

    auto* central = new QWidget{this};
    auto* column = new QVBoxLayout{central};
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    ruler_ = new TimeRuler{central};

    auto* splitter = new QSplitter{Qt::Vertical, central};
    waveform_ = new WaveformView{splitter};
    spectrogram_ = new SpectrogramView{splitter};
    splitter->addWidget(waveform_);
    splitter->addWidget(spectrogram_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);

    column->addWidget(ruler_);
    column->addWidget(splitter, 1);

    // The meters sit beside the views rather than in a floating window: the
    // numbers and the picture are answers to the same question, and a reader
    // who has to move a window to see both will stop looking at one of them.
    meters_ = new LoudnessPanel{this};
    auto* row = new QWidget{this};
    auto* across = new QHBoxLayout{row};
    across->setContentsMargins(0, 0, 0, 0);
    across->setSpacing(0);
    across->addWidget(central, 1);
    across->addWidget(meters_);
    setCentralWidget(row);

    // One time axis and one selection. Either view can drive them; the other
    // and the ruler follow.
    connect(waveform_, &TimeAxisView::viewRangeChanged, spectrogram_, &TimeAxisView::setViewRange);
    connect(spectrogram_, &TimeAxisView::viewRangeChanged, waveform_, &TimeAxisView::setViewRange);
    connect(waveform_, &TimeAxisView::viewRangeChanged, ruler_, &TimeRuler::setViewRange);
    connect(spectrogram_, &TimeAxisView::viewRangeChanged, ruler_, &TimeRuler::setViewRange);

    connect(waveform_, &TimeAxisView::selectionChanged, this, &MainWindow::selectionChanged);
    connect(spectrogram_, &TimeAxisView::selectionChanged, this, &MainWindow::selectionChanged);

    connect(waveform_, &WaveformView::cursorMoved, this, &MainWindow::showWaveformCursor);
    connect(spectrogram_, &SpectrogramView::cursorMoved, this, &MainWindow::showSpectrogramCursor);

    status_ = new QLabel{tr("Open an audio file to begin"), this};
    readout_ = new QLabel{this};
    readout_->setMinimumWidth(340);
    readout_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addWidget(status_, 1);
    statusBar()->addPermanentWidget(readout_);

    // 30 Hz: fast enough that the playhead looks continuous, slow enough that
    // it costs nothing. The position it reads is frames the callback has
    // actually played, not frames queued, so it does not run ahead of the
    // sound.
    playheadTimer_ = new QTimer{this};
    playheadTimer_->setInterval(33);
    connect(playheadTimer_, &QTimer::timeout, this, &MainWindow::followPlayhead);

    buildMenus();
    refreshActions();

    setStyleSheet("QMainWindow { background: #101015; }"
                  "QWidget { background: #101015; }"
                  "QMenuBar { background: #16171d; color: #c8ccd8; }"
                  "QMenuBar::item:selected { background: #2a2c38; }"
                  "QMenu { background: #16171d; color: #c8ccd8; }"
                  "QMenu::item:selected { background: #2a2c38; }"
                  "QMenu::item:disabled { color: #4a4e5e; }"
                  "QStatusBar { background: #16171d; color: #8a8fa0; }"
                  "QStatusBar::item { border: none; }"
                  "QLabel { color: #8a8fa0; }"
                  "QSplitter::handle { background: #2a2c38; }");
}

MainWindow::~MainWindow() {
    // The worker holds a pointer to this window's cancellation token and posts
    // back to this object. Joining here is what makes both safe; a detached
    // worker would outlive the thing it reports to.
    cancelSpectrogramBuild();
    stopPlayback();
}

bool MainWindow::hasDocument() const noexcept {
    return document_.duration() > 0;
}

TimeSelection MainWindow::selection() const noexcept {
    return waveform_->selection();
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open audio…"), QKeySequence::Open, this, &MainWindow::chooseFile);
    file->addSeparator();
    file->addAction(tr("Open &session…"), QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_O}, this,
                    &MainWindow::chooseOpenSession);
    file->addAction(tr("&Save session…"), QKeySequence::Save, this, &MainWindow::chooseSaveSession);
    file->addSeparator();
    file->addAction(tr("&Export…"), QKeySequence::SaveAs, this, &MainWindow::chooseExport);
    exportSelectionAction_ =
        file->addAction(tr("Export &selection…"), this, &MainWindow::chooseExportSelection);
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    undoAction_ = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, &MainWindow::undo);
    redoAction_ = edit->addAction(tr("&Redo"), QKeySequence::Redo, this, &MainWindow::redo);
    edit->addSeparator();
    cutAction_ = edit->addAction(tr("Cu&t"), QKeySequence::Cut, this, &MainWindow::cutSelection);
    copyAction_ =
        edit->addAction(tr("&Copy"), QKeySequence::Copy, this, &MainWindow::copySelection);
    pasteAction_ =
        edit->addAction(tr("&Paste"), QKeySequence::Paste, this, &MainWindow::pasteClipboard);
    deleteAction_ = edit->addAction(tr("&Delete"), QKeySequence::Delete, this,
                                    [this] { deleteSelection(true); });
    // Silencing keeps timing; deleting closes the gap. Both are wanted, by
    // different people, often in the same session -- a music editor cannot have
    // the bar move, a dialogue editor wants the pause gone.
    silenceAction_ = edit->addAction(tr("&Silence selection"), QKeySequence{Qt::CTRL | Qt::Key_L},
                                     this, [this] { deleteSelection(false); });
    trimAction_ = edit->addAction(tr("T&rim to selection"), QKeySequence{Qt::CTRL | Qt::Key_T},
                                  this, &MainWindow::trimToSelection);
    edit->addSeparator();
    edit->addAction(tr("Select &all"), QKeySequence::SelectAll, this, [this] {
        const TimeSelection all{0, document_.duration()};
        waveform_->setSelection(all);
        spectrogram_->setSelection(all);
        ruler_->setSelection(all);
        refreshActions();
        updateStatus();
    });
    edit->addAction(tr("Deselect"), QKeySequence{Qt::Key_Escape}, this, [this] {
        const TimeSelection none{selection().start, selection().start};
        waveform_->setSelection(none);
        spectrogram_->setSelection(none);
        ruler_->setSelection(none);
        refreshActions();
        updateStatus();
    });

    QMenu* process = menuBar()->addMenu(tr("&Process"));
    process->addAction(tr("&Gain…"), QKeySequence{Qt::CTRL | Qt::Key_G}, this,
                       &MainWindow::chooseGain);
    process->addAction(tr("&Filter…"), QKeySequence{Qt::CTRL | Qt::Key_F}, this,
                       &MainWindow::chooseFilter);
    process->addAction(tr("&Limiter…"), QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_L}, this,
                       &MainWindow::chooseLimiter);
    process->addAction(tr("&Time stretch…"), this, &MainWindow::chooseTimeStretch);
    process->addAction(tr("&Pitch shift…"), this, &MainWindow::choosePitchShift);
    normaliseAction_ =
        process->addAction(tr("&Normalise to target"), QKeySequence{Qt::CTRL | Qt::Key_N}, this,
                           &MainWindow::normaliseToTarget);
    process->addSeparator();
    process->addAction(tr("Fade &in"), this, [this] { applyFade(true); });
    process->addAction(tr("Fade &out"), this, [this] { applyFade(false); });
    process->addSeparator();
    process->addAction(tr("F&latten"), this, &MainWindow::flattenRange);

    QMenu* transport = menuBar()->addMenu(tr("&Transport"));
    playAction_ = transport->addAction(tr("&Play"), QKeySequence{Qt::Key_Space}, this,
                                       &MainWindow::togglePlayback);
    transport->addAction(tr("&Stop"), QKeySequence{Qt::Key_Escape | Qt::SHIFT}, this,
                         &MainWindow::stopPlayback);

    QMenu* repair = menuBar()->addMenu(tr("&Repair"));
    attenuateAction_ =
        repair->addAction(tr("&Attenuate selection…"), QKeySequence{Qt::CTRL | Qt::Key_R}, this,
                          &MainWindow::chooseAttenuate);
    healAction_ = repair->addAction(tr("&Heal selection"), QKeySequence{Qt::CTRL | Qt::Key_H}, this,
                                    &MainWindow::healSelection);
    repair->addSeparator();
    repair->addAction(tr("&Learn noise profile from selection"),
                      QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_N}, this,
                      &MainWindow::learnNoiseProfile);
    denoiseAction_ = repair->addAction(tr("Reduce &noise…"), QKeySequence{Qt::CTRL | Qt::Key_D},
                                       this, &MainWindow::chooseDenoise);
    repair->addSeparator();
    repair->addAction(tr("Select all &frequencies"), this,
                      [this] { selectFrequencyBand(0.0, document_.sampleRate().hz() * 0.5); });

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Zoom to &fit"), QKeySequence{Qt::Key_F}, this,
                    [this] { waveform_->showAll(); });
    view->addAction(tr("Zoom to se&lection"), QKeySequence{Qt::CTRL | Qt::Key_E}, this,
                    [this] { waveform_->zoomToSelection(); });
    view->addSeparator();

    QMenu* scales = view->addMenu(tr("&Frequency scale"));
    auto* scaleGroup = new QActionGroup{this};
    const auto addScale = [&](const QString& label, FrequencyScale scale, bool checked) {
        QAction* action =
            scales->addAction(label, this, [this, scale] { setFrequencyScale(scale); });
        action->setCheckable(true);
        action->setChecked(checked);
        scaleGroup->addAction(action);
    };
    addScale(tr("&Logarithmic"), FrequencyScale::Logarithmic, true);
    addScale(tr("Li&near"), FrequencyScale::Linear, false);

    QMenu* range = view->addMenu(tr("&Dynamic range"));
    auto* rangeGroup = new QActionGroup{this};
    const auto addRange = [&](float floorDb, bool checked) {
        QAction* action =
            range->addAction(tr("%1 dB").arg(static_cast<int>(floorDb)), this,
                             [this, floorDb] { spectrogram_->setFloorDecibels(floorDb); });
        action->setCheckable(true);
        action->setChecked(checked);
        rangeGroup->addAction(action);
    };
    addRange(-60.0f, false);
    addRange(-80.0f, false);
    addRange(-96.0f, true);
    addRange(-120.0f, false);

    QMenu* colours = view->addMenu(tr("&Colour map"));
    auto* group = new QActionGroup{this};
    const auto addMap = [&](const QString& label, Colourmap map, bool checked) {
        QAction* action = colours->addAction(label, this, [this, map] { setColourmap(map); });
        action->setCheckable(true);
        action->setChecked(checked);
        group->addAction(action);
    };
    addMap(tr("Magma"), Colourmap::Magma, true);
    addMap(tr("Viridis"), Colourmap::Viridis, false);
    addMap(tr("Greyscale"), Colourmap::Grey, false);
}

void MainWindow::setColourmap(Colourmap map) {
    spectrogram_->setColourmap(map);
}

void MainWindow::setFrequencyScale(FrequencyScale scale) {
    spectrogram_->setFrequencyScale(scale);
}

void MainWindow::selectionChanged(SampleIndex start, SampleIndex end) {
    const TimeSelection selected{start, end};
    // setSelection is idempotent and does not re-emit, so echoing between the
    // views terminates rather than looping.
    waveform_->setSelection(selected);
    spectrogram_->setSelection(selected);
    ruler_->setSelection(selected);
    refreshActions();
    updateStatus();
    remeasure();
}

void MainWindow::remeasure() {
    if (!documentSource_ || document_.duration() <= 0) {
        meters_->clear();
        return;
    }
    const TimeSelection selected = selection();
    if (selected.isEmpty()) {
        meters_->measure(documentSource_, 0, document_.duration(), tr("whole document"));
    } else {
        meters_->measure(documentSource_, selected.start, selected.length(), tr("selection"));
    }
}

void MainWindow::showWaveformCursor(double seconds, double peakDecibels) {
    if (seconds < 0.0) {
        readout_->clear();
        return;
    }
    readout_->setText(tr("%1   peak %2 dBFS")
                          .arg(QString::fromStdString(formatTime(seconds, 60.0)))
                          .arg(peakDecibels, 0, 'f', 1));
}

void MainWindow::showSpectrogramCursor(double seconds, double hz, double decibels) {
    if (hz < 0.0) {
        readout_->clear();
        return;
    }
    readout_->setText(tr("%1   %2 Hz   %3 dB")
                          .arg(QString::fromStdString(formatTime(seconds, 60.0)))
                          .arg(QString::fromStdString(formatFrequency(hz)))
                          .arg(decibels, 0, 'f', 1));
}

void MainWindow::chooseFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open audio"), {},
        tr("Audio files (*.wav *.aif *.aiff *.aifc *.flac *.mp3);;All files (*)"));
    if (!path.isEmpty()) {
        openFile(path.toStdString());
    }
}

void MainWindow::chooseSaveSession() {
    if (!hasDocument()) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Save session"),
                                                      QString::fromStdString(sessionPath_.string()),
                                                      tr("Sound Analyser session (*.sa)"));
    if (!path.isEmpty()) {
        saveSession(path.toStdString());
    }
}

void MainWindow::chooseOpenSession() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Open session"), {},
                                                      tr("Sound Analyser session (*.sa)"));
    if (!path.isEmpty()) {
        openSession(path.toStdString());
    }
}

bool MainWindow::saveSession(const std::filesystem::path& path) {
    if (!hasDocument()) {
        return false;
    }
    stopPlayback();

    // Audio generated while editing -- a paste, a flatten, a repair -- exists
    // only in memory. A session records sources by path, so without this the
    // arrangement reopens intact and silent, which looks like it worked.
    const auto consolidated = engine::consolidateSources(document_, path);
    if (!consolidated.failures.empty()) {
        status_->setText(tr("Could not save all the audio: %1")
                             .arg(QString::fromStdString(consolidated.failures.front())));
        return false;
    }

    if (auto status = engine::saveSession(document_, path); !status) {
        status_->setText(tr("Could not save %1: %2")
                             .arg(QString::fromStdString(path.filename().string()),
                                  QString::fromStdString(std::string{status.error().what()})));
        return false;
    }

    sessionPath_ = path;
    setWindowTitle(tr("%1 — Sound Analyser").arg(QString::fromStdString(path.filename().string())));
    status_->setText(consolidated.written.empty()
                         ? tr("Saved %1").arg(QString::fromStdString(path.filename().string()))
                         : tr("Saved %1 with %2 consolidated file(s)")
                               .arg(QString::fromStdString(path.filename().string()))
                               .arg(consolidated.written.size()));
    return true;
}

bool MainWindow::openSession(const std::filesystem::path& path) {
    stopPlayback();

    engine::FileSourceResolver resolver;
    auto loaded = engine::loadSession(path, resolver);
    if (!loaded) {
        status_->setText(tr("Could not open %1: %2")
                             .arg(QString::fromStdString(path.filename().string()),
                                  QString::fromStdString(std::string{loaded.error().what()})));
        return false;
    }

    // Only now, once the session is known to have loaded: a failed open should
    // leave the analysis of whatever is already open alone.
    cancelSpectrogramBuild();

    document_ = std::move(loaded).value().document;
    const auto& details = loaded.value();
    sessionPath_ = path;
    openedPath_ = path;
    sessionPath_.clear();
    history_.emplace(document_);
    rebuildCaches();
    refreshViews();
    waveform_->showAll();

    // A missing file is reported, not hidden. Its clips are still there backed
    // by silence, so the arrangement survives and the user can relink -- but
    // they have to know, or they will mix a session with a hole in it.
    if (!details.missingSources.empty()) {
        status_->setText(tr("%1 opened, but %2 referenced file(s) are missing -- their clips "
                            "are silent")
                             .arg(QString::fromStdString(path.filename().string()))
                             .arg(details.missingSources.size()));
    }
    setWindowTitle(tr("%1 — Sound Analyser").arg(QString::fromStdString(path.filename().string())));
    return true;
}

void MainWindow::chooseExport() {
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Export"), {}, tr("WAV audio (*.wav)"));
    if (!path.isEmpty()) {
        exportTo(path.toStdString(), false);
    }
}

void MainWindow::chooseExportSelection() {
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Export selection"), {}, tr("WAV audio (*.wav)"));
    if (!path.isEmpty()) {
        exportTo(path.toStdString(), true);
    }
}

bool MainWindow::openFile(const std::filesystem::path& path) {
    const QString name = QString::fromStdString(path.filename().string());

    auto opened = io::openAudioFile(path);
    if (!opened) {
        status_->setText(
            tr("Could not open %1: %2")
                .arg(name, QString::fromStdString(std::string{opened.error().what()})));
        return false;
    }

    const auto audio = opened.value();
    const io::AudioFileInfo& info = audio->info();

    // Only now, once the file is known to be readable: a failed open should
    // leave whatever was already loaded playing and drawing.
    stopPlayback();
    cancelSpectrogramBuild();

    document_ = engine::Document{info.sampleRate, info.layout};
    auto source = document_.addSource(audio, path.filename().string(), path);
    if (!source || !document_.appendSource(source.value(), 0)) {
        status_->setText(tr("Could not place %1 on the timeline").arg(name));
        return false;
    }

    openedPath_ = path;
    sessionPath_.clear();
    history_.emplace(document_);
    rebuildCaches();
    refreshViews();
    waveform_->showAll();
    setWindowTitle(tr("%1 — Sound Analyser").arg(name));
    return true;
}

void MainWindow::rebuildCaches() {
    documentSource_ = std::make_shared<const engine::DocumentSource>(document_);
    peaks_.reset();
    spectra_.reset();
    spectrogramNote_.clear();

    if (document_.duration() <= 0) {
        return;
    }

    // Both caches read the document rather than the file, so an edit is
    // reflected everywhere the moment it lands.
    if (auto peaks = io::PeakPyramid::buildStreaming(*documentSource_)) {
        peaks_ = std::make_shared<const io::PeakPyramid>(std::move(peaks).value());
    } else {
        spectrogramNote_ = tr("waveform analysis failed");
        return;
    }

    // What the cache will cost, before paying for it: one byte per bin per
    // frame at level 0, and the levels above it are a geometric series that
    // roughly doubles that.
    const auto config = displayConfig();
    const auto frames = static_cast<std::size_t>(document_.duration() / config.hopSize + 1);
    const auto bins = static_cast<std::size_t>(config.fftSize / 2 + 1);
    const std::size_t pyramidBytes = frames * bins * 2;

    if (pyramidBytes > kMaximumPyramidBytes) {
        spectrogramNote_ = tr("too long for a spectrogram in this build (it would need %1 GB); "
                              "the waveform and the meters are unaffected")
                               .arg(static_cast<double>(pyramidBytes) / 1e9, 0, 'f', 1);
        return;
    }

    startSpectrogramBuild();
}

void MainWindow::cancelSpectrogramBuild() {
    spectrogramCancellation_.cancel();
    spectrogramGeneration_->fetch_add(1);
    if (spectrogramWorker_.joinable()) {
        spectrogramWorker_.join();
    }
    spectrogramCancellation_.reset();
    spectrogramBusy_ = false;
}

void MainWindow::startSpectrogramBuild() {
    cancelSpectrogramBuild();
    if (!documentSource_ || document_.duration() <= 0) {
        return;
    }

    const std::uint64_t mine = spectrogramGeneration_->load();
    spectrogramBusy_ = true;
    spectrogramNote_ = tr("building the spectrogram…");

    // The source is captured by shared_ptr and the token by pointer into this
    // window, which outlives the worker because cancelSpectrogramBuild joins it
    // before anything replaces either.
    spectrogramWorker_ = std::thread{[this, source = documentSource_, config = displayConfig(),
                                      mine, generation = spectrogramGeneration_] {
        JobMonitor monitor;
        monitor.cancellation = &spectrogramCancellation_;

        auto built = spectral::SpectrogramPyramid::buildStreaming(*source, 0, config, monitor);
        if (generation->load() != mine) {
            return;
        }

        QMetaObject::invokeMethod(
            this,
            [this, built = std::make_shared<Result<spectral::SpectrogramPyramid>>(std::move(built)),
             mine, generation] {
                if (generation->load() != mine) {
                    return;
                }
                if (*built) {
                    spectra_ = std::make_shared<const spectral::SpectrogramPyramid>(
                        std::move(*built).value());
                    spectrogramNote_.clear();
                } else {
                    spectrogramNote_ =
                        tr("spectrogram analysis failed: %1")
                            .arg(QString::fromStdString(std::string{built->error().what()}));
                }
                spectrogramBusy_ = false;
                spectrogram_->setPyramid(spectra_, document_.sampleRate(), document_.duration());
                spectrogram_->setViewRange(waveform_->viewStart(), waveform_->viewLength());
                spectrogram_->setSelection(selection());
                updateStatus();
            },
            Qt::QueuedConnection);
    }};
}

void MainWindow::refreshViews() {
    const TimeSelection previous = selection();

    ruler_->setSampleRate(document_.sampleRate());
    waveform_->setPyramid(peaks_, document_.sampleRate());
    spectrogram_->setPyramid(spectra_, document_.sampleRate(), document_.duration());

    // setPyramid resets the view to the whole document, which is right on open
    // and wrong after an edit. Put the range and the selection back.
    const TimeSelection clamped{std::min(previous.start, document_.duration()),
                                std::min(previous.end, document_.duration())};
    waveform_->setSelection(clamped);
    spectrogram_->setSelection(clamped);
    ruler_->setSelection(clamped);
    ruler_->setViewRange(waveform_->viewStart(), waveform_->viewLength());
    spectrogram_->setViewRange(waveform_->viewStart(), waveform_->viewLength());

    refreshActions();
    updateStatus();
    remeasure();
}

void MainWindow::refreshActions() {
    const bool document = hasDocument();
    const bool selected = document && !selection().isEmpty();

    undoAction_->setEnabled(history_ && history_->canUndo());
    redoAction_->setEnabled(history_ && history_->canRedo());
    undoAction_->setText(
        history_ && history_->canUndo()
            ? tr("&Undo %1").arg(QString::fromStdString(std::string{history_->undoLabel()}))
            : tr("&Undo"));
    redoAction_->setText(
        history_ && history_->canRedo()
            ? tr("&Redo %1").arg(QString::fromStdString(std::string{history_->redoLabel()}))
            : tr("&Redo"));

    cutAction_->setEnabled(selected);
    copyAction_->setEnabled(selected);
    deleteAction_->setEnabled(selected);
    silenceAction_->setEnabled(selected);
    trimAction_->setEnabled(selected);
    exportSelectionAction_->setEnabled(selected);
    pasteAction_->setEnabled(document && clipboard_.frames() > 0);
    normaliseAction_->setEnabled(document && meters_->conformGainDb().has_value());
    attenuateAction_->setEnabled(document);
    healAction_->setEnabled(document);
    denoiseAction_->setEnabled(document && !noiseProfile_.isEmpty());
}

void MainWindow::updateStatus() {
    if (!hasDocument()) {
        status_->setText(tr("Open an audio file to begin"));
        return;
    }

    const double seconds = samplesToSeconds(document_.duration(), document_.sampleRate());
    QString text = tr("%1  ·  %2 Hz  ·  %3 ch  ·  %4")
                       .arg(QString::fromStdString(openedPath_.filename().string()))
                       .arg(static_cast<int>(document_.sampleRate().hz()))
                       .arg(document_.layout().count())
                       .arg(QString::fromStdString(formatTime(seconds, 60.0)));

    const TimeSelection selected = selection();
    if (!selected.isEmpty()) {
        const double from = samplesToSeconds(selected.start, document_.sampleRate());
        const double to = samplesToSeconds(selected.end, document_.sampleRate());
        const double length = samplesToSeconds(selected.length(), document_.sampleRate());
        // Both edges and the span. A start rounded to the second reads as a
        // different edit from the one the user made, and the span is the number
        // they are usually after.
        text += tr("   ·   %1 – %2   (%3 s)")
                    .arg(QString::fromStdString(formatTime(from, 20.0)),
                         QString::fromStdString(formatTime(to, 20.0)))
                    .arg(length, 0, 'f', 3);
    }
    if (!spectrogramNote_.isEmpty()) {
        text += tr("   ·   %1").arg(spectrogramNote_);
    }
    status_->setText(text);
}

template <typename Edit>
bool MainWindow::applyEdit(const QString& label, Edit&& edit) {
    if (!hasDocument() || !history_) {
        return false;
    }
    // Before the edit, not after it. The reader each of these owns is looking
    // at audio that is about to stop being what the user is editing, and a
    // player that goes on playing the old version through an edit is wrong even
    // where it is safe. (It is now safe either way -- DocumentSource keeps its
    // own copy -- but that is a floor under this, not a substitute for it.)
    stopPlayback();
    cancelSpectrogramBuild();

    if (!edit()) {
        status_->setText(tr("%1 did not apply").arg(label));
        // Nothing changed, so the analysis that was stopped for it is still
        // the right analysis. Put it back rather than leaving the window
        // looking like the edit did something.
        startSpectrogramBuild();
        return false;
    }
    history_->commit(document_, label.toStdString());
    rebuildCaches();
    refreshViews();
    return true;
}

TimeSelection MainWindow::targetRange() const noexcept {
    const TimeSelection selected = selection();
    return selected.isEmpty() ? TimeSelection{0, document_.duration()} : selected;
}

void MainWindow::togglePlayback() {
    if (player_ && player_->isPlaying()) {
        stopPlayback();
        return;
    }
    if (!hasDocument() || !documentSource_) {
        return;
    }

    // The device is opened on first use rather than at startup: a tool that
    // grabs the sound card the moment it launches is a tool people close before
    // using anything else.
    if (!player_) {
        device::AudioDeviceConfig config;
        config.sampleRate = document_.sampleRate();
        config.outputChannels = document_.layout().count();
        config.inputChannels = 0;

        // openOrFallback rather than openDefault: a default endpoint that is
        // listed but cannot be opened -- held in exclusive mode, or a driver
        // that has gone away -- should cost the user the next device in the
        // list, not playback altogether.
        device::AudioDeviceManager manager;
        auto opened = manager.openOrFallback("", config);
        if (!opened) {
            status_->setText(tr("No audio output: %1")
                                 .arg(QString::fromStdString(std::string{opened.error().what()})));
            return;
        }
        // The last resort in that chain is a device that plays to nothing.
        // Using it without saying so would move the playhead and make no sound,
        // which is a worse answer than an honest one.
        if (opened.value()->description().id == device::NullAudioBackend::kDeviceId) {
            status_->setText(tr("No sound card was available -- playback will run silently"));
        }
        auto created = transport::Player::create(std::move(opened).value());
        if (!created) {
            status_->setText(tr("Could not start playback: %1")
                                 .arg(QString::fromStdString(std::string{created.error().what()})));
            return;
        }
        player_.emplace(std::move(created).value());
    }

    const TimeSelection selected = selection();
    const SampleIndex from = selected.isEmpty() ? selected.start : selected.start;
    const SampleIndex to = selected.isEmpty() ? document_.duration() : selected.end;
    if (to <= from) {
        return;
    }

    if (auto status = player_->play(documentSource_, from, to); !status) {
        status_->setText(tr("Could not play: %1")
                             .arg(QString::fromStdString(std::string{status.error().what()})));
        return;
    }
    playAction_->setText(tr("&Stop"));
    playheadTimer_->start();
}

void MainWindow::stopPlayback() {
    if (player_) {
        player_->stop();
    }
    playheadTimer_->stop();
    playAction_->setText(tr("&Play"));
    for (TimeAxisView* view :
         {static_cast<TimeAxisView*>(waveform_), static_cast<TimeAxisView*>(spectrogram_)}) {
        view->setPlayhead(-1);
    }
    ruler_->setPlayhead(-1);
}

void MainWindow::followPlayhead() {
    if (!player_) {
        return;
    }
    const SampleIndex position = player_->position();
    waveform_->setPlayhead(position);
    spectrogram_->setPlayhead(position);
    ruler_->setPlayhead(position);

    const TimeSelection selected = selection();
    const SampleIndex end = selected.isEmpty() ? document_.duration() : selected.end;
    if (position >= end) {
        stopPlayback();
    }
}

void MainWindow::selectFrequencyBand(double lowHz, double highHz) {
    spectrogram_->setFrequencySelection(lowHz, highHz);
    updateStatus();
}

template <typename Edit>
void MainWindow::applySpectralEdit(const QString& label, Edit&& edit) {
    const TimeSelection range = targetRange();
    if (range.isEmpty() || !documentSource_) {
        return;
    }

    // Two analysis windows either side: the edit itself reaches one window past
    // the region by construction, and the second gives the reconstruction room
    // to settle before the audio is spliced back. Writing back only the
    // selection would clip that spread into a click at each seam.
    const SampleCount kContext = 2 * spectral::SpectralEditSettings{}.fftSize;
    const SampleIndex spanStart = std::max<SampleIndex>(0, range.start - kContext);
    const SampleIndex spanEnd = std::min<SampleIndex>(document_.duration(), range.end + kContext);

    AudioBuffer span{document_.layout(), spanEnd - spanStart};
    if (!documentSource_->read(spanStart, span.view())) {
        status_->setText(tr("Could not read the selection"));
        return;
    }

    spectral::SpectralRegion region;
    region.startSample = range.start - spanStart;
    region.endSample = range.end - spanStart;
    region.lowHz = spectrogram_->selectionLowHz();
    region.highHz = spectrogram_->selectionHighHz();

    const auto status = edit(span.view(), region, document_.sampleRate());
    if (!status) {
        status_->setText(
            tr("%1 failed: %2")
                .arg(label, QString::fromStdString(std::string{status.error().what()})));
        return;
    }

    (void)applyEdit(label, [this, spanStart, &span] {
        return engine::replaceRange(document_, spanStart, std::move(span)).ok();
    });
}

void MainWindow::learnNoiseProfile() {
    const TimeSelection selected = selection();
    if (selected.isEmpty() || !documentSource_) {
        status_->setText(tr("Select a passage of noise on its own, then learn from it"));
        return;
    }

    AudioBuffer passage{document_.layout(), selected.length()};
    if (!documentSource_->read(selected.start, passage.view())) {
        status_->setText(tr("Could not read the selection"));
        return;
    }

    auto learned = spectral::NoiseProfile::learn(passage.constView(), document_.sampleRate(), 0,
                                                 passage.frames());
    if (!learned) {
        status_->setText(tr("Could not learn a profile: %1")
                             .arg(QString::fromStdString(std::string{learned.error().what()})));
        return;
    }

    noiseProfile_ = std::move(learned).value();
    refreshActions();
    status_->setText(
        tr("Learned a noise profile from %1 s -- now select what to clean and reduce noise")
            .arg(samplesToSeconds(selected.length(), document_.sampleRate()), 0, 'f', 2));
}

void MainWindow::chooseDenoise() {
    if (noiseProfile_.isEmpty()) {
        status_->setText(tr("Learn a noise profile first, from a passage of noise on its own"));
        return;
    }
    bool accepted = false;
    const double decibels =
        QInputDialog::getDouble(this, tr("Reduce noise"), tr("Reduce the noise floor by (dB):"),
                                12.0, 1.0, 48.0, 1, &accepted);
    if (!accepted) {
        return;
    }

    applySpectralEdit(tr("reduce noise %1 dB").arg(decibels, 0, 'f', 0),
                      [this, decibels](AudioBufferView audio,
                                       const spectral::SpectralRegion& region, SampleRate rate) {
                          spectral::DenoiseSettings settings;
                          settings.reductionDb = decibels;
                          // The whole span is cleaned rather than just the
                          // region's frequency band: a noise profile describes
                          // the spectrum, and applying it to a slice of that
                          // spectrum leaves the rest of the hiss in place with
                          // a step where the band ended.
                          return spectral::denoise(audio, rate, noiseProfile_, region.startSample,
                                                   region.endSample, settings);
                      });
}

void MainWindow::chooseAttenuate() {
    if (!hasDocument()) {
        return;
    }
    bool accepted = false;
    const double decibels =
        QInputDialog::getDouble(this, tr("Attenuate"), tr("Reduce the selected region by (dB):"),
                                24.0, 0.0, 120.0, 1, &accepted);
    if (!accepted || decibels <= 0.0) {
        return;
    }
    // The dialog asks for a reduction, so the sign is flipped here rather than
    // asking the user to type a minus they will forget.
    applySpectralEdit(
        tr("attenuate %1 dB").arg(decibels, 0, 'f', 0),
        [decibels](AudioBufferView audio, const spectral::SpectralRegion& region, SampleRate rate) {
            return spectral::attenuateRegion(audio, rate, region, -decibels);
        });
}

void MainWindow::healSelection() {
    applySpectralEdit(tr("heal"),
                      [](AudioBufferView audio, const spectral::SpectralRegion& region,
                         SampleRate rate) { return spectral::healRegion(audio, rate, region); });
}

void MainWindow::applyGainDecibels(double decibels, const QString& label) {
    const TimeSelection range = targetRange();
    if (range.isEmpty()) {
        return;
    }
    const auto factor = static_cast<float>(std::pow(10.0, decibels / 20.0));
    (void)applyEdit(label, [this, range, factor] {
        return engine::applyRangeGain(document_, range.start, range.end, factor).ok();
    });
}

void MainWindow::chooseGain() {
    if (!hasDocument()) {
        return;
    }
    bool accepted = false;
    const double decibels = QInputDialog::getDouble(this, tr("Gain"), tr("Change level by (dB):"),
                                                    0.0, -96.0, 24.0, 2, &accepted);
    if (accepted && decibels != 0.0) {
        applyGainDecibels(decibels, tr("gain %1 dB").arg(decibels, 0, 'f', 2));
    }
}

void MainWindow::chooseFilter() {
    if (!hasDocument()) {
        return;
    }

    // Three shapes rather than a full parametric EQ. A proper EQ wants a curve
    // you can drag over the analyser, which is a different piece of work; these
    // three are what corrective editing actually reaches for, and a high-pass
    // to lose rumble is the single most-used filter in repair. Spectral
    // attenuation can take out a band, but it cannot roll one off smoothly, and
    // rumble wants a slope rather than a hole.
    const QStringList shapes{tr("High-pass (remove rumble below)"),
                             tr("Low-pass (remove hiss above)"), tr("Peak or dip at")};
    bool accepted = false;
    const QString shape =
        QInputDialog::getItem(this, tr("Filter"), tr("Shape:"), shapes, 0, false, &accepted);
    if (!accepted) {
        return;
    }
    const int index = shapes.indexOf(shape);

    const double frequency =
        QInputDialog::getDouble(this, tr("Filter"), tr("Frequency (Hz):"),
                                index == 0 ? 80.0 : (index == 1 ? 12000.0 : 1000.0), 10.0,
                                document_.sampleRate().hz() * 0.45, 1, &accepted);
    if (!accepted) {
        return;
    }

    double gainDb = 0.0;
    if (index == 2) {
        gainDb = QInputDialog::getDouble(this, tr("Filter"), tr("Gain (dB), negative to cut:"),
                                         -6.0, -24.0, 24.0, 1, &accepted);
        if (!accepted) {
            return;
        }
    }

    const int type = index == 0 ? static_cast<int>(dsp::FilterType::HighPass)
                                : (index == 1 ? static_cast<int>(dsp::FilterType::LowPass)
                                              : static_cast<int>(dsp::FilterType::Peaking));
    const QString label =
        index == 2 ? tr("%1 dB at %2 Hz").arg(gainDb, 0, 'f', 1).arg(frequency, 0, 'f', 0)
                   : tr("%1 at %2 Hz")
                         .arg(index == 0 ? tr("high-pass") : tr("low-pass"))
                         .arg(frequency, 0, 'f', 0);
    applyFilter(type, frequency, dsp::kButterworthQ, gainDb, label);
}

void MainWindow::applyFilter(int filterType, double frequency, double q, double gainDb,
                             const QString& label) {
    const TimeSelection range = targetRange();
    if (range.isEmpty() || !documentSource_) {
        return;
    }

    const double rate = document_.sampleRate().hz();
    const double period = rate / std::max(frequency, 1.0);

    // A biquad has memory, so starting it cold at the selection's edge steps out
    // of silence into signal and clicks. It gets a run-up outside the range to
    // settle in, and only the range itself is written back. Twenty periods of
    // the corner settles a Q of 0.7 far below the last bit; the quarter-second
    // floor is there for the high corners, where twenty periods is nothing.
    const auto context = static_cast<SampleCount>(std::max(0.25 * rate, 20.0 * period));
    const SampleIndex spanStart = std::max<SampleIndex>(0, range.start - context);

    AudioBuffer span{document_.layout(), range.end - spanStart};
    if (!documentSource_->read(spanStart, span.view())) {
        status_->setText(tr("Could not read the selection"));
        return;
    }

    const int channels = document_.layout().count();
    const SampleCount offset = range.start - spanStart;

    // The range as it stands, kept back for the edge blends below.
    AudioBuffer original{document_.layout(), range.length()};
    for (int channel = 0; channel < channels; ++channel) {
        std::copy_n(span.channel(channel) + offset, original.frames(), original.channel(channel));
    }

    dsp::FilterSpec spec;
    spec.type = static_cast<dsp::FilterType>(filterType);
    spec.frequency = frequency;
    spec.q = q;
    spec.gainDb = gainDb;

    for (int channel = 0; channel < channels; ++channel) {
        auto eq = dsp::ParametricEq::create(document_.sampleRate());
        if (!eq) {
            status_->setText(tr("Could not build the filter: %1")
                                 .arg(QString::fromStdString(std::string{eq.error().what()})));
            return;
        }
        dsp::EqBand band;
        band.filter = spec;
        if (!eq.value().addBand(band)) {
            status_->setText(tr("Those filter settings are not usable"));
            return;
        }
        eq.value().processInPlace(span.channel(channel), span.frames());
    }

    // Write back only the range. The run-up was there to settle the filter, not
    // to be applied to audio the user did not select.
    AudioBuffer applied{document_.layout(), range.length()};
    for (int channel = 0; channel < channels; ++channel) {
        std::copy_n(span.channel(channel) + offset, applied.frames(), applied.channel(channel));
    }

    // Filtering part of a file leaves a step at each end of it: inside the range
    // the removed band is gone, outside it is still there, and the jump between
    // the two is a click. Measured on a 40 Hz rumble high-passed at 80 Hz, the
    // join stepped by 0.166 where the steps either side of it were 0.053.
    //
    // Blending across a couple of periods of the corner frequency spreads that
    // step over the wavelength that caused it. It costs the very edges of the
    // range, which is why the length follows the corner rather than being
    // fixed: a 12 kHz low-pass gives up a fraction of a millisecond, an 80 Hz
    // high-pass gives up 25 ms, and each is the shortest blend its own step can
    // hide behind. An end with nothing beyond it has no step to hide, so it is
    // filtered to the last sample.
    const auto blend = std::min<SampleCount>(
        applied.frames() / 4, static_cast<SampleCount>(std::max(0.002 * rate, 2.0 * period)));
    const bool blendHead = range.start > 0;
    const bool blendTail = range.end < document_.duration();
    if (blend > 0 && (blendHead || blendTail)) {
        for (int channel = 0; channel < channels; ++channel) {
            float* out = applied.channel(channel);
            const float* was = original.channel(channel);
            for (SampleCount i = 0; i < blend; ++i) {
                // Raised cosine, so the blend has no corner of its own in it.
                const double weight =
                    0.5 - 0.5 * std::cos(std::numbers::pi * (static_cast<double>(i) + 0.5) /
                                         static_cast<double>(blend));
                if (blendHead) {
                    out[i] = static_cast<float>(weight * out[i] + (1.0 - weight) * was[i]);
                }
                if (blendTail) {
                    const SampleCount j = applied.frames() - 1 - i;
                    out[j] = static_cast<float>(weight * out[j] + (1.0 - weight) * was[j]);
                }
            }
        }
    }

    (void)applyEdit(label, [this, &range, &applied] {
        return engine::replaceRange(document_, range.start, std::move(applied)).ok();
    });
}

void MainWindow::chooseTimeStretch() {
    if (!hasDocument()) {
        return;
    }
    // Asked for as a length rather than as a factor, because that is what the
    // job is: this take has to fit that slot. A factor makes the user do the
    // division, and they will do it in their head, and sometimes wrongly.
    bool accepted = false;
    const double percent = QInputDialog::getDouble(
        this, tr("Time stretch"), tr("New length, as a percentage of the current one:"), 100.0,
        100.0 * dsp::stretch::kMinimumFactor, 100.0 * dsp::stretch::kMaximumFactor, 2, &accepted);
    if (!accepted || std::abs(percent - 100.0) < 1e-9) {
        return;
    }
    applyTimeStretch(percent / 100.0, tr("stretch to %1%").arg(percent, 0, 'f', 2));
}

void MainWindow::choosePitchShift() {
    if (!hasDocument()) {
        return;
    }
    bool accepted = false;
    const double semitones = QInputDialog::getDouble(
        this, tr("Pitch shift"), tr("Semitones, fractions allowed (0.01 is a cent):"), 0.0,
        dsp::stretch::kMinimumSemitones, dsp::stretch::kMaximumSemitones, 2, &accepted);
    if (!accepted || std::abs(semitones) < 1e-9) {
        return;
    }
    applyPitchShift(semitones, tr("shift by %1 semitones").arg(semitones, 0, 'f', 2));
}

bool MainWindow::applyTimeStretch(double factor, const QString& label) {
    const TimeSelection range = targetRange();
    if (range.isEmpty() || !documentSource_) {
        return false;
    }

    AudioBuffer span{document_.layout(), range.length()};
    if (!documentSource_->read(range.start, span.view())) {
        status_->setText(tr("Could not read the selection"));
        return false;
    }

    // A phase vocoder is thousands of transforms and it is not instant on a
    // long selection. Saying so beats a window that has stopped answering.
    //
    // repaint() rather than processEvents(): the message has to appear now, but
    // running the event loop here would let the user start a second stretch on
    // top of this one, and the second would finish first and apply to a
    // document the first still thinks it knows.
    status_->setText(tr("Stretching…"));
    status_->repaint();

    dsp::StretchSettings settings;
    settings.factor = factor;
    auto stretched = dsp::timeStretch(span, settings);
    if (!stretched) {
        status_->setText(tr("Could not stretch: %1")
                             .arg(QString::fromStdString(std::string{stretched.error().what()})));
        return false;
    }

    // Rebuilding a waveform from reconstructed phases does not reproduce the
    // original crest, so a file that was already close to the ceiling can come
    // back over it. Small -- a tenth of a decibel on the material this was
    // measured on -- but a tenth of a decibel is the difference between a clean
    // export and a clipped one, and the user should hear it from us rather than
    // from the file.
    const QString warning =
        peakOf(stretched.value()) > 1.0
            ? tr(" — it now peaks over full scale, so limit it before exporting")
            : QString{};

    const SampleIndex start = range.start;
    const SampleIndex end = range.end;
    const bool applied = applyEdit(label, [this, start, end, &stretched] {
        // The length changes, so the range comes out and the new audio goes in
        // rather than being written over the top. Everything after it moves,
        // which is the point of a stretch.
        if (!engine::deleteRange(document_, start, end, true).ok()) {
            return false;
        }
        const SampleCount length = stretched.value().frames();
        if (!engine::insertSilence(document_, start, length).ok()) {
            return false;
        }
        auto source = document_.addSource(std::make_shared<engine::BufferSource>(
                                              std::move(stretched.value()), document_.sampleRate()),
                                          "time stretch");
        return source.hasValue() && document_.appendSource(source.value(), start).hasValue();
    });
    if (applied && !warning.isEmpty()) {
        status_->setText(label + warning);
    }
    return applied;
}

bool MainWindow::applyPitchShift(double semitones, const QString& label) {
    const TimeSelection range = targetRange();
    if (range.isEmpty() || !documentSource_) {
        return false;
    }

    AudioBuffer span{document_.layout(), range.length()};
    if (!documentSource_->read(range.start, span.view())) {
        status_->setText(tr("Could not read the selection"));
        return false;
    }

    status_->setText(tr("Shifting…"));
    status_->repaint(); // As above: no event loop, so no re-entry.

    dsp::PitchSettings settings;
    settings.semitones = semitones;
    auto shifted = dsp::pitchShift(span, settings);
    if (!shifted) {
        status_->setText(tr("Could not shift: %1")
                             .arg(QString::fromStdString(std::string{shifted.error().what()})));
        return false;
    }

    const QString warning =
        peakOf(shifted.value()) > 1.0
            ? tr(" — it now peaks over full scale, so limit it before exporting")
            : QString{};

    // A shift keeps the length, so this writes over the range in place and
    // nothing downstream of it moves.
    const bool applied = applyEdit(label, [this, &range, &shifted] {
        return engine::replaceRange(document_, range.start, std::move(shifted.value())).ok();
    });
    if (applied && !warning.isEmpty()) {
        status_->setText(label + warning);
    }
    return applied;
}

void MainWindow::chooseLimiter() {
    if (!hasDocument()) {
        return;
    }
    bool accepted = false;
    const double ceiling =
        QInputDialog::getDouble(this, tr("Limiter"), tr("Ceiling (dBTP), true-peak aware:"), -1.0,
                                -24.0, 0.0, 1, &accepted);
    if (accepted) {
        limitTo(ceiling);
    }
}

void MainWindow::limitTo(double ceilingDb) {
    if (!hasDocument() || !documentSource_) {
        return;
    }
    const TimeSelection range = targetRange();
    if (range.isEmpty()) {
        return;
    }

    // The limiter has look-ahead, so it delays the audio. Feeding it the range
    // alone would lose the first few milliseconds off the front and leave its
    // tail unflushed, so it gets a run-up and a run-out and the delay is undone
    // afterwards by taking the output from latencySamples() in.
    const auto context = static_cast<SampleCount>(0.5 * document_.sampleRate().hz());
    const SampleIndex spanStart = std::max<SampleIndex>(0, range.start - context);
    const SampleIndex spanEnd = std::min<SampleIndex>(document_.duration(), range.end + context);

    AudioBuffer span{document_.layout(), spanEnd - spanStart};
    if (!documentSource_->read(spanStart, span.view())) {
        status_->setText(tr("Could not read the selection"));
        return;
    }

    // The offline limiter, not the streaming one. The streaming limiter detects
    // on an oversampled view but applies gain at the base rate, which leaves
    // about 0.2 dB over the ceiling in the reconstruction -- fine inside a
    // chain, wrong for the last thing before a file. This one oversamples the
    // signal path and then measures the result exactly and trims the residual,
    // so the ceiling is held rather than approached.
    dsp::OfflineLimitSettings settings;
    settings.limiter.ceilingDb = ceilingDb;
    settings.oversampling = 4;
    settings.linkStereo = true;
    settings.trimToCeiling = true;

    if (const auto status = dsp::limitOffline(span.view(), document_.sampleRate(), settings);
        !status) {
        status_->setText(tr("Could not limit: %1")
                             .arg(QString::fromStdString(std::string{status.error().what()})));
        return;
    }

    AudioBuffer aligned{document_.layout(), span.frames()};
    for (int channel = 0; channel < document_.layout().count(); ++channel) {
        std::copy_n(span.channel(channel), span.frames(), aligned.channel(channel));
    }

    (void)applyEdit(tr("limit to %1 dBTP").arg(ceilingDb, 0, 'f', 1), [this, spanStart, &aligned] {
        return engine::replaceRange(document_, spanStart, std::move(aligned)).ok();
    });
}

void MainWindow::normaliseToTarget() {
    if (!hasDocument()) {
        return;
    }
    // The measurement may still be running -- on a long file it will be, and on
    // a freshly opened one it always is. Waiting is the right answer either way:
    // the alternative is a menu item that silently does nothing depending on how
    // fast the user reached for it.
    if (meters_->busy()) {
        status_->setText(tr("Waiting for the measurement to finish…"));
        if (!waitForAnalysis()) {
            status_->setText(tr("The measurement did not finish in time"));
            return;
        }
    }

    const std::optional<double> gain = meters_->conformGainDb();
    if (!gain) {
        status_->setText(tr("Nothing measurable to normalise yet"));
        return;
    }
    if (std::abs(*gain) < 0.01) {
        status_->setText(tr("Already on target for %1").arg(meters_->targetName()));
        return;
    }
    // The measurement covers whatever the panel last measured, which is the
    // same range this will change -- targetRange decides both.
    applyGainDecibels(*gain, tr("normalise to %1").arg(meters_->targetName()));
}

void MainWindow::applyFade(bool fadingIn) {
    const TimeSelection range = targetRange();
    if (range.isEmpty()) {
        return;
    }
    (void)applyEdit(fadingIn ? tr("fade in") : tr("fade out"), [this, range, fadingIn] {
        return engine::applyRangeFade(document_, range.start, range.end, fadingIn,
                                      engine::FadeShape::Linear)
            .ok();
    });
}

void MainWindow::flattenRange() {
    const TimeSelection range = targetRange();
    if (range.isEmpty()) {
        return;
    }
    (void)applyEdit(tr("flatten"), [this, range] {
        return engine::flattenRange(document_, range.start, range.end).ok();
    });
}

void MainWindow::copySelection() {
    const TimeSelection selected = selection();
    if (selected.isEmpty() || !documentSource_) {
        return;
    }
    AudioBuffer copied{document_.layout(), selected.length()};
    if (!documentSource_->read(selected.start, copied.view())) {
        status_->setText(tr("Could not read the selection"));
        return;
    }
    clipboard_ = std::move(copied);
    refreshActions();
}

void MainWindow::cutSelection() {
    copySelection();
    deleteSelection(true);
}

void MainWindow::deleteSelection(bool ripple) {
    const TimeSelection selected = selection();
    if (selected.isEmpty()) {
        return;
    }
    const bool applied = applyEdit(ripple ? tr("delete") : tr("silence"), [this, selected, ripple] {
        return engine::deleteRange(document_, selected.start, selected.end, ripple).ok();
    });
    if (applied && ripple) {
        const TimeSelection collapsed{selected.start, selected.start};
        waveform_->setSelection(collapsed);
        spectrogram_->setSelection(collapsed);
        ruler_->setSelection(collapsed);
        refreshActions();
        updateStatus();
    }
}

void MainWindow::trimToSelection() {
    const TimeSelection selected = selection();
    if (selected.isEmpty()) {
        return;
    }
    const bool applied = applyEdit(tr("trim"), [this, selected] {
        // Back to front: deleting the tail first leaves the head's indices
        // unchanged, so both ranges refer to the same audio they did when the
        // user chose them.
        //
        // Either half can be empty -- a selection running to the end of the
        // document has no tail to remove -- and deleteRange rejects an empty
        // range, so skip rather than fail. Trimming to a selection that is
        // already the whole document is a no-op, not an error.
        if (selected.end < document_.duration() &&
            !engine::deleteRange(document_, selected.end, document_.duration(), true).ok()) {
            return false;
        }
        if (selected.start > 0 && !engine::deleteRange(document_, 0, selected.start, true).ok()) {
            return false;
        }
        return true;
    });
    if (!applied) {
        return;
    }
    const TimeSelection all{0, document_.duration()};
    waveform_->setSelection(all);
    spectrogram_->setSelection(all);
    ruler_->setSelection(all);
    waveform_->showAll();
    refreshActions();
    updateStatus();
}

void MainWindow::pasteClipboard() {
    if (clipboard_.frames() <= 0 || !hasDocument()) {
        return;
    }
    if (clipboard_.layout().count() != document_.layout().count()) {
        status_->setText(tr("The clipboard has %1 channels and this document has %2")
                             .arg(clipboard_.layout().count())
                             .arg(document_.layout().count()));
        return;
    }

    const SampleIndex at = selection().start;
    const SampleCount length = clipboard_.frames();
    (void)applyEdit(tr("paste"), [this, at, length] {
        // Open a gap first, then lay the clipboard into it. The gap is silence
        // and overlapping clips mix, so the result is exactly the pasted audio
        // without a dedicated "replace range" verb.
        if (!engine::insertSilence(document_, at, length).ok()) {
            return false;
        }
        AudioBuffer copy{clipboard_.layout(), clipboard_.frames()};
        for (int channel = 0; channel < copy.layout().count(); ++channel) {
            std::copy_n(clipboard_.channel(channel), clipboard_.frames(), copy.channel(channel));
        }
        auto source = document_.addSource(
            std::make_shared<engine::BufferSource>(std::move(copy), document_.sampleRate()),
            "clipboard");
        return source.hasValue() && document_.appendSource(source.value(), at).hasValue();
    });
}

void MainWindow::undo() {
    if (!history_ || !history_->canUndo()) {
        return;
    }
    // Undo rewrites the document exactly as an edit does, so it owes the
    // readers the same courtesy -- see applyEdit.
    stopPlayback();
    cancelSpectrogramBuild();
    if (history_->undo(document_)) {
        rebuildCaches();
        refreshViews();
    } else {
        startSpectrogramBuild();
    }
}

void MainWindow::redo() {
    if (!history_ || !history_->canRedo()) {
        return;
    }
    stopPlayback();
    cancelSpectrogramBuild();
    if (history_->redo(document_)) {
        rebuildCaches();
        refreshViews();
    } else {
        startSpectrogramBuild();
    }
}

void MainWindow::selectSeconds(double from, double to) {
    if (!hasDocument()) {
        return;
    }
    const auto toSamples = [this](double seconds) {
        return std::clamp<SampleIndex>(
            static_cast<SampleIndex>(seconds * document_.sampleRate().hz()), 0,
            document_.duration());
    };
    selectionChanged(toSamples(from), toSamples(to));
}

bool MainWindow::applyOperation(const QString& name) {
    if (name.startsWith("select:")) {
        const QString span = name.mid(7);
        const qsizetype dash = span.indexOf('-', 1);
        if (dash <= 0) {
            return false;
        }
        bool okFrom = false;
        bool okTo = false;
        const double from = span.left(dash).toDouble(&okFrom);
        const double to = span.mid(dash + 1).toDouble(&okTo);
        if (!okFrom || !okTo) {
            return false;
        }
        selectSeconds(from, to);
        return true;
    }
    if (name.startsWith("gain:")) {
        bool ok = false;
        const double decibels = name.mid(5).toDouble(&ok);
        if (!ok) {
            return false;
        }
        applyGainDecibels(decibels, QStringLiteral("gain"));
        return true;
    }
    if (name.startsWith("band:")) {
        const QString span = name.mid(5);
        const qsizetype dash = span.indexOf('-', 1);
        if (dash <= 0) {
            return false;
        }
        bool okLow = false;
        bool okHigh = false;
        const double low = span.left(dash).toDouble(&okLow);
        const double high = span.mid(dash + 1).toDouble(&okHigh);
        if (!okLow || !okHigh) {
            return false;
        }
        selectFrequencyBand(low, high);
        return true;
    }
    if (name.startsWith("attenuate:")) {
        bool ok = false;
        const double decibels = name.mid(10).toDouble(&ok);
        if (!ok) {
            return false;
        }
        applySpectralEdit(QStringLiteral("attenuate"),
                          [decibels](AudioBufferView audio, const spectral::SpectralRegion& region,
                                     SampleRate rate) {
                              return spectral::attenuateRegion(audio, rate, region, -decibels);
                          });
        return true;
    }
    if (name == "heal") {
        healSelection();
        return true;
    }
    if (name == "learnnoise") {
        learnNoiseProfile();
        return true;
    }
    if (name.startsWith("denoise:")) {
        bool ok = false;
        const double decibels = name.mid(8).toDouble(&ok);
        if (!ok || noiseProfile_.isEmpty()) {
            return false;
        }
        applySpectralEdit(
            QStringLiteral("reduce noise"),
            [this, decibels](AudioBufferView audio, const spectral::SpectralRegion& region,
                             SampleRate rate) {
                spectral::DenoiseSettings settings;
                settings.reductionDb = decibels;
                return spectral::denoise(audio, rate, noiseProfile_, region.startSample,
                                         region.endSample, settings);
            });
        return true;
    }
    if (name.startsWith("highpass:")) {
        bool ok = false;
        const double frequency = name.mid(9).toDouble(&ok);
        if (!ok) {
            return false;
        }
        applyFilter(static_cast<int>(dsp::FilterType::HighPass), frequency, dsp::kButterworthQ, 0.0,
                    QStringLiteral("high-pass"));
        return true;
    }
    if (name.startsWith("lowpass:")) {
        bool ok = false;
        const double frequency = name.mid(8).toDouble(&ok);
        if (!ok) {
            return false;
        }
        applyFilter(static_cast<int>(dsp::FilterType::LowPass), frequency, dsp::kButterworthQ, 0.0,
                    QStringLiteral("low-pass"));
        return true;
    }
    if (name.startsWith("stretch:")) {
        bool ok = false;
        const double percent = name.mid(8).toDouble(&ok);
        if (!ok) {
            return false;
        }
        return applyTimeStretch(percent / 100.0, QStringLiteral("stretch"));
    }
    if (name.startsWith("pitch:")) {
        bool ok = false;
        const double semitones = name.mid(6).toDouble(&ok);
        if (!ok) {
            return false;
        }
        return applyPitchShift(semitones, QStringLiteral("pitch shift"));
    }
    if (name.startsWith("limit:")) {
        bool ok = false;
        const double ceiling = name.mid(6).toDouble(&ok);
        if (!ok) {
            return false;
        }
        limitTo(ceiling);
        return true;
    }
    if (name == "normalise") {
        normaliseToTarget();
    } else if (name == "fadein") {
        applyFade(true);
    } else if (name == "fadeout") {
        applyFade(false);
    } else if (name == "flatten") {
        flattenRange();
    } else if (name == "cut") {
        cutSelection();
    } else if (name == "copy") {
        copySelection();
    } else if (name == "paste") {
        pasteClipboard();
    } else if (name == "delete") {
        deleteSelection(true);
    } else if (name == "silence") {
        deleteSelection(false);
    } else if (name == "trim") {
        trimToSelection();
    } else if (name == "undo") {
        undo();
    } else if (name == "redo") {
        redo();
    } else if (name == "selectall") {
        selectionChanged(0, document_.duration());
    } else if (name == "deselect") {
        selectionChanged(selection().start, selection().start);
    } else {
        return false;
    }
    return true;
}

bool MainWindow::exportTo(const std::filesystem::path& path, bool selectionOnly) {
    if (!hasDocument() || !documentSource_) {
        return false;
    }

    const TimeSelection selected = selection();
    const SampleIndex start = selectionOnly && !selected.isEmpty() ? selected.start : 0;
    const SampleCount length =
        selectionOnly && !selected.isEmpty() ? selected.length() : document_.duration();

    std::ofstream stream{path, std::ios::binary};
    if (!stream) {
        status_->setText(tr("Could not write %1").arg(QString::fromStdString(path.string())));
        return false;
    }

    auto writer = io::WavWriter::create(stream, document_.sampleRate(), document_.layout());
    if (!writer) {
        status_->setText(tr("Could not start the export: %1")
                             .arg(QString::fromStdString(std::string{writer.error().what()})));
        return false;
    }

    // Block at a time, so exporting a long document costs one block of memory
    // rather than all of it.
    constexpr SampleCount kBlock = 65536;
    AudioBuffer block{document_.layout(), kBlock};
    SampleCount written = 0;
    while (written < length) {
        const SampleCount want = std::min<SampleCount>(kBlock, length - written);
        AudioBufferView view = block.view().subRange(0, want);
        const auto read = documentSource_->read(start + written, view);
        if (!read || read.value() <= 0) {
            break;
        }
        if (!writer.value().write(view.subRange(0, read.value()))) {
            status_->setText(tr("The export failed partway through"));
            return false;
        }
        written += read.value();
    }

    if (!writer.value().finish()) {
        status_->setText(tr("Could not finish %1").arg(QString::fromStdString(path.string())));
        return false;
    }

    status_->setText(tr("Exported %1 to %2")
                         .arg(QString::fromStdString(formatTime(
                                  samplesToSeconds(written, document_.sampleRate()), 60.0)),
                              QString::fromStdString(path.filename().string())));
    return true;
}

bool MainWindow::playToEnd(int timeoutMs) {
    togglePlayback();
    if (!player_ || !player_->isPlaying()) {
        return false;
    }

    const TimeSelection selected = selection();
    const SampleIndex end = selected.isEmpty() ? document_.duration() : selected.end;

    QElapsedTimer clock;
    clock.start();
    while (player_->isPlaying() && player_->position() < end) {
        if (clock.elapsed() > timeoutMs) {
            stopPlayback();
            return false;
        }
        QApplication::processEvents(QEventLoop::WaitForMoreEvents, 20);
    }

    const SampleIndex reached = player_->position();
    const std::uint64_t dropouts = player_->underruns();
    stopPlayback();

    std::printf("played_to=%lld\nplayed_from=%lld\nunderruns=%llu\n",
                static_cast<long long>(reached), static_cast<long long>(selected.start),
                static_cast<unsigned long long>(dropouts));
    std::fflush(stdout);
    return true;
}

bool MainWindow::printAnalysis() const {
    const analysis::ProgrammeAnalysis* result = meters_->latest();
    if (result == nullptr) {
        return false;
    }
    const auto line = [](const char* key, double value) { std::printf("%s=%.6f\n", key, value); };
    line("integrated_lufs", result->loudness.integratedLufs);
    line("short_term_lufs", result->loudness.shortTermLufs);
    line("max_short_term_lufs", result->loudness.maximumShortTermLufs);
    line("max_momentary_lufs", result->loudness.maximumMomentaryLufs);
    line("loudness_range_lu", result->loudness.loudnessRangeLu);
    line("true_peak_dbtp", result->truePeakDbtp);
    line("sample_peak_dbfs", result->statistics.samplePeakDbfs);
    line("rms_dbfs", result->statistics.rmsDbfs);
    line("crest_factor_db", result->statistics.crestFactorDb);
    line("dc_offset", result->statistics.dcOffset);
    line("peak_to_loudness_lu", result->statistics.peakToLoudnessRatioDb);
    std::printf("gated_blocks=%lld\n", static_cast<long long>(result->loudness.gatedBlockCount));
    std::printf("frames=%lld\n", static_cast<long long>(result->statistics.frames));
    std::fflush(stdout);
    return true;
}

bool MainWindow::waitForAnalysis(int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (meters_->busy() || spectrogramBusy_) {
        if (clock.elapsed() > timeoutMs) {
            std::fprintf(stderr, "sound-analyser: gave up waiting for %s after %d ms\n",
                         spectrogramBusy_ ? "the spectrogram" : "the meters", timeoutMs);
            return false;
        }
        // Wait for work rather than spinning: the worker posts its result as a
        // queued call, so the loop only needs to wake when something arrives.
        QApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    }
    QApplication::processEvents();
    return true;
}

bool MainWindow::saveSpectrogramImage(const std::filesystem::path& path) {
    QApplication::processEvents();
    const QPixmap shot = spectrogram_->grab(
        QRect{kGutterWidth, 0, spectrogram_->width() - kGutterWidth, spectrogram_->height()});
    QApplication::processEvents();
    return shot.save(QString::fromStdString(path.string()), "PNG");
}

bool MainWindow::saveScreenshot(const std::filesystem::path& path) {
    // Force a layout and paint pass first: with no compositor there is nothing
    // to trigger one, and grabbing early captures an empty window.
    QApplication::processEvents();
    const QPixmap shot = grab();
    QApplication::processEvents();
    return shot.save(QString::fromStdString(path.string()), "PNG");
}

} // namespace sa::ui
