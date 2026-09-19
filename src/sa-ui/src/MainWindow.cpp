#include <sa/analysis/Spectrum.h>
#include <sa/device/AudioDeviceManager.h>
#include <sa/device/NullAudioDevice.h>
#include <sa/dsp/Declick.h>
#include <sa/dsp/Declip.h>
#include <sa/dsp/Dehum.h>
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
#include <sa/ui/FieldDialog.h>
#include <sa/ui/MainWindow.h>
#include <sa/ui/PreferencesDialog.h>
#include <sa/ui/ViewGeometry.h>

#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QList>
#include <QMenuBar>
#include <QScreen>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <string>
#include <system_error>
#include <utility>

namespace sa::ui {

namespace {

/// The five fade curves, with the name each goes by in the menu and in a batch
/// verb.
///
/// One table rather than a switch in each place: the menu, the undo label and
/// the verb parser all have to agree about the set, and three lists that have
/// to agree are two chances to forget one. The label is a callable because
/// tr() cannot run before QApplication exists.
struct FadeShapeEntry {
    engine::FadeShape shape;
    QString (*label)();
};

const FadeShapeEntry kFadeShapes[] = {
    {engine::FadeShape::Linear, [] { return MainWindow::tr("&Linear"); }},
    {engine::FadeShape::EqualPower, [] { return MainWindow::tr("&Equal power"); }},
    {engine::FadeShape::Logarithmic, [] { return MainWindow::tr("Lo&garithmic"); }},
    {engine::FadeShape::Exponential, [] { return MainWindow::tr("E&xponential"); }},
    {engine::FadeShape::SCurve, [] { return MainWindow::tr("&S-curve"); }},
};

/// The shape a verb names, or nothing if it names none of them.
///
/// The words are the settings layer's, which is also what writes them into the
/// file. One table for the verb and the stored name, because a shape renamed
/// in one of the two places and not the other is a settings file that silently
/// stops restoring.
[[nodiscard]] std::optional<engine::FadeShape> fadeShapeFor(const QString& verb) {
    return fadeShapeFromName(verb.toStdString());
}

/// A .sa file is an arrangement, not audio.
[[nodiscard]] bool isSessionPath(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    if (extension.size() != 3 || extension[0] != '.') {
        return false;
    }
    return (extension[1] == 's' || extension[1] == 'S') &&
           (extension[2] == 'a' || extension[2] == 'A');
}

/// What to call a shape in an undo label. Lower case and without the menu's
/// ampersand, because it reads as part of a sentence rather than as a menu
/// entry.
[[nodiscard]] QString fadeShapeName(engine::FadeShape shape) {
    for (const FadeShapeEntry& entry : kFadeShapes) {
        if (entry.shape == shape) {
            return entry.label().remove(QLatin1Char{'&'}).toLower();
        }
    }
    return MainWindow::tr("unknown");
}

/// Display analysis settings. The defaults -- 4096 at 48 kHz -- are an 11.7 Hz
/// bin and a 21 ms hop. A log axis stretches the bottom two octaves over half
/// the display, and 2048 gives them four bins to fill it with; 4096 gives them
/// eight, at a time resolution transients still survive.
///
/// A preference rather than a constant because the right answer depends on the
/// material: speech and drums want the time resolution, a room measurement or
/// a hum hunt wants the frequency resolution, and neither can be had at once.
[[nodiscard]] spectral::SpectrogramConfig displayConfig(const Preferences& preferences) {
    spectral::SpectrogramConfig config;
    config.fftSize = preferences.fftSize;
    config.hopSize = preferences.hopSize;
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

MainWindow::MainWindow(std::optional<std::filesystem::path> settingsFile)
    // A file named on the command line is neither the portable one nor the
    // per-user one; it is the one that was asked for. PerUser is the honest
    // label for the one sentence that reads it, which says where the settings
    // are and claims nothing else about them.
    : settingsFile_{!settingsFile ? settingsFileForThisBuild()
                                  : SettingsFile{std::move(*settingsFile), SettingsHome::PerUser}} {
    // First, because the preferences decide what several of the widgets below
    // are built with, and because the window's own size is one of them.
    const SavedSettings saved = loadSettings();
    spectrogramConfig_ = displayConfig(preferences_);

    setWindowTitle(tr("Auscultate"));
    resize(kDefaultWindowWidth, kDefaultWindowHeight);

    auto* central = new QWidget{this};
    auto* column = new QVBoxLayout{central};
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    ruler_ = new TimeRuler{central};

    auto* splitter = new QSplitter{Qt::Vertical, central};
    splitter_ = splitter;
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
    analysis_ = new AnalysisPanel{this};
    spectrum_ = new EqCurveView{this};

    // The spectrum goes under the meters rather than beside the views: it
    // answers the same kind of question the numbers do -- what is this passage
    // made of -- and unlike the two views above it does not have to line up
    // with the time axis.
    //
    // The musical answers go between them for the same reason. They are
    // numbers about this passage, they belong beside the other numbers about
    // it, and they were reachable only from the command line until now -- which
    // for a product that calls itself an analyser was the wrong way round.
    //
    // Of the three, only the analysis panel scrolls. Its length is not
    // fixed -- the room section alone is eight rows, and it appears only when
    // someone asks for it -- so without this the side column's minimum height
    // would change with a menu tick and force the whole window taller than the
    // screen it was opened on.
    auto* analysisScroll = new QScrollArea{this};
    analysisScroll->setWidget(analysis_);
    analysisScroll->setWidgetResizable(true);
    analysisScroll->setFrameShape(QFrame::NoFrame);
    analysisScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Two rows and a heading: enough that the key is on screen whatever else
    // the splitter is asked to fit.
    analysisScroll->setMinimumHeight(96);

    auto* side = new QSplitter{Qt::Vertical, this};
    sideSplitter_ = side;
    side->addWidget(meters_);
    side->addWidget(analysisScroll);
    side->addWidget(spectrum_);
    // The split as it opens, in pixels, because the alternative does not work:
    // a splitter divides by size hints, and a scroll area's hint says nothing
    // about the panel inside it, so the analysis panel would be handed the
    // smallest share of the three and its tempo would start below the fold.
    // These are the shares that put the key and the tempo on screen at the
    // default window height. Anyone who wants it otherwise drags the handle --
    // and, since the sizes are saved, drags it once rather than every launch.
    side->setSizes({420, 230, 140});
    // Where a larger window's extra height goes. Not to the analysis panel:
    // it has a fixed amount to say, and once it is all on screen more room
    // for it is room taken from the spectrum, which can always use it.
    side->setStretchFactor(0, 2);
    side->setStretchFactor(1, 0);
    side->setStretchFactor(2, 3);
    auto* row = new QWidget{this};
    auto* across = new QHBoxLayout{row};
    across->setContentsMargins(0, 0, 0, 0);
    across->setSpacing(0);
    across->addWidget(central, 1);
    across->addWidget(side);
    setCentralWidget(row);

    // One time axis and one selection. Either view can drive them; the other
    // and the ruler follow.
    connect(waveform_, &TimeAxisView::viewRangeChanged, spectrogram_, &TimeAxisView::setViewRange);
    connect(spectrogram_, &TimeAxisView::viewRangeChanged, waveform_, &TimeAxisView::setViewRange);
    connect(waveform_, &TimeAxisView::viewRangeChanged, ruler_, &TimeRuler::setViewRange);
    connect(spectrogram_, &TimeAxisView::viewRangeChanged, ruler_, &TimeRuler::setViewRange);
    connect(spectrogram_, &TimeAxisView::viewRangeChanged, this,
            [this](SampleIndex, SampleCount) { requestSpectrogramDetailSoon(); });

    connect(waveform_, &TimeAxisView::selectionChanged, this, &MainWindow::selectionChanged);
    connect(spectrogram_, &TimeAxisView::selectionChanged, this, &MainWindow::selectionChanged);

    connect(waveform_, &WaveformView::cursorMoved, this, &MainWindow::showWaveformCursor);
    connect(spectrogram_, &SpectrogramView::cursorMoved, this, &MainWindow::showSpectrogramCursor);

    // The EQ menu entries follow the curve, so that "apply" cannot be pressed
    // on a curve that would do nothing.
    connect(spectrum_, &EqCurveView::bandsChanged, this, &MainWindow::refreshActions);

    // The beat grid, the contour and the bands are drawn from the panel's
    // result, so they are put on screen when that result lands and at no other
    // time. A view updated from anywhere else could draw a grid belonging to a
    // measurement that has since been superseded.
    connect(analysis_, &AnalysisPanel::analysisFinished, this, &MainWindow::showMusicalAnalysis);

    status_ = new QLabel{tr("Open an audio file to begin"), this};
    readout_ = new QLabel{this};
    readout_->setMinimumWidth(340);
    readout_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addWidget(status_, 1);
    statusBar()->addPermanentWidget(readout_);

    // A quarter of a second after the last change: past the end of a drag, and
    // short enough not to feel deferred.
    analysisTimer_ = new QTimer{this};
    analysisTimer_->setSingleShot(true);
    analysisTimer_->setInterval(250);
    connect(analysisTimer_, &QTimer::timeout, this, &MainWindow::reanalyseNow);

    // Shorter than the analysis timer, because this one is about the picture
    // sharpening under the cursor rather than about numbers settling, and a
    // quarter of a second of blur after every scroll step is felt.
    detailTimer_ = new QTimer{this};
    detailTimer_->setSingleShot(true);
    detailTimer_->setInterval(80);
    connect(detailTimer_, &QTimer::timeout, this, &MainWindow::requestSpectrogramDetail);

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

    // Last: the preferences go into the widgets that draw with them, and the
    // saved layout goes into the widgets that have just been laid out.
    applyPreferences();
    applySavedLayout(saved);

    // Only now. The target lives in the panel's combo, and this is how a
    // change there reaches the file -- saved at once rather than at the next
    // close, for the reason the preferences dialog is: a setting somebody has
    // just made is the one they would most notice losing. Connected after the
    // preferences have been applied, because applying them sets the combo, and
    // a window that saved its settings while it was still being built would
    // write the file on every launch for no reason.
    connect(meters_, &LoudnessPanel::targetChanged, this, [this] {
        preferences_.loudnessTarget = meters_->target();
        saveSettings();
    });
}

MainWindow::~MainWindow() {
    // The ways out that deliver no close event still have settings worth
    // keeping: a batch run ends by returning from main, and a window torn down
    // without being closed is how several of the headless paths finish.
    if (!settingsWritten_) {
        saveSettings();
    }

    // The worker holds a pointer to this window's cancellation token and posts
    // back to this object. Joining here is what makes both safe; a detached
    // worker would outlive the thing it reports to.
    cancelSpectrogramBuild();
    // Same reasoning for the detail fetcher, which also holds a token pointing
    // into this window and posts back to it.
    cancelDetailFetch();
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

    // Rebuilt as it opens rather than as the list changes, because whether a
    // file is still there is a question about the disk whose answer keeps for
    // about as long as it takes to read it.
    recentMenu_ = file->addMenu(tr("Open &recent"));
    connect(recentMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
    rebuildRecentMenu();

    file->addSeparator();
    file->addAction(tr("Open &session…"), QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_O}, this,
                    &MainWindow::chooseOpenSession);
    file->addAction(tr("&Save session…"), QKeySequence::Save, this, &MainWindow::chooseSaveSession);
    file->addSeparator();
    file->addAction(tr("&Export…"), QKeySequence::SaveAs, this, &MainWindow::chooseExport);
    exportSelectionAction_ =
        file->addAction(tr("Export &selection…"), this, &MainWindow::chooseExportSelection);

    // What an export is written as. A submenu rather than a dialog on every
    // export, because it is a delivery decision made once for a job and then
    // left alone, and a prompt on each save would be in the way every time.
    QMenu* formats = file->addMenu(tr("Export &format"));
    auto* formatGroup = new QActionGroup{this};
    const auto addFormat = [&](const QString& label, io::SampleFormat format) {
        QAction* action = formats->addAction(label, this, [this, format] {
            preferences_.exportFormat = format;
            refreshActions();
        });
        action->setCheckable(true);
        formatGroup->addAction(action);
        preferenceTicks_.emplace_back(
            action, [this, format] { return preferences_.exportFormat == format; });
    };
    addFormat(tr("&16-bit"), io::SampleFormat::PcmInt16);
    addFormat(tr("&24-bit"), io::SampleFormat::PcmInt24);
    addFormat(tr("&32-bit float"), io::SampleFormat::Float32);

    QMenu* dithers = file->addMenu(tr("&Dither"));
    auto* ditherGroup = new QActionGroup{this};
    const auto addDither = [&](const QString& label, dsp::DitherType type) {
        QAction* action = dithers->addAction(label, this, [this, type] {
            preferences_.dither = type;
            refreshActions();
        });
        action->setCheckable(true);
        ditherGroup->addAction(action);
        preferenceTicks_.emplace_back(action, [this, type] { return preferences_.dither == type; });
    };
    addDither(tr("&None"), dsp::DitherType::None);
    addDither(tr("&Triangular"), dsp::DitherType::Tpdf);
    addDither(tr("Triangular, noise-&shaped"), dsp::DitherType::TpdfNoiseShaped);

    file->addSeparator();
    file->addAction(tr("&Preferences…"), QKeySequence::Preferences, this,
                    &MainWindow::choosePreferences);

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
    process->addSeparator();
    process->addAction(tr("Re&verse"), this,
                       [this] { (void)applyChannelOp(dsp::ChannelOp::Reverse, tr("reverse")); });
    process->addAction(tr("&Invert polarity"), this, [this] {
        (void)applyChannelOp(dsp::ChannelOp::InvertPolarity, tr("invert polarity"));
    });
    process->addAction(tr("S&wap left and right"), this, [this] {
        (void)applyChannelOp(dsp::ChannelOp::SwapChannels, tr("swap channels"));
    });
    process->addAction(tr("Sum to &mono"), this, [this] {
        (void)applyChannelOp(dsp::ChannelOp::SumToMono, tr("sum to mono"));
    });
    process->addSeparator();
    // The EQ is not a dialog, so it is a pair of verbs on a curve that is
    // edited over the analyser. Showing it and applying it are separate
    // because looking at a proposed curve against the spectrum is most of what
    // the curve is for, and a tool that applied on sight would make that
    // impossible.
    showEqAction_ =
        process->addAction(tr("Show &EQ curve"), QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_E},
                           this, [this] { setEqCurveVisible(!spectrum_->isEqVisible()); });
    showEqAction_->setCheckable(true);
    applyEqAction_ =
        process->addAction(tr("App&ly EQ curve"), QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_Q},
                           this, &MainWindow::applyEqCurve);
    resetEqAction_ = process->addAction(tr("Reset EQ &bands"), this, &MainWindow::resetEqCurve);

