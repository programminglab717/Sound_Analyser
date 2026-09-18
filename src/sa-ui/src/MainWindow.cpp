#include <sa/device/AudioDeviceManager.h>
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

namespace sa::ui {

namespace {

/// Building a spectrogram currently needs the decoded audio resident, so a file
/// long enough to exhaust memory is analysed as a waveform only rather than
/// taking the machine down. Lifting this means a streaming pyramid build --
/// tracked in docs/07-autonomous-queue.md.
constexpr std::size_t kMaximumDecodedBytes = 1'500'000'000;

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

    const auto decodedBytes = static_cast<std::size_t>(document_.duration()) *
                              static_cast<std::size_t>(document_.layout().count()) * sizeof(float);
    if (decodedBytes > kMaximumDecodedBytes) {
        spectrogramNote_ = tr("too long for spectrogram analysis in this build (%1 GB decoded)")
                               .arg(static_cast<double>(decodedBytes) / 1e9, 0, 'f', 1);
        return;
    }

    AudioBuffer whole{document_.layout(), document_.duration()};
    if (!documentSource_->read(0, whole.view())) {
        spectrogramNote_ = tr("could not read the document for analysis");
        return;
    }
    auto spectra = spectral::SpectrogramPyramid::build(whole.constView(), 0, displayConfig());
    if (spectra) {
        spectra_ = std::make_shared<const spectral::SpectrogramPyramid>(std::move(spectra).value());
    } else {
        spectrogramNote_ = tr("spectrogram analysis failed");
    }
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
    if (!edit()) {
        status_->setText(tr("%1 did not apply").arg(label));
        return false;
    }
    // The player is streaming from the document source that is about to be
    // replaced. Stopping first is not politeness, it is the lifetime rule.
    stopPlayback();
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

        device::AudioDeviceManager manager;
        auto opened = manager.openDefault(config);
        if (!opened) {
            status_->setText(tr("No audio output: %1")
                                 .arg(QString::fromStdString(std::string{opened.error().what()})));
            return;
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
    if (history_ && history_->undo(document_)) {
        rebuildCaches();
        refreshViews();
    }
}

void MainWindow::redo() {
    if (history_ && history_->redo(document_)) {
        rebuildCaches();
        refreshViews();
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
    while (meters_->busy()) {
        if (clock.elapsed() > timeoutMs) {
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