    process->addSeparator();
    process->addAction(tr("Co&mpressor…"), this, &MainWindow::chooseCompressor);
    process->addAction(tr("&Gate…"), this, &MainWindow::chooseGate);
    process->addAction(tr("&Time stretch…"), this, &MainWindow::chooseTimeStretch);
    process->addAction(tr("&Pitch shift…"), this, &MainWindow::choosePitchShift);
    normaliseAction_ =
        process->addAction(tr("&Normalise to target"), QKeySequence{Qt::CTRL | Qt::Key_N}, this,
                           &MainWindow::normaliseToTarget);
    process->addSeparator();
    process->addAction(tr("Fade &in"), this, [this] { applyFade(true, preferences_.fadeShape); });
    process->addAction(tr("Fade &out"), this, [this] { applyFade(false, preferences_.fadeShape); });

    // The shape is a setting rather than five pairs of menu entries. Ten
    // entries for what is one choice made once and then left alone would push
    // the two verbs people actually reach for down a list.
    QMenu* shapes = process->addMenu(tr("Fade &shape"));
    auto* shapeGroup = new QActionGroup{this};
    for (const FadeShapeEntry& entry : kFadeShapes) {
        const engine::FadeShape shape = entry.shape;
        QAction* action = shapes->addAction(entry.label(), this, [this, shape] {
            preferences_.fadeShape = shape;
            refreshActions();
        });
        action->setCheckable(true);
        shapeGroup->addAction(action);
        preferenceTicks_.emplace_back(action,
                                      [this, shape] { return preferences_.fadeShape == shape; });
    }
    process->addSeparator();
    process->addAction(tr("F&latten"), this, &MainWindow::flattenRange);

    QMenu* transport = menuBar()->addMenu(tr("&Transport"));
    playAction_ = transport->addAction(tr("&Play"), QKeySequence{Qt::Key_Space}, this,
                                       &MainWindow::togglePlayback);
    transport->addAction(tr("&Stop"), QKeySequence{Qt::Key_Escape | Qt::SHIFT}, this,
                         &MainWindow::stopPlayback);

    QMenu* markers = menuBar()->addMenu(tr("&Markers"));
    markers->addAction(tr("&Add marker…"), QKeySequence{Qt::CTRL | Qt::Key_M}, this,
                       &MainWindow::chooseAddMarker);
    markers->addAction(tr("&Rename nearest marker…"), this, &MainWindow::renameMarker);
    markers->addAction(tr("&Delete nearest marker"), this, [this] { (void)deleteNearestMarker(); });
    markers->addSeparator();
    markers->addAction(tr("&Next marker"), QKeySequence{Qt::ALT | Qt::Key_Right}, this,
                       [this] { goToMarker(true); });
    markers->addAction(tr("&Previous marker"), QKeySequence{Qt::ALT | Qt::Key_Left}, this,
                       [this] { goToMarker(false); });
    markers->addSeparator();
    markers->addAction(tr("&Clear all markers"), this, [this] { (void)clearMarkers(); });

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
    repair->addAction(tr("De-&ess…"), this, &MainWindow::chooseDeess);
    repair->addAction(tr("Remove &clicks…"), QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_C}, this,
                      &MainWindow::chooseDeclick);
    repair->addAction(tr("Restore clipped &peaks"), this, &MainWindow::restoreClipping);
    repair->addAction(tr("Remove mains &hum"), this, [this] { (void)removeHum(); });
    repair->addSeparator();
    repair->addAction(tr("Select all &frequencies"), this,
                      [this] { selectFrequencyBand(0.0, document_.sampleRate().hz() * 0.5); });

    // A menu of its own rather than four entries under View, because these are
    // not ways of looking at the same picture: each one runs an analysis, and
    // two of them cost real time on a long file.
    QMenu* analyse = menuBar()->addMenu(tr("&Analyse"));
    const auto addToggle = [&](const QString& label, const QKeySequence& shortcut, bool& wanted) {
        QAction* action = analyse->addAction(label, shortcut, this, [this, &wanted] {
            wanted = !wanted;
            refreshActions();
            // Now rather than behind the timer: this is a menu press, so the
            // drag the timer exists to sit out is already over.
            reanalyseNow();
        });
        action->setCheckable(true);
        action->setChecked(wanted);
        return action;
    };

    beatGridAction_ =
        addToggle(tr("&Beat grid over the waveform"),
                  QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_B}, preferences_.showBeatGrid);
    pitchContourAction_ =
        addToggle(tr("&Pitch contour over the waveform"),
                  QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_P}, preferences_.showPitchContour);
    octaveBandsAction_ =
        addToggle(tr("Third-&octave bands on the spectrum"),
                  QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_T}, preferences_.showOctaveBands);
    analyse->addSeparator();
    // Named for what it assumes rather than for what it reports. Run on music
    // it produces a refusal and not a reverberation time, and the entry should
    // say so before it is pressed rather than after.
    roomAction_ =
        addToggle(tr("Measure as an &impulse response"), QKeySequence{}, preferences_.measureRoom);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Zoom to &fit"), QKeySequence{Qt::Key_F}, this,
                    [this] { waveform_->showAll(); });
    view->addAction(tr("Zoom to se&lection"), QKeySequence{Qt::CTRL | Qt::Key_E}, this,
                    [this] { waveform_->zoomToSelection(); });
    view->addSeparator();

    QMenu* scales = view->addMenu(tr("&Frequency scale"));
    auto* scaleGroup = new QActionGroup{this};
    const auto addScale = [&](const QString& label, FrequencyScale scale) {
        QAction* action = scales->addAction(label, this, [this, scale] {
            preferences_.frequencyScale = scale;
            setFrequencyScale(scale);
            refreshActions();
        });
        action->setCheckable(true);
        scaleGroup->addAction(action);
        preferenceTicks_.emplace_back(
            action, [this, scale] { return preferences_.frequencyScale == scale; });
    };
    addScale(tr("&Logarithmic"), FrequencyScale::Logarithmic);
    addScale(tr("Li&near"), FrequencyScale::Linear);

    QMenu* range = view->addMenu(tr("&Dynamic range"));
    auto* rangeGroup = new QActionGroup{this};
    // Built from the same list the settings validator holds a stored floor to,
    // so a value the file may contain and a value the menu offers cannot come
    // apart.
    const auto addRange = [&](double floorDb) {
        QAction* action =
            range->addAction(tr("%1 dB").arg(static_cast<int>(floorDb)), this, [this, floorDb] {
                preferences_.spectrogramFloorDb = floorDb;
                spectrogram_->setFloorDecibels(static_cast<float>(floorDb));
                refreshActions();
            });
        action->setCheckable(true);
        rangeGroup->addAction(action);
        preferenceTicks_.emplace_back(
            action, [this, floorDb] { return preferences_.spectrogramFloorDb == floorDb; });
    };
    for (const double floorDb : kFloorChoicesDb) {
        addRange(floorDb);
    }

    QMenu* colours = view->addMenu(tr("&Colour map"));
    auto* group = new QActionGroup{this};
    const auto addMap = [&](const QString& label, Colourmap map) {
        QAction* action = colours->addAction(label, this, [this, map] {
            preferences_.colourmap = map;
            setColourmap(map);
            refreshActions();
        });
        action->setCheckable(true);
        group->addAction(action);
        preferenceTicks_.emplace_back(action,
                                      [this, map] { return preferences_.colourmap == map; });
    };
    addMap(tr("Magma"), Colourmap::Magma);
    addMap(tr("Viridis"), Colourmap::Viridis);
    addMap(tr("Greyscale"), Colourmap::Grey);

    view->addSeparator();
    // Ctrl+Shift+R, because Ctrl+R is Repair > Attenuate. Two actions on one
    // shortcut is not a smaller problem than none: Qt resolves an ambiguous
    // shortcut by firing neither, so both keys silently stop working.
    view->addAction(tr("Set spectrum &reference"), QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_R},
                    this, &MainWindow::captureSpectrumReference);
    clearReferenceAction_ =
        view->addAction(tr("Clear spectrum reference"), this, &MainWindow::clearSpectrumReference);
}

SavedSettings MainWindow::loadSettings() {
    const SettingsStore store{settingsFile_.path};
    SavedSettings saved = readSettings(store.read());
    preferences_ = saved.preferences;
    recent_ = saved.recent;
    mayWriteSettings_ = saved.writeBack;
    loadedMainSplit_ = saved.mainSplit;
    loadedSideSplit_ = saved.sideSplit;
    return saved;
}

void MainWindow::applyPreferences() {
    spectrogram_->setColourmap(preferences_.colourmap);
    spectrogram_->setFrequencyScale(preferences_.frequencyScale);
    spectrogram_->setFloorDecibels(static_cast<float>(preferences_.spectrogramFloorDb));
    meters_->setTarget(preferences_.loudnessTarget);
    analysis_->setBounds(preferences_.analysisSeconds, preferences_.keySeconds);
    refreshActions();
}

std::vector<Rect> MainWindow::attachedScreens() const {
    std::vector<Rect> screens;
    const QList<QScreen*> attached = QGuiApplication::screens();
    screens.reserve(static_cast<std::size_t>(attached.size()));
    for (const QScreen* screen : attached) {
        // The available area rather than the whole screen: a window restored
        // under the taskbar is a window with its title bar under the taskbar.
        const QRect area = screen->availableGeometry();
        screens.push_back(Rect{area.x(), area.y(), area.width(), area.height()});
    }
    return screens;
}

void MainWindow::applySavedLayout(const SavedSettings& saved) {
    // Two panes above, three beside. The counts are given rather than taken
    // from the splitters, because a stored list that happens to be the wrong
    // length is exactly what this is checking for.
    if (splitter_ != nullptr && splitterSizesUsable(saved.mainSplit, 2)) {
        splitter_->setSizes(QList<int>{saved.mainSplit.begin(), saved.mainSplit.end()});
    }
    if (sideSplitter_ != nullptr && splitterSizesUsable(saved.sideSplit, 3)) {
        sideSplitter_->setSizes(QList<int>{saved.sideSplit.begin(), saved.sideSplit.end()});
    }

    if (!saved.window) {
        return;
    }

    const Rect placed = confineToScreens(saved.window->frame, attachedScreens());
    setGeometry(placed.x, placed.y, placed.width, placed.height);
    restoredFrame_ = placed;
    restoredMaximised_ = saved.window->maximised;
    if (saved.window->maximised) {
        // Set rather than shown: the window is not visible yet -- whoever
        // constructed it decides when it appears -- and a state set now is the
        // state it appears in, with the geometry above as the size it returns
        // to when it is un-maximised.
        setWindowState(windowState() | Qt::WindowMaximized);
    }
    restoredWindow_ = true;
}

Rect MainWindow::normalFrame() const {
    // geometry() rather than frameGeometry(), because setGeometry() is what
    // puts it back: saving one and restoring through the other is how a window
    // creeps down the screen by its own title bar height on every launch.
    const QRect live = isMaximized() ? normalGeometry() : geometry();
    if (live.isValid() && live.width() > 0 && live.height() > 0) {
        return Rect{live.x(), live.y(), live.width(), live.height()};
    }
    // Maximised, and never shown any other way. The rectangle the restore
    // applied is the best answer anyone has to where this window goes when it
    // is not maximised, and it is a great deal better than nothing -- nothing
    // means the next launch opens at the default size, in the default place,
    // and not maximised either.
    if (restoredFrame_) {
        return *restoredFrame_;
    }
    const QRect fallback = geometry();
    return Rect{fallback.x(), fallback.y(), fallback.width(), fallback.height()};
}

void MainWindow::saveSettings() {
    // No file is not a failure to write one: a batch run without --settings
    // has asked to keep nothing, and saying so on stderr every time would make
    // a clean run look broken.
    if (!mayWriteSettings_ || settingsFile_.path.empty()) {
        return;
    }

    SavedSettings settings;
    settings.preferences = preferences_;
    settings.recent = recent_;
    if (meters_ != nullptr) {
        // The panel's combo owns the target; the preference is a copy of its
        // answer, taken here so that a change made in the panel is saved
        // whether or not anything told the window about it.
        settings.preferences.loudnessTarget = meters_->target();
    }

    // A window that has never been shown has never been laid out either: its
    // splitters answer with a handful of pixels each and its geometry is
    // whatever the constructor asked for. That is not a layout anybody chose,
    // and saving it would mean that opening a file from the command line and
    // then being killed -- which saves, because the recent list is saved at
    // once -- quietly replaced the layout of every launch before it.
    //
    // So a run that has not shown the window writes back what it read.
    if (isVisible()) {
        const Rect frame = normalFrame();
        if (frame.width > 0 && frame.height > 0) {
            WindowPlacement placement;
            placement.frame = frame;
            placement.maximised = isMaximized();
            settings.window = placement;
        }

        // A reading that is not a usable layout falls back to the stored one
        // rather than to nothing: losing a layout somebody dragged into place
        // because the widget answered oddly once would be the same failure
        // this whole branch exists to avoid.
        const auto sizesOf = [](const QSplitter* splitter, std::size_t panes,
                                const std::vector<int>& stored) {
            std::vector<int> held;
            if (splitter != nullptr) {
                const QList<int> sizes = splitter->sizes();
                held.assign(sizes.begin(), sizes.end());
            }
            return splitterSizesUsable(held, panes) ? held : stored;
        };
        settings.mainSplit = sizesOf(splitter_, 2, loadedMainSplit_);
        settings.sideSplit = sizesOf(sideSplitter_, 3, loadedSideSplit_);
    } else {
        if (restoredFrame_) {
            settings.window = WindowPlacement{*restoredFrame_, restoredMaximised_};
        }
        settings.mainSplit = loadedMainSplit_;
        settings.sideSplit = loadedSideSplit_;
    }

    const SettingsStore store{settingsFile_.path};
    if (!store.write(writeSettings(settings))) {
        // Nothing to show: by the time this runs on the way out, the status
        // bar is going with it. On stderr so that a run started from a
        // terminal says something -- which is how a portable copy in a folder
        // its user cannot write to would be diagnosed.
        std::fprintf(stderr, "could not write settings to %s\n",
                     toUtf8(settingsFile_.path).c_str());
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    settingsWritten_ = true;
    QMainWindow::closeEvent(event);
}

void MainWindow::choosePreferences() {
    const std::optional<Preferences> chosen =
        PreferencesDialog::ask(this, preferences_, settingsFile_.path, settingsFile_.home);
    if (!chosen) {
        return;
    }

    const bool analysisMoved =
        chosen->fftSize != preferences_.fftSize || chosen->hopSize != preferences_.hopSize;
    preferences_ = *chosen;
    applyPreferences();
    // Saved at once rather than at the next close: a preference somebody has
    // just set is the one thing they would most notice losing to a crash.
    saveSettings();

    if (analysisMoved) {
        // The only preference here that is not a way of drawing what has
        // already been analysed: a different window or hop is a different
        // spectrogram, and it has to be built again.
        spectrogramConfig_ = displayConfig(preferences_);
        startSpectrogramBuild();
    }
    // The bounds and the loudness target feed the panels rather than the
    // picture, so they are answered by measuring again.
    reanalyseNow();
    updateStatus();
}

void MainWindow::rebuildRecentMenu() {
    if (recentMenu_ == nullptr) {
        return;
    }
    recentMenu_->clear();

    if (recent_.empty()) {
        QAction* nothing = recentMenu_->addAction(tr("Nothing opened yet"));
        nothing->setEnabled(false);
        return;
    }

    int number = 1;
    for (const std::filesystem::path& path : recent_.paths()) {
        const QString name = QString::fromStdString(toUtf8(path));
        // &1 to &9 and then no mnemonic, because &10 is not one: Qt would
        // read it as "1" followed by a zero and give two entries the same key.
        QAction* action = recentMenu_->addAction(
            number <= 9 ? tr("&%1  %2").arg(number).arg(name) : name, this, [this, path] {
                if (openPath(path)) {
                    return;
                }
                // Dropped now, and only now. A file that is merely absent
                // stays on the list -- a recording on a drive that is not
                // plugged in this morning is the case the list is most useful
                // for -- but one that has actually failed to open has earned
                // its removal.
                recent_.forget(path);
                saveSettings();
                refreshActions();
            });

        // Greyed rather than pruned. Pruning at load would empty the list of
        // everything on a network share that happened to be down, or on a
        // stick that happened to be out, and those are the entries a person
        // most wants to still be there when it comes back. The cost is a stat
        // per entry each time this menu opens, which for a path on a
        // disconnected share can be slow -- and is paid by the person who
        // opened the menu rather than by everyone at launch.
        std::error_code failed;
        const bool there = std::filesystem::exists(path, failed) && !failed;
        action->setEnabled(there);
        if (!there) {
            action->setStatusTip(tr("%1 is not where it was").arg(name));
        }
        ++number;
    }

    recentMenu_->addSeparator();
    recentMenu_->addAction(tr("&Clear the list"), this, [this] {
        recent_.clear();
        saveSettings();
        refreshActions();
    });
}

void MainWindow::rememberRecent(const std::filesystem::path& path) {
    std::error_code failed;
    // Absolute, so that one file opened from two working directories is one
    // entry -- and so that it is still findable next launch, whose working
    // directory is wherever the shortcut points rather than wherever a
    // terminal was.
    std::filesystem::path full = std::filesystem::absolute(path, failed);
    if (failed) {
        full = path;
    }
    recent_.remember(full);
    saveSettings();
    refreshActions();
}

bool MainWindow::openPath(const std::filesystem::path& path) {
    return isSessionPath(path) ? openSession(path) : openFile(path);
}

bool MainWindow::printSettings() const {
    const auto printName = [](const char* key, std::string_view name) {
        std::printf("%s=%.*s\n", key, static_cast<int>(name.size()), name.data());
    };

    std::printf("settings_file=%s\n", toUtf8(settingsFile_.path).c_str());
    std::printf("settings_portable=%d\n", settingsFile_.home == SettingsHome::Portable ? 1 : 0);
    std::printf("settings_writeback=%d\n", mayWriteSettings_ ? 1 : 0);

    // The live geometry rather than what the file said, because the claim
    // being checked is that the file reached the window -- including the case
    // where the confining rule decided the file was asking for somewhere
    // nobody could see.
    const Rect frame = normalFrame();
    std::printf("window_restored=%d\nwindow_x=%d\nwindow_y=%d\nwindow_width=%d\n"
                "window_height=%d\nwindow_maximised=%d\n",
                restoredWindow_ ? 1 : 0, frame.x, frame.y, frame.width, frame.height,
                isMaximized() ? 1 : 0);

    // The screens the confining rule was given, so that a driver can hold the
    // window to them rather than to numbers somebody typed into a test. What
    // counts as off-screen depends entirely on what is attached.
    const std::vector<Rect> screens = attachedScreens();
    std::printf("screen_count=%d\n", static_cast<int>(screens.size()));
    int screenNumber = 0;
    for (const Rect& screen : screens) {
        std::printf("screen%d=%d,%d,%d,%d\n", screenNumber, screen.x, screen.y, screen.width,
                    screen.height);
        ++screenNumber;
    }

    const auto printSizes = [](const char* key, const QSplitter* splitter) {
        if (splitter == nullptr) {
            return;
        }
        const QList<int> sizes = splitter->sizes();
        std::string joined;
        for (const int size : sizes) {
            if (!joined.empty()) {
                joined += ',';
            }
            joined += std::to_string(size);
        }
        std::printf("%s=%s\n", key, joined.c_str());
    };
    printSizes("split_main", splitter_);
    printSizes("split_side", sideSplitter_);

    // Read back out of the widgets wherever a widget holds the answer, for the
    // same reason as the geometry: a preference that was parsed correctly and
    // then applied to nothing would pass a check made against the struct.
    printName("pref_exportformat", toName(preferences_.exportFormat));
    printName("pref_dither", toName(preferences_.dither));
    printName("pref_fadeshape", toName(preferences_.fadeShape));
    printName("pref_loudnesstarget", toName(meters_->target()));
    printName("pref_frequencyscale", toName(spectrogram_->frequencyScale()));
    printName("pref_colourmap", toName(spectrogram_->colourmap()));
    std::printf("pref_floordb=%.1f\n", static_cast<double>(spectrogram_->floorDecibels()));
    std::printf("pref_fftsize=%d\npref_hopsize=%d\n", spectrogramConfig_.fftSize,
                spectrogramConfig_.hopSize);
    std::printf("pref_analysisseconds=%.0f\npref_keyseconds=%.0f\n", analysis_->mostSeconds(),
                analysis_->keySeconds());
    std::printf("pref_beatgrid=%d\npref_pitchcontour=%d\npref_octavebands=%d\npref_room=%d\n",
                preferences_.showBeatGrid ? 1 : 0, preferences_.showPitchContour ? 1 : 0,
                preferences_.showOctaveBands ? 1 : 0, preferences_.measureRoom ? 1 : 0);

    std::printf("recent_count=%d\n", static_cast<int>(recent_.size()));
    int index = 1;
    for (const std::filesystem::path& path : recent_.paths()) {
        std::printf("recent%d=%s\n", index, toUtf8(path).c_str());
        ++index;
    }
    std::fflush(stdout);
    return true;
}

void MainWindow::captureSpectrumReference() {
    // The spectrum is computed behind the same quarter-second timer as the
    // meters, so a batch run -- where a selection and this verb arrive in the
    // same instant -- has nothing on screen yet to keep. Flush it first, the
    // way normalising flushes the measurement it is about to use. Interactively
    // this is a no-op, because by the time anyone reaches the menu the timer
    // has long since fired.
    if (analysisTimer_ != nullptr && analysisTimer_->isActive()) {
        reanalyseNow();
    }

    // Whatever is on screen, not a fresh measurement of the selection. The
    // curve shown is the curve being compared against, and taking it from the
    // panel means the two are the same thing by construction rather than by
    // two code paths agreeing.
    if (!spectrum_ || !spectrum_->captureReference()) {
        status_->setText(tr("There is no spectrum to keep as a reference yet"));
        return;
    }
    status_->setText(tr("Spectrum reference set; later selections are drawn against it"));
    refreshActions();
}

void MainWindow::clearSpectrumReference() {
    if (spectrum_) {
        spectrum_->clearReference();
    }
    status_->setText(tr("Spectrum reference cleared"));
    refreshActions();
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
    reanalyseSoon();
}

void MainWindow::reanalyseSoon() {
    if (analysisTimer_) {
        analysisTimer_->start();
    }
}

void MainWindow::reanalyseNow() {
    if (analysisTimer_) {
        analysisTimer_->stop();
    }
    remeasure();
    remeasureMusical();
    respectrum();
}

void MainWindow::remeasureMusical() {
    if (analysis_ == nullptr) {
        return;
    }
    if (!documentSource_ || document_.duration() <= 0) {
        analysis_->clear();
        waveform_->clearOverlays();
        spectrum_->setOctaveBands({});
        return;
    }

    AnalysisPanel::Request request;
    request.pitch = preferences_.showPitchContour;
    request.room = preferences_.measureRoom;

    const TimeSelection selected = selection();
    if (selected.isEmpty()) {
        analysis_->analyse(documentSource_, 0, document_.duration(), tr("whole document"), request);
    } else {
        analysis_->analyse(documentSource_, selected.start, selected.length(), tr("selection"),
                           request);
    }
}

void MainWindow::showMusicalAnalysis() {
    const MusicalAnalysis* result = analysis_ == nullptr ? nullptr : analysis_->latest();
    if (result == nullptr) {
        waveform_->clearOverlays();
        spectrum_->setOctaveBands({});
        return;
    }

    // The grid is drawn only where there is a tempo to draw. A refused tempo
    // leaves the waveform bare, which is the same answer the panel gives in
    // words, and is not the same picture as a grid nobody can see.
    if (preferences_.showBeatGrid && result->hasBeats()) {
        waveform_->setBeatGrid(result->tempo.beatSeconds, result->start,
                               result->tempo.confidence < kTempoDoubtfulBelow);
    } else {
        waveform_->setBeatGrid({}, result->start, false);
    }

    if (preferences_.showPitchContour && result->pitchRequested) {
        const analysis::PitchSettings settings;
        waveform_->setPitchContour(result->pitch, result->start, settings.minHz, settings.maxHz);
    } else {
        waveform_->setPitchContour({}, result->start, 0.0, 0.0);
    }

    // Where the analysis stopped short of what was asked about, so that a grid
    // ending part-way along the file reads as a bound and not as a failure.
    waveform_->setAnalysedEnd(result->analysedFrames < result->requestedFrames
                                  ? result->start + result->analysedFrames
                                  : -1);

    spectrum_->setOctaveBands(preferences_.showOctaveBands ? result->bands
                                                           : std::vector<analysis::Band>{});
}

void MainWindow::respectrum() {
    if (!spectrum_) {
        return;
    }
    if (!documentSource_ || document_.duration() <= 0) {
        spectrum_->clear();
        spectrum_->setNote(tr("Nothing open"));
        return;
    }

    const TimeSelection range = targetRange();
    auto made = analysis::SpectrumAnalyser::create(document_.sampleRate());
    if (!made) {
        spectrum_->setNote(tr("Could not analyse"));
        return;
    }
    analysis::SpectrumAnalyser& analyser = made.value();

    // A bounded amount of reading, whatever is selected. An average spectrum is
    // characterised by a few hundred frames, so reading two hours to compute
    // one would make selecting anything feel broken and would say the same
    // thing. A long selection is sampled in stretches spread across it, with a
    // segment boundary between them so the joins are not analysed as audio.
    constexpr SampleCount kStretch = 1 << 17; // About 2.7 s at 48 kHz.
    constexpr int kMostStretches = 24;

    const SampleCount length = range.length();
    const int stretches = length <= kStretch
                              ? 1
                              : static_cast<int>(std::min<SampleCount>(
                                    kMostStretches, (length + kStretch - 1) / kStretch));

    AudioBuffer block{document_.layout(), std::min<SampleCount>(kStretch, length)};
    for (int i = 0; i < stretches; ++i) {
        const SampleIndex at =
            stretches > 1
                ? range.start + static_cast<SampleIndex>(
                                    static_cast<double>(length - block.frames()) *
                                    static_cast<double>(i) / static_cast<double>(stretches - 1))
                : range.start;
        if (!documentSource_->read(at, block.view())) {
            break;
        }
        if (i > 0) {
            analyser.startSegment();
        }
        // The first channel only. A spectrum of a stereo sum shows a comb
        // wherever the two channels disagree in phase, which is a picture of
        // the summing rather than of the material.
        analyser.add(block.constView(), 0);
    }

    if (analyser.frameCount() == 0) {
        spectrum_->clear();
        spectrum_->setNote(tr("Too short for a spectrum"));
        return;
    }
    spectrum_->setSpectrum(analyser.averageDb(), analyser.peakDb(), document_.sampleRate(),
                           analyser.fftSize());
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
                                                      tr("Auscultate session (*.sa)"));
    if (!path.isEmpty()) {
        saveSession(path.toStdString());
    }
}

void MainWindow::chooseOpenSession() {
    const QString path =
        QFileDialog::getOpenFileName(this, tr("Open session"), {}, tr("Auscultate session (*.sa)"));
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
    setWindowTitle(tr("%1 — Auscultate").arg(QString::fromStdString(path.filename().string())));
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
    setWindowTitle(tr("%1 — Auscultate").arg(QString::fromStdString(path.filename().string())));
    rememberRecent(path);
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
    setWindowTitle(tr("%1 — Auscultate").arg(name));
    rememberRecent(path);
    return true;
}

void MainWindow::rebuildCaches() {
    // Both halves of the spectrum panel move with the document: the frequency
    // axis it is drawn on, and the rate the EQ bands are designed at. Done
    // here rather than on open because a session can bring a different rate
    // in, and done before the first transform because the curve is editable
    // straight away.
    spectrum_->setSampleRate(document_.sampleRate());
    spectrum_->setEqSampleRate(document_.sampleRate());

    // The overlays belong to the document that has just been replaced. A beat
    // grid is a list of instants, and after a cut those instants are somewhere
    // else in the audio -- so it goes now rather than when its replacement
    // lands. A selection change does not come through here, and does not clear
    // them: the document is the same, so the old grid is over the right audio
    // until a better one arrives.
    waveform_->clearOverlays();
    spectrum_->setOctaveBands({});

    documentSource_ = std::make_shared<const engine::DocumentSource>(document_);
    peaks_.reset();
    spectra_.reset();
    spectrogramNote_.clear();
    spectrogramResolutionNote_.clear();

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
    // No length limit and no coarsening any more.
    //
    // Both existed because the whole pyramid had to be resident: a three-hour
    // recording needed more than a gigabyte, so the window first refused it and
    // then, less badly, analysed it at a coarser hop and said so. The tiled
    // cache holds a decimated overview plus whatever detail the view is
    // actually over, so the resident cost no longer grows with the file and the
    // picture is at full resolution wherever the user is looking.
    spectrogramConfig_ = displayConfig(preferences_);
    spectrogramResolutionNote_.clear();

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

    // Whatever detail was being fetched belongs to the previous document.
    cancelDetailFetch();

    const std::uint64_t mine = spectrogramGeneration_->load();
    spectrogramBusy_ = true;
    spectrogramNote_ = tr("building the spectrogram…");

    // The source is captured by shared_ptr and the token by pointer into this
    // window, which outlives the worker because cancelSpectrogramBuild joins it
    // before anything replaces either.
    spectrogramWorker_ = std::thread{[this, source = documentSource_, config = spectrogramConfig_,
                                      mine, generation = spectrogramGeneration_] {
        JobMonitor monitor;
        monitor.cancellation = &spectrogramCancellation_;

        spectral::SpectrogramTiles::Settings settings;
        settings.config = config;
        // Decimate only as much as the length requires. A short file comes out
        // at decimation zero, where the overview is the ordinary pyramid and
        // the picture is exactly what it was before the cache existed.
        constexpr std::size_t kOverviewBudgetBytes = 96u << 20;
        settings.coarseLevel = spectral::SpectrogramTiles::coarseLevelFor(
            source->info().frameCount, config, kOverviewBudgetBytes);
        // Tile boundaries have to land on overview-frame boundaries.
        settings.tileFrames = std::max<SampleCount>(SampleCount{1} << settings.coarseLevel, 1024);
        auto made = spectral::SpectrogramTiles::create(source, 0, settings);
        Result<spectral::SpectrogramTiles> built = std::move(made);
        if (built) {
            // Only the overview here. Detail follows the view, and asking for
            // all of it would be the eager pyramid again under another name.
            if (const Status status = built.value().buildOverview(monitor); !status) {
                built = status.error();
            }
        }
        if (generation->load() != mine) {
            return;
        }

        QMetaObject::invokeMethod(
            this,
            [this, built = std::make_shared<Result<spectral::SpectrogramTiles>>(std::move(built)),
             mine, generation] {
                if (generation->load() != mine) {
                    return;
                }
                if (*built) {
                    spectra_ =
                        std::make_shared<spectral::SpectrogramTiles>(std::move(*built).value());
                    spectrogramNote_.clear();
                } else {
                    spectra_.reset();
                    spectrogramNote_ =
                        tr("spectrogram analysis failed: %1")
                            .arg(QString::fromStdString(std::string{built->error().what()}));
                }
                spectrogramBusy_ = false;
                spectrogram_->setTiles(spectra_, document_.sampleRate(), document_.duration());
                spectrogram_->setViewRange(waveform_->viewStart(), waveform_->viewLength());
                spectrogram_->setSelection(selection());
                requestSpectrogramDetail();
                updateStatus();
            },
            Qt::QueuedConnection);
    }};
}

void MainWindow::cancelDetailFetch() {
    detailCancellation_.cancel();
    if (detailWorker_.joinable()) {
        detailWorker_.join();
    }
    detailCancellation_.reset();
}

void MainWindow::requestSpectrogramDetailSoon() {
    if (detailTimer_ != nullptr) {
        detailTimer_->start();
    }
}

void MainWindow::requestSpectrogramDetail() {
    if (detailTimer_ != nullptr) {
        detailTimer_->stop();
    }
    if (!spectra_ || !spectra_->hasOverview()) {
        return;
    }

    const SampleIndex start = spectrogram_->viewStart();
    const SampleCount length = spectrogram_->viewLength();
    if (length <= 0) {
        return;
    }
    // Nothing to fetch when a column is already wider than the overview's own
    // hop: detail would be folded away in the drawing.
    const int columns = std::max(1, spectrogram_->width());
    if (!spectra_->detailWorthwhile(length / columns)) {
        return;
    }

    // Half a screen either side, so a scroll of less than that lands on tiles
    // that are already there.
    const SampleCount margin = length / 2;
    const SampleIndex from = std::max<SampleIndex>(0, start - margin);
    const SampleIndex to = std::min<SampleIndex>(document_.duration(), start + length + margin);

    // One fetch at a time. Superseding the previous one is the point: the range
    // it was working on is not the range being looked at any more.
    cancelDetailFetch();

    detailWorker_ = std::thread{[this, tiles = spectra_, from, to] {
        JobMonitor monitor;
        monitor.cancellation = &detailCancellation_;
        // The result is deliberately ignored. A cancelled fetch is the normal
        // case, and a failed one leaves the overview on screen, which is what
        // the display falls back to anyway.
        (void)tiles->ensureDetail(from, to, monitor);

        QMetaObject::invokeMethod(this, [this] { spectrogram_->update(); }, Qt::QueuedConnection);
    }};
}

void MainWindow::refreshViews() {
    const TimeSelection previous = selection();

    ruler_->setSampleRate(document_.sampleRate());
    waveform_->setPyramid(peaks_, document_.sampleRate());
    spectrogram_->setTiles(spectra_, document_.sampleRate(), document_.duration());

    // setTiles resets the view to the whole document, which is right on open
    // and wrong after an edit. Put the range and the selection back.
    const TimeSelection clamped{std::min(previous.start, document_.duration()),
                                std::min(previous.end, document_.duration())};
    waveform_->setSelection(clamped);
    spectrogram_->setSelection(clamped);
    ruler_->setSelection(clamped);
    ruler_->setViewRange(waveform_->viewStart(), waveform_->viewLength());
    spectrogram_->setViewRange(waveform_->viewStart(), waveform_->viewLength());

    std::vector<TimeRuler::Mark> marks;
    marks.reserve(document_.markers().size());
    for (const engine::Marker& marker : document_.markers()) {
        marks.push_back(
            TimeRuler::Mark{marker.position, marker.length, QString::fromStdString(marker.label)});
    }
    ruler_->setMarkers(std::move(marks));

    refreshActions();
    updateStatus();
    reanalyseSoon();
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
    if (clearReferenceAction_ != nullptr) {
        clearReferenceAction_->setEnabled(spectrum_ != nullptr && spectrum_->hasReference());
    }
    pasteAction_->setEnabled(document && clipboard_.frames() > 0);
    // Enabled whenever there is a document, not only once a measurement has
    // landed. The action waits for the measurement itself and says what it
    // found, which is more useful than a menu item that is mysteriously grey
    // for the first second after opening a file -- and with the measurement
    // now deferred behind a timer, that second became indefinite while the
    // user sat still.
    normaliseAction_->setEnabled(document);
    attenuateAction_->setEnabled(document);
    healAction_->setEnabled(document);
    denoiseAction_->setEnabled(document && !noiseProfile_.isEmpty());

    if (applyEqAction_ != nullptr) {
        // A curve of bands all sitting at 0 dB would be an edit that changes
        // nothing but still costs an undo step, so it is not offered.
        applyEqAction_->setEnabled(document && !spectrum_->curve().isFlat());
        resetEqAction_->setEnabled(spectrum_->curve().bandCount() > 0);
        showEqAction_->setChecked(spectrum_->isEqVisible());
    }

    // The ticks follow the flags rather than the other way round, so that a
    // batch verb, a menu press and a restored settings file all leave the menu
    // saying the same thing.
    for (const auto& [action, wanted] : preferenceTicks_) {
        action->setChecked(wanted());
    }
    if (recentMenu_ != nullptr) {
        recentMenu_->setEnabled(!recent_.empty());
    }
    if (beatGridAction_ != nullptr) {
        beatGridAction_->setChecked(preferences_.showBeatGrid);
        pitchContourAction_->setChecked(preferences_.showPitchContour);
        octaveBandsAction_->setChecked(preferences_.showOctaveBands);
        roomAction_->setChecked(preferences_.measureRoom);
    }
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

bool MainWindow::applyChannelOp(dsp::ChannelOp operation, const QString& label) {
    const TimeSelection range = targetRange();
    if (range.isEmpty() || !documentSource_) {
        return false;
    }
    // Checked before reading anything, so the refusal costs nothing and can
    // say why rather than just failing.
    const int channels = document_.layout().count();
    if (dsp::channelOpNeedsStereo(operation) && channels != 2) {
        status_->setText(
            tr("That needs a stereo file; this one has %n channel(s)", nullptr, channels));
        return false;
    }

    AudioBuffer span{document_.layout(), range.length()};
    if (!documentSource_->read(range.start, span.view())) {
        status_->setText(tr("Could not read the selection"));
        return false;
    }

    if (const Status status = dsp::applyChannelOp(span.view(), operation); !status) {
        status_->setText(QString::fromUtf8(status.error().what()));
        return false;
    }

    return applyEdit(label, [this, &range, &span] {
        return engine::replaceRange(document_, range.start, std::move(span)).ok();
    });
}

int MainWindow::nearestMarker() const noexcept {
    const auto& markers = document_.markers();
    if (markers.empty()) {
        return -1;
    }
    const SampleIndex caret = selection().start;
    int best = 0;
    SampleCount closest = std::abs(markers[0].position - caret);
    for (std::size_t i = 1; i < markers.size(); ++i) {
        const SampleCount distance = std::abs(markers[i].position - caret);
        if (distance < closest) {
            closest = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool MainWindow::addMarker(const QString& label) {
    if (!hasDocument()) {
        return false;
    }
    const TimeSelection selected = selection();
    // A selection makes a region, a caret makes a point. Both are the same
    // record; the length is what distinguishes them, and asking the user which
    // they meant when they have already shown you would be a question with an
    // answer already on screen.
    engine::Marker marker;
    marker.position = std::clamp<SampleIndex>(selected.start, 0, document_.duration());
    marker.length = selected.isEmpty() ? 0 : selected.length();
    marker.label = label.toStdString();

    return applyEdit(tr("add marker"), [this, marker = std::move(marker)]() mutable {
        document_.markers().push_back(std::move(marker));
        // Kept in time order, so "next" and "previous" mean what they say
        // however they were added.
        std::sort(document_.markers().begin(), document_.markers().end(),
                  [](const engine::Marker& a, const engine::Marker& b) {
                      return a.position < b.position;
                  });
        return true;
    });
}

void MainWindow::chooseAddMarker() {
    if (!hasDocument()) {
        return;
    }
    // Numbered from the count rather than from the highest number used, which
    // would need parsing labels the user may have rewritten.
    const QString suggested = tr("Marker %1").arg(document_.markers().size() + 1);
    bool accepted = false;
    const QString label = QInputDialog::getText(this, tr("Add marker"), tr("Label:"),
                                                QLineEdit::Normal, suggested, &accepted);
    if (accepted) {
        (void)addMarker(label);
    }
}

void MainWindow::renameMarker() {
    const int index = nearestMarker();
    if (index < 0) {
        status_->setText(tr("There are no markers"));
        return;
    }
    bool accepted = false;
    const QString label = QInputDialog::getText(
        this, tr("Rename marker"), tr("Label:"), QLineEdit::Normal,
        QString::fromStdString(document_.markers()[static_cast<std::size_t>(index)].label),
        &accepted);
    if (!accepted) {
        return;
    }
    (void)applyEdit(tr("rename marker"), [this, index, label] {
        document_.markers()[static_cast<std::size_t>(index)].label = label.toStdString();
        return true;
    });
}

bool MainWindow::deleteNearestMarker() {
    const int index = nearestMarker();
    if (index < 0) {
        status_->setText(tr("There are no markers"));
        return false;
    }
    return applyEdit(tr("delete marker"), [this, index] {
        auto& markers = document_.markers();
        markers.erase(markers.begin() + index);
        return true;
    });
}

bool MainWindow::clearMarkers() {
    if (!hasDocument() || document_.markers().empty()) {
        return false;
    }
    return applyEdit(tr("clear markers"), [this] {
        document_.markers().clear();
        return true;
    });
}

void MainWindow::goToMarker(bool forwards) {
    const auto& markers = document_.markers();
    if (markers.empty()) {
        status_->setText(tr("There are no markers"));
        return;
    }

    const SampleIndex caret = selection().start;
    const engine::Marker* found = nullptr;
    if (forwards) {
        for (const engine::Marker& marker : markers) {
            if (marker.position > caret) {
                found = &marker;
                break;
            }
        }
    } else {
        for (auto it = markers.rbegin(); it != markers.rend(); ++it) {
            if (it->position < caret) {
                found = &*it;
                break;
            }
        }
    }
    if (found == nullptr) {
        status_->setText(forwards ? tr("No marker after here") : tr("No marker before here"));
        return;
    }

    // Selecting the region rather than just moving the caret, where the marker
    // has one: a region marker exists to be acted on, and arriving at it with
    // it already selected is the point.
    const double rate = document_.sampleRate().hz();
    const double start = static_cast<double>(found->position) / rate;
    selectSeconds(start, found->length > 0
                             ? static_cast<double>(found->position + found->length) / rate
                             : start);
    // Bring it into view if it is not already, centred so there is context on
    // both sides; leave the view alone when it is, because scrolling under
    // someone who can already see the thing they asked for is disorienting.
    const SampleIndex viewStart = waveform_->viewStart();
    const SampleCount viewLength = waveform_->viewLength();
    if (found->position < viewStart || found->position >= viewStart + viewLength) {
        waveform_->scrollBySamples(found->position - viewLength / 2 - viewStart);
    }
    status_->setText(found->label.empty() ? tr("Marker at %1 s").arg(start, 0, 'f', 3)
                                          : QString::fromStdString(found->label));
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
    dsp::EqBand band;
    band.filter.type = static_cast<dsp::FilterType>(filterType);
    band.filter.frequency = frequency;
    band.filter.q = q;
    band.filter.gainDb = gainDb;
    (void)applyEqBands({band}, label);
}

bool MainWindow::applyEqBands(const std::vector<dsp::EqBand>& bands, const QString& label) {
    const TimeSelection range = targetRange();
    if (bands.empty() || range.isEmpty() || !documentSource_) {
        return false;
    }

    const double rate = document_.sampleRate().hz();
    // The lowest band decides the run-up and the blend, because it is the one
    // with the longest memory and the widest step to hide. Taking any other
    // would settle the slowest filter in the chain least.
    double lowest = rate * 0.5;
    for (const dsp::EqBand& band : bands) {
        lowest = std::min(lowest, band.filter.frequency);
    }
    const double period = rate / std::max(lowest, 1.0);

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
        return false;
    }

    const int channels = document_.layout().count();
    const SampleCount offset = range.start - spanStart;

    // The range as it stands, kept back for the edge blends below.
    AudioBuffer original{document_.layout(), range.length()};
    for (int channel = 0; channel < channels; ++channel) {
        std::copy_n(span.channel(channel) + offset, original.frames(), original.channel(channel));
    }

    // One EQ per channel, built fresh so that each starts from cleared state
    // and the channels cannot inherit each other's ringing.
    for (int channel = 0; channel < channels; ++channel) {
        auto eq = dsp::ParametricEq::create(document_.sampleRate());
        if (!eq) {
            status_->setText(tr("Could not build the filter: %1")
                                 .arg(QString::fromStdString(std::string{eq.error().what()})));
            return false;
        }
        for (const dsp::EqBand& band : bands) {
            if (!eq.value().addBand(band)) {
                status_->setText(tr("Those filter settings are not usable"));
                return false;
            }
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

    return applyEdit(label, [this, &range, &applied] {
        return engine::replaceRange(document_, range.start, std::move(applied)).ok();
    });
}

void MainWindow::setEqCurveVisible(bool visible) {
    spectrum_->setEqVisible(visible);
    // The menu's tick follows the panel rather than the other way round, so
    // that the two cannot disagree after a verb has shown the curve without
    // going through the action.
    refreshActions();
}

bool MainWindow::applyEqCurve() {
    if (!hasDocument() || spectrum_->curve().isFlat()) {
        return false;
    }
    const QString summary = QString::fromStdString(spectrum_->curve().summarise());
    if (!applyEqBands(spectrum_->bands(), tr("EQ: %1").arg(summary))) {
        return false;
    }
    status_->setText(tr("EQ applied (%1). The curve is still up, so applying it again "
                        "applies it twice.")
                         .arg(summary));
    return true;
}

void MainWindow::resetEqCurve() {
    spectrum_->clearBands();
    refreshActions();
}

bool MainWindow::printEqBands() const {
    for (int index = 0; index < spectrum_->curve().bandCount(); ++index) {
        const dsp::EqBand* band = spectrum_->curve().band(index);
        std::printf("eq_band%d_hz=%.4f\neq_band%d_gain_db=%.4f\neq_band%d_q=%.4f\n", index,
                    band->filter.frequency, index, band->filter.gainDb, index, band->filter.q);
    }
    std::printf("eq_bands=%d\n", spectrum_->curve().bandCount());
    std::fflush(stdout);
    return true;
}

bool MainWindow::applyOverRange(
    const QString& label, double attackSeconds, double releaseSeconds,
    const std::function<Status(AudioBufferView, SampleCount, SampleCount)>& apply) {
    const TimeSelection range = targetRange();
    if (range.isEmpty() || !documentSource_) {
        return false;
    }

    // Audio before the selection for the envelope to settle on, and a blend at
    // each end so the gain the processor settled on does not meet the untouched
    // audio as a step. Both are the processor's own time constants: a slow
    // release needs a longer run-up and leaves a bigger step to spread.
    const SampleRate rate = document_.sampleRate();
    const SampleCount runUp =
        std::min(dsp::dynamicsRunUp(rate, attackSeconds, releaseSeconds), range.start);
    const auto blend = std::min<SampleCount>(
        static_cast<SampleCount>(std::max(0.010, releaseSeconds) * rate.hz()), range.length() / 4);

    AudioBuffer span{document_.layout(), runUp + range.length()};
    if (!documentSource_->read(range.start - runUp, span.view())) {
        status_->setText(tr("Could not read the selection"));
        return false;
    }

    if (const Status status = apply(span.view(), runUp, blend); !status) {
        status_->setText(QString::fromUtf8(status.error().what()));
        return false;
    }

    // Only the selection is written back. The run-up was there to settle the
    // envelope, not to be applied to audio nobody selected.
    AudioBuffer applied{document_.layout(), range.length()};
    for (int channel = 0; channel < applied.channelCount(); ++channel) {
        std::copy_n(span.channel(channel) + runUp, applied.frames(), applied.channel(channel));
    }

    return applyEdit(label, [this, &range, &applied] {
        return engine::replaceRange(document_, range.start, std::move(applied)).ok();
    });
}

void MainWindow::chooseCompressor() {
    if (!hasDocument()) {
        return;
    }
    dsp::CompressorSettings settings;
    const std::vector<double> values = FieldDialog::ask(
        this, tr("Compressor"),
        {{tr("Threshold"), settings.thresholdDb, -80.0, 0.0, 1, 1.0, tr("dB")},
         {tr("Ratio"), settings.ratio, 1.0, 100.0, 1, 0.5, tr(": 1")},
         {tr("Attack"), settings.attackSeconds * 1000.0, 0.1, 500.0, 1, 1.0, tr("ms")},
         {tr("Release"), settings.releaseSeconds * 1000.0, 1.0, 5000.0, 0, 10.0, tr("ms")},
         {tr("Knee"), settings.kneeDb, 0.0, 24.0, 1, 1.0, tr("dB")},
         {tr("Makeup gain"), settings.makeupGainDb, -24.0, 24.0, 1, 0.5, tr("dB")}});
    if (values.size() != 6) {
        return;
    }

    settings.thresholdDb = values[0];
    settings.ratio = values[1];
    settings.attackSeconds = values[2] / 1000.0;
    settings.releaseSeconds = values[3] / 1000.0;
    settings.kneeDb = values[4];
    settings.makeupGainDb = values[5];

    const SampleRate rate = document_.sampleRate();
    (void)applyOverRange(
        tr("compress %1:1 at %2 dB")
            .arg(settings.ratio, 0, 'f', 1)
            .arg(settings.thresholdDb, 0, 'f', 1),
        settings.attackSeconds, settings.releaseSeconds,
        [rate, settings](AudioBufferView audio, SampleCount runUp, SampleCount blend) {
            return dsp::compressOffline(audio, rate, settings, runUp, blend);
        });
}

void MainWindow::chooseGate() {
    if (!hasDocument()) {
        return;
    }
    dsp::GateSettings settings;
    const std::vector<double> values = FieldDialog::ask(
        this, tr("Gate"),
        {{tr("Threshold"), settings.thresholdDb, -100.0, 0.0, 1, 1.0, tr("dB")},
         {tr("Hysteresis"), settings.hysteresisDb, 0.0, 24.0, 1, 1.0, tr("dB")},
         {tr("Attack"), settings.attackSeconds * 1000.0, 0.1, 200.0, 1, 0.5, tr("ms")},
         {tr("Hold"), settings.holdSeconds * 1000.0, 0.0, 2000.0, 0, 10.0, tr("ms")},
         {tr("Release"), settings.releaseSeconds * 1000.0, 1.0, 5000.0, 0, 10.0, tr("ms")},
         {tr("Depth"), settings.rangeDb, -120.0, 0.0, 1, 3.0, tr("dB")}});
    if (values.size() != 6) {
        return;
    }

    settings.thresholdDb = values[0];
    settings.hysteresisDb = values[1];
    settings.attackSeconds = values[2] / 1000.0;
    settings.holdSeconds = values[3] / 1000.0;
    settings.releaseSeconds = values[4] / 1000.0;
    settings.rangeDb = values[5];

    const SampleRate rate = document_.sampleRate();
    (void)applyOverRange(
        tr("gate at %1 dB").arg(settings.thresholdDb, 0, 'f', 1), settings.attackSeconds,
        settings.releaseSeconds,
        [rate, settings](AudioBufferView audio, SampleCount runUp, SampleCount blend) {
            return dsp::gateOffline(audio, rate, settings, runUp, blend);
        });
}

void MainWindow::chooseDeess() {
    if (!hasDocument()) {
        return;
    }
    dsp::DeessSettings settings;
    const std::vector<double> values = FieldDialog::ask(
        this, tr("De-ess"),
        {{tr("Sibilance above"), settings.frequencyHz, 1000.0, document_.sampleRate().hz() * 0.45,
          0, 250.0, tr("Hz")},
         {tr("Threshold"), settings.thresholdDb, -80.0, 0.0, 1, 1.0, tr("dB")},
         {tr("Ratio"), settings.ratio, 1.0, 40.0, 1, 0.5, tr(": 1")},
         {tr("Most it may remove"), settings.maximumReductionDb, 0.0, 30.0, 1, 1.0, tr("dB")}});
    if (values.size() != 4) {
        return;
    }
    settings.frequencyHz = values[0];
    settings.thresholdDb = values[1];
    settings.ratio = values[2];
    settings.maximumReductionDb = values[3];

    const SampleRate rate = document_.sampleRate();
    // Its report is worth showing: "nothing happened" and "it worked" look the
    // same in a waveform, and the fraction acted on is what says whether the
    // threshold is anywhere near right.
    dsp::DeessReport report;
    const bool applied = applyOverRange(
        tr("de-ess above %1 Hz").arg(settings.frequencyHz, 0, 'f', 0), settings.attackSeconds,
        settings.releaseSeconds,
        [rate, settings, &report](AudioBufferView audio, SampleCount runUp, SampleCount blend) {
            (void)blend;
            auto made = dsp::deess(audio, rate, settings, runUp);
            if (!made) {
                return Status{made.error()};
            }
            report = made.value();
            return Status{};
        });
    if (applied) {
        status_->setText(tr("De-essed: up to %1 dB off the top band, on %2% of the selection")
                             .arg(report.peakReductionDb, 0, 'f', 1)
                             .arg(100.0 * report.fractionReduced, 0, 'f', 0));
    }
}

void MainWindow::chooseDeclick() {
    if (!hasDocument()) {
        return;
    }
    bool accepted = false;
    const double threshold = QInputDialog::getDouble(
        this, tr("Remove clicks"),
        tr("Sensitivity — how far above the passage's own noise a sample has to be\n"
           "before it counts as damage. Lower finds more, and repairs more that\n"
           "did not need it:"),
        5.0, 2.0, 20.0, 1, &accepted);
    if (!accepted) {
        return;
    }
    (void)applyDeclick(threshold, tr("remove clicks"));
}

bool MainWindow::applyDeclick(double threshold, const QString& label) {
    const TimeSelection range = targetRange();
    if (range.isEmpty() || !documentSource_) {
        return false;
    }

    dsp::DeclickSettings settings;
    settings.threshold = threshold;

    // A run-up of exactly the model's order on each side, which is what makes
    // the selection's own first and last samples judgeable: the detector
    // ignores the first and last `order` samples of whatever it is given,
    // because their residual has no history behind it, and the repair needs
    // that much context either side to interpolate at all.
    const auto context = static_cast<SampleCount>(settings.order);
    const SampleIndex spanStart = std::max<SampleIndex>(0, range.start - context);
    const SampleIndex spanEnd = std::min<SampleIndex>(document_.duration(), range.end + context);

    AudioBuffer span{document_.layout(), spanEnd - spanStart};
    if (!documentSource_->read(spanStart, span.view())) {
        status_->setText(tr("Could not read the selection"));
        return false;
    }

    status_->setText(tr("Looking for clicks…"));
    status_->repaint();

    const auto report = dsp::declick(span.view(), settings);
    if (!report) {
        status_->setText(tr("Could not remove clicks: %1")
                             .arg(QString::fromStdString(std::string{report.error().what()})));
        return false;
    }
    if (report.value().clicks == 0 && report.value().tooLong == 0) {
        // Finding nothing is the right answer on undamaged audio, not a
        // failure. Returning false here failed a whole batch run on a clean
        // file, which is exactly the file a pipeline is most likely to hand it.
        status_->setText(tr("No clicks found"));
        return true;
    }

    AudioBuffer applied{document_.layout(), range.length()};
    const SampleCount offset = range.start - spanStart;
    for (int channel = 0; channel < document_.layout().count(); ++channel) {
        std::copy_n(span.channel(channel) + offset, applied.frames(), applied.channel(channel));
    }

    const bool changed = applyEdit(label, [this, &range, &applied] {
        return engine::replaceRange(document_, range.start, std::move(applied)).ok();
    });
    if (changed) {
        // The count is the point. A user needs to know whether it found three
        // clicks or three thousand, because those call for different next
        // steps, and whether it passed over anything too long to repair --
        // which is a dropout, and wants a different tool.
        QString note = tr("Removed %n click(s)", nullptr, report.value().clicks);
        if (report.value().tooLong > 0) {
            note += tr(" — %n stretch(es) were too long to repair and were left alone", nullptr,
                       report.value().tooLong);
        }
        status_->setText(note);
    }
    return changed;
}

void MainWindow::restoreClipping() {
    const TimeSelection range = targetRange();
    if (range.isEmpty() || !documentSource_) {
        return;
    }

    // The whole selection, with no run-up: unlike a filter or the declicker,
    // this fits its model around each flat top out of the material it is given,
    // and the level it detects the clipping at is the loudest sample in that
    // material. A run-up would move that level.
    AudioBuffer span{document_.layout(), range.length()};
    if (!documentSource_->read(range.start, span.view())) {
        status_->setText(tr("Could not read the selection"));
        return;
    }

    status_->setText(tr("Looking for clipping…"));
    status_->repaint();

    const auto report = dsp::declip(span.view());
    if (!report) {
        status_->setText(tr("Could not restore clipping: %1")
                             .arg(QString::fromStdString(std::string{report.error().what()})));
        return;
    }
    if (report.value().runs == 0) {
        status_->setText(report.value().tooLong > 0
                             ? tr("Nothing restored — the flat parts are too long to be peaks")
                             : tr("No clipping found"));
        return;
    }

    const bool changed = applyEdit(tr("restore clipped peaks"), [this, &range, &span] {
        return engine::replaceRange(document_, range.start, std::move(span)).ok();
    });
    if (changed) {
        QString note = tr("Restored %n clipped peak(s)", nullptr, report.value().runs);
        if (report.value().gainDb < 0.0) {
            // Said out loud, because a repair that quietly changes the level of
            // a master is not a repair the user can trust.
            note += tr(" — and brought the file down %1 dB so they fit")
                        .arg(-report.value().gainDb, 0, 'f', 2);
        }
        if (report.value().tooLong > 0) {
            note += tr(", leaving %n flat stretch(es) too long to be peaks", nullptr,
                       report.value().tooLong);
        }
        status_->setText(note);
    }
}

bool MainWindow::removeHum() {
    const TimeSelection range = targetRange();
    if (range.isEmpty() || !documentSource_) {
        return false;
    }

    AudioBuffer span{document_.layout(), range.length()};
    if (!documentSource_->read(range.start, span.view())) {
        status_->setText(tr("Could not read the selection"));
        return false;
    }

    status_->setText(tr("Looking for hum…"));
    status_->repaint();

    const auto report = dsp::dehum(span.view(), document_.sampleRate());
    if (!report) {
        status_->setText(tr("Could not remove hum: %1")
                             .arg(QString::fromStdString(std::string{report.error().what()})));
        return false;
    }
    if (!report.value().found) {
        // Not a failure: a recording with no hum in it is the common case, and
        // the useful thing to say is that it looked and there was none.
        status_->setText(tr("No mains hum found"));
        return true;
    }

    const bool changed = applyEdit(tr("remove hum"), [this, &range, &span] {
        return engine::replaceRange(document_, range.start, std::move(span)).ok();
    });
    if (changed) {
        status_->setText(tr("Removed %n partial(s) of %1 Hz hum, taking out %2 dB", nullptr,
                            report.value().harmonics)
                             .arg(report.value().frequency, 0, 'f', 2)
                             .arg(-report.value().removedDb, 0, 'f', 2));
    }
    return changed;
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
    // The measurement may not have started yet -- it is deferred behind a short
    // timer so that dragging a selection does not restart it on every mouse
    // move -- and on a long file it will still be running when it has. Either
    // way the answer is to make it happen and then wait: the alternative is a
    // menu item that silently does nothing depending on how fast the user
    // reached for it, which is exactly what deferring the measurement
    // reintroduced until a stress run caught it.
    const bool pending =
        (analysisTimer_ != nullptr && analysisTimer_->isActive()) || meters_->busy();
    if (pending) {
        status_->setText(tr("Waiting for the measurement to finish…"));
    }
    if (!waitForAnalysis()) {
        status_->setText(tr("The measurement did not finish in time"));
        return;
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

void MainWindow::applyFade(bool fadingIn, engine::FadeShape shape) {
    const TimeSelection range = targetRange();
    if (range.isEmpty()) {
        return;
    }
    // The label names the curve, so the undo menu distinguishes two fades of
    // different shapes over the same range rather than offering "fade in"
    // twice.
    const QString label = fadingIn ? tr("%1 fade in").arg(fadeShapeName(shape))
                                   : tr("%1 fade out").arg(fadeShapeName(shape));
    (void)applyEdit(label, [this, range, fadingIn, shape] {
        return engine::applyRangeFade(document_, range.start, range.end, fadingIn, shape).ok();
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
    if (name == "reverse") {
        return applyChannelOp(dsp::ChannelOp::Reverse, QStringLiteral("reverse"));
    }
    if (name == "invert") {
        return applyChannelOp(dsp::ChannelOp::InvertPolarity, QStringLiteral("invert polarity"));
    }
    if (name == "swapchannels") {
        return applyChannelOp(dsp::ChannelOp::SwapChannels, QStringLiteral("swap channels"));
    }
    if (name == "mono") {
        return applyChannelOp(dsp::ChannelOp::SumToMono, QStringLiteral("sum to mono"));
    }
    if (name == "mark") {
        return addMarker(QString{});
    }
    // fadein:scurve names the curve for this one fade without disturbing what
    // the menu has selected, so a batch script says what it means rather than
    // depending on hidden state.
    for (const char* prefix : {"fadein:", "fadeout:"}) {
        const QString start = QLatin1String{prefix};
        if (!name.startsWith(start)) {
            continue;
        }
        const auto shape = fadeShapeFor(name.mid(start.size()));
        if (!shape) {
            return false;
        }
        applyFade(start == QLatin1String{"fadein:"}, *shape);
        return true;
    }

    if (name.startsWith("mark:")) {
        return addMarker(name.mid(5));
    }
    if (name == "nextmarker") {
        goToMarker(true);
        return true;
    }
    if (name == "prevmarker") {
        goToMarker(false);
        return true;
    }
    if (name == "deletemarker") {
        return deleteNearestMarker();
    }
    if (name == "clearmarkers") {
        return clearMarkers();
    }
    if (name == "dehum") {
        return removeHum();
    }
    if (name == "declip") {
        restoreClipping();
        return true;
    }
    if (name == "declick") {
        return applyDeclick(5.0, QStringLiteral("remove clicks"));
    }
    if (name.startsWith("declick:")) {
        bool ok = false;
        const double threshold = name.mid(8).toDouble(&ok);
        if (!ok) {
            return false;
        }
        return applyDeclick(threshold, QStringLiteral("remove clicks"));
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
    // compress:threshold/ratio[/attack_ms/release_ms] and
    // gate:threshold[/depth_dB]. Positional rather than named, because a batch
    // verb is a line in a script rather than a form, and the first two are the
    // ones that decide what the processor does.
    //
    // Slashes rather than commas: the verb list itself is comma-separated, so
    // a comma inside a verb is a verb boundary and compress:-30,8 arrives as
    // two operations, the second of them nonsense.
    // deess:frequency[/threshold/ratio], with slashes for the same reason the
    // dynamics verbs use them.
    if (name.startsWith("deess:")) {
        const QStringList parts = name.mid(6).split(QLatin1Char{'/'}, Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            return false;
        }
        std::vector<double> numbers;
        for (const QString& part : parts) {
            bool ok = false;
            numbers.push_back(part.toDouble(&ok));
            if (!ok) {
                return false;
            }
        }
        dsp::DeessSettings settings;
        settings.frequencyHz = numbers[0];
        if (numbers.size() > 1) {
            settings.thresholdDb = numbers[1];
        }
        if (numbers.size() > 2) {
            settings.ratio = numbers[2];
        }
        const SampleRate rate = document_.sampleRate();
        return applyOverRange(
            QStringLiteral("de-ess"), settings.attackSeconds, settings.releaseSeconds,
            [rate, settings](AudioBufferView audio, SampleCount runUp, SampleCount blend) {
                (void)blend;
                auto made = dsp::deess(audio, rate, settings, runUp);
                return made ? Status{} : Status{made.error()};
            });
    }

    if (name.startsWith("compress:") || name.startsWith("gate:")) {
        const bool compressing = name.startsWith("compress:");
        const QStringList parts =
            name.mid(compressing ? 9 : 5).split(QLatin1Char{'/'}, Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            return false;
        }
        std::vector<double> numbers;
        for (const QString& part : parts) {
            bool ok = false;
            numbers.push_back(part.toDouble(&ok));
            if (!ok) {
                return false;
            }
        }

        const SampleRate rate = document_.sampleRate();
        if (compressing) {
            dsp::CompressorSettings settings;
            settings.thresholdDb = numbers[0];
            if (numbers.size() > 1) {
                settings.ratio = numbers[1];
            }
            if (numbers.size() > 2) {
                settings.attackSeconds = numbers[2] / 1000.0;
            }
            if (numbers.size() > 3) {
                settings.releaseSeconds = numbers[3] / 1000.0;
            }
            return applyOverRange(
                QStringLiteral("compress"), settings.attackSeconds, settings.releaseSeconds,
                [rate, settings](AudioBufferView audio, SampleCount runUp, SampleCount blend) {
                    return dsp::compressOffline(audio, rate, settings, runUp, blend);
                });
        }

        dsp::GateSettings settings;
        settings.thresholdDb = numbers[0];
        if (numbers.size() > 1) {
            settings.rangeDb = numbers[1];
        }
        return applyOverRange(
            QStringLiteral("gate"), settings.attackSeconds, settings.releaseSeconds,
            [rate, settings](AudioBufferView audio, SampleCount runUp, SampleCount blend) {
                return dsp::gateOffline(audio, rate, settings, runUp, blend);
            });
    }

    // Export settings, so a batch run can ask for a 16-bit delivery master
    // with the dither it wants rather than only ever getting the default.
    if (name.startsWith("format:")) {
        const QString wanted = name.mid(7);
        if (wanted == QLatin1String{"16"}) {
            preferences_.exportFormat = io::SampleFormat::PcmInt16;
        } else if (wanted == QLatin1String{"24"}) {
            preferences_.exportFormat = io::SampleFormat::PcmInt24;
        } else if (wanted == QLatin1String{"float"}) {
            preferences_.exportFormat = io::SampleFormat::Float32;
        } else {
            return false;
        }
        return true;
    }
    if (name.startsWith("dither:")) {
        const QString wanted = name.mid(7);
        if (wanted == QLatin1String{"none"}) {
            preferences_.dither = dsp::DitherType::None;
        } else if (wanted == QLatin1String{"tpdf"}) {
            preferences_.dither = dsp::DitherType::Tpdf;
        } else if (wanted == QLatin1String{"shaped"}) {
            preferences_.dither = dsp::DitherType::TpdfNoiseShaped;
        } else {
            return false;
        }
        return true;
    }

    // The analysis overlays, driven without a menu. Each one sets the flag the
    // menu sets and then re-runs, so a script and a user reach the same state
    // by the same path.
    if (name == "beatgrid" || name == "nobeatgrid" || name == "pitchcontour" ||
        name == "nopitchcontour" || name == "bands" || name == "nobands" || name == "room" ||
        name == "noroom") {
        const bool wanted = !name.startsWith(QLatin1String{"no"});
        const QString which = wanted ? name : name.mid(2);
        if (which == QLatin1String{"beatgrid"}) {
            preferences_.showBeatGrid = wanted;
        } else if (which == QLatin1String{"pitchcontour"}) {
            preferences_.showPitchContour = wanted;
        } else if (which == QLatin1String{"bands"}) {
            preferences_.showOctaveBands = wanted;
        } else {
            preferences_.measureRoom = wanted;
        }
        refreshActions();
        reanalyseNow();
        // The analysis runs on a worker, so a verb after this one would
        // otherwise act on the previous answer.
        return waitForAnalysis();
    }
    if (name == "analysisprint") {
        return waitForAnalysis() && printMusicalAnalysis();
    }

    // The EQ curve, driven without a mouse. eqadd and eqdrag speak in hertz
    // and decibels and are turned into pixels inside the view, so a script
    // stays readable while the gesture still goes through the hit-testing and
    // the axis mapping a pointer would -- which is where the mistakes are.
    if (name == "eq") {
        setEqCurveVisible(true);
        return true;
    }
    if (name == "eqhide") {
        setEqCurveVisible(false);
        return true;
    }
    if (name == "eqreset") {
        resetEqCurve();
        return true;
    }
    if (name == "eqapply") {
        return applyEqCurve();
    }
    if (name == "eqprint") {
        return printEqBands();
    }
    if (name.startsWith("eqadd:") || name.startsWith("eqdrag:") || name.startsWith("eqq:") ||
        name.startsWith("eqshiftdrag:") || name.startsWith("eqremove:")) {
        const qsizetype colon = name.indexOf(QLatin1Char{':'});
        const QStringList parts = name.mid(colon + 1).split(QLatin1Char{'/'}, Qt::SkipEmptyParts);
        std::vector<double> numbers;
        for (const QString& part : parts) {
            bool ok = false;
            numbers.push_back(part.toDouble(&ok));
            if (!ok) {
                return false;
            }
        }
        if (name.startsWith("eqadd:")) {
            return numbers.size() == 2 && spectrum_->addBandAt(numbers[0], numbers[1]);
        }
        if (name.startsWith("eqdrag:")) {
            return numbers.size() == 3 &&
                   spectrum_->dragBandTo(static_cast<int>(numbers[0]), numbers[1], numbers[2]);
        }
        if (name.startsWith("eqq:")) {
            return numbers.size() == 2 &&
                   spectrum_->turnWheelOverBand(static_cast<int>(numbers[0]), numbers[1]);
        }
        if (name.startsWith("eqshiftdrag:")) {
            return numbers.size() == 2 && spectrum_->shiftDragBandBy(static_cast<int>(numbers[0]),
                                                                     static_cast<int>(numbers[1]));
        }
        return numbers.size() == 1 && spectrum_->clickAwayBand(static_cast<int>(numbers[0]));
    }

    if (name == "reference") {
        captureSpectrumReference();
        return spectrum_ != nullptr && spectrum_->hasReference();
    }
    if (name == "clearreference") {
        clearSpectrumReference();
        return true;
    }
    if (name == "normalise") {
        normaliseToTarget();
    } else if (name == "fadein") {
        applyFade(true, preferences_.fadeShape);
    } else if (name == "fadeout") {
        applyFade(false, preferences_.fadeShape);
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

    io::WavOptions options;
    options.format = preferences_.exportFormat;
    auto writer =
        io::WavWriter::create(stream, document_.sampleRate(), document_.layout(), options);
    if (!writer) {
        status_->setText(tr("Could not start the export: %1")
                             .arg(QString::fromStdString(std::string{writer.error().what()})));
        return false;
    }

    // Sixteen bits only, and that is a deliberate limit rather than an
    // oversight. The setting is a standing policy, applied to every export
    // without being asked for again, so what it costs when it does nothing
    // matters: dithering a 24-bit export means opening a 24-bit file and
    // exporting it gives back a different file, which breaks the bit-identical
    // round trip this project checks for in several places and relies on in
    // several more. The noise it would remove sits 144 dB down. That is not a
    // close trade.
    //
    // A float export drops nothing at all, so adding noise to one would be
    // vandalism whatever the setting says.
    //
    // sa-cli's --dither is different on purpose: there it is typed out per
    // invocation rather than standing, so an explicit request at 24 bits is
    // honoured.
    //
    // One ditherer for the whole export, not one per block, because a shaper
    // carries its error across the join and restarting it at every block would
    // put a discontinuity in the noise floor every 65536 samples.
    std::optional<dsp::Ditherer> ditherer;
    const int exportBits = preferences_.exportFormat == io::SampleFormat::PcmInt16 ? 16 : 0;
    if (exportBits > 0 && preferences_.dither != dsp::DitherType::None) {
        dsp::DitherSettings settings;
        settings.type = preferences_.dither;
        settings.bits = exportBits;
        if (auto made = dsp::Ditherer::create(settings, document_.layout().count())) {
            ditherer.emplace(std::move(made).value());
        }
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
        AudioBufferView filled = view.subRange(0, read.value());
        if (ditherer) {
            ditherer->process(filled);
        }
        if (!writer.value().write(filled)) {
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
    // Only when there is one. A mono file printing stereo_valid=0 and four
    // zeroes invites a test to compare against the zeroes.
    std::printf("stereo_valid=%d\n", result->stereo.valid ? 1 : 0);
    if (result->stereo.valid) {
        line("stereo_correlation", result->stereo.correlation);
        line("stereo_width_db", result->stereo.widthDb);
        line("stereo_balance_db", result->stereo.balanceDb);
        line("stereo_mono_loss_db", result->stereo.monoLossDb);
    }
    std::printf("gated_blocks=%lld\n", static_cast<long long>(result->loudness.gatedBlockCount));
    std::printf("frames=%lld\n", static_cast<long long>(result->statistics.frames));

    // Markers too, because the alternative for a test is reading them off a
    // picture of a ruler.
    std::printf("markers=%lld\n", static_cast<long long>(document_.markers().size()));
    for (std::size_t i = 0; i < document_.markers().size(); ++i) {
        const engine::Marker& marker = document_.markers()[i];
        std::printf("marker_%zu=%lld,%lld,%s\n", i, static_cast<long long>(marker.position),
                    static_cast<long long>(marker.length), marker.label.c_str());
    }
    std::fflush(stdout);
    return true;
}

bool MainWindow::printMusicalAnalysis() const {
    const MusicalAnalysis* result = analysis_ == nullptr ? nullptr : analysis_->latest();
    if (result == nullptr) {
        return false;
    }
    const auto line = [](const char* key, double value) { std::printf("%s=%.6f\n", key, value); };
    const auto text = [](const char* key, const std::string& value) {
        std::printf("%s=%s\n", key, value.c_str());
    };

    std::printf("analysed_frames=%lld\nrequested_frames=%lld\n",
                static_cast<long long>(result->analysedFrames),
                static_cast<long long>(result->requestedFrames));
    text("coverage_note", coverageNote(*result));

    // What the labels actually hold, row by row, before anything recomputed.
    // A driver that only ever compared the window against a second copy of the
    // window's own reasoning would agree with it however wrong both were.
    for (const AnalysisPanel::PanelRow& row : analysis_->shownRows()) {
        std::printf("shown_%s=%s\n", qPrintable(row.name), qPrintable(row.text));
        std::printf("visible_%s=%d\n", qPrintable(row.name), row.visible ? 1 : 0);
    }

    // The reading before the raw fields, because the reading is the claim the
    // window is making and the fields are only what it made it from.
    const Reading key = keyReading(result->key, result->keyError);
    text("key_shown", key.value);
    text("key_caveat", key.caveat);
    std::printf("key_certainty=%d\n", static_cast<int>(key.certainty));
    line("key_strength", result->key.strength);
    line("key_fit", result->key.fit);
    line("key_contrast", result->key.contrast);
    line("key_tuning_cents", result->key.tuningOffsetCents);
    text("key_runner_up", runnerUpReading(result->key));

    const Reading tempo = tempoReading(result->tempo, result->tempoError);
    text("tempo_shown", tempo.value);
    text("tempo_caveat", tempo.caveat);
    std::printf("tempo_certainty=%d\ntempo_valid=%d\n", static_cast<int>(tempo.certainty),
                result->tempo.valid ? 1 : 0);
    line("tempo_bpm", result->tempo.bpm);
    line("tempo_confidence", result->tempo.confidence);
    line("tempo_first_beat", result->tempo.firstBeatSeconds);
    std::printf("tempo_beats=%lld\n", static_cast<long long>(result->tempo.beatSeconds.size()));

    const Reading pitch = pitchReading(result->pitch, result->pitchRequested, result->pitchError);
    text("pitch_shown", pitch.value);
    text("pitch_caveat", pitch.caveat);
    std::printf("pitch_certainty=%d\npitch_requested=%d\npitch_frames=%lld\n",
                static_cast<int>(pitch.certainty), result->pitchRequested ? 1 : 0,
                static_cast<long long>(result->pitch.size()));
    std::printf("pitch_hop=%lld\n", static_cast<long long>(result->pitchHop));

    const Reading room = roomReading(result->room, result->roomRequested, result->roomError);
    text("room_shown", room.value);
    text("room_caveat", room.caveat);
    std::printf("room_certainty=%d\nroom_valid=%d\n", static_cast<int>(room.certainty),
                result->room.valid ? 1 : 0);
    // The flags beside the text, so a driver can hold the two against each
    // other: a figure whose flag is false has to print a dash and never a
    // number, and that rule is what the room section is for.
    std::printf("room_has_edt=%d\nroom_has_t20=%d\nroom_has_t30=%d\n",
                result->room.hasEarlyDecay ? 1 : 0, result->room.hasT20 ? 1 : 0,
                result->room.hasT30 ? 1 : 0);
    // Through roomSeconds, so that what a test reads is exactly what the panel
    // shows -- "--" included.
    text("room_edt", roomSeconds(result->room.valid && result->room.hasEarlyDecay,
                                 result->room.earlyDecaySeconds));
    text("room_t20",
         roomSeconds(result->room.valid && result->room.hasT20, result->room.t20Seconds));
    text("room_t30",
         roomSeconds(result->room.valid && result->room.hasT30, result->room.t30Seconds));
    line("room_usable_range_db", result->room.usableRangeDb);

    std::printf("bands=%lld\n", static_cast<long long>(result->bands.size()));
    for (std::size_t i = 0; i < result->bands.size(); ++i) {
        const analysis::Band& band = result->bands[i];
        std::printf("band_%zu=%.1f,%.4f\n", i, band.centreHz, band.levelDb);
    }
    std::printf("bands_drawn=%d\nbeat_grid_drawn=%d\ncontour_drawn=%d\n",
                preferences_.showOctaveBands ? 1 : 0, preferences_.showBeatGrid ? 1 : 0,
                preferences_.showPitchContour ? 1 : 0);
    std::fflush(stdout);
    return true;
}

bool MainWindow::waitForAnalysis(int timeoutMs) {
    // The analysis is deferred behind a timer so that dragging a selection does
    // not restart it on every mouse move. A batch run has no drag: force
    // whatever is pending rather than racing the timer and capturing the
    // previous selection's numbers.
    if (analysisTimer_ && analysisTimer_->isActive()) {
        reanalyseNow();
    }

    QElapsedTimer clock;
    clock.start();
    while (meters_->busy() || spectrogramBusy_ || (analysis_ != nullptr && analysis_->busy())) {
        if (clock.elapsed() > timeoutMs) {
            const char* waitingFor = spectrogramBusy_  ? "the spectrogram"
                                     : meters_->busy() ? "the meters"
                                                       : "the musical analysis";
            std::fprintf(stderr, "sound-analyser: gave up waiting for %s after %d ms\n", waitingFor,
                         timeoutMs);
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

bool MainWindow::saveSpectrumImage(const std::filesystem::path& path) {
    if (analysisTimer_ && analysisTimer_->isActive()) {
        reanalyseNow();
    }
    QApplication::processEvents();
    const QPixmap shot = spectrum_->grab(
        QRect{kGutterWidth, 0, spectrum_->width() - kGutterWidth, spectrum_->height()});
    QApplication::processEvents();
    return shot.save(QString::fromStdString(path.string()), "PNG");
}

bool MainWindow::saveWaveformImage(const std::filesystem::path& path) {
    QApplication::processEvents();
    const QPixmap shot = waveform_->grab(
        QRect{kGutterWidth, 0, waveform_->width() - kGutterWidth, waveform_->height()});
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
