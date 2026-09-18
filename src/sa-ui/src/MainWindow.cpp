#include <sa/io/AudioFile.h>
#include <sa/ui/MainWindow.h>
#include <sa/ui/ViewGeometry.h>

#include <QActionGroup>
#include <QApplication>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>
#include <cmath>

namespace sa::ui {

namespace {

/// Building a spectrogram currently needs the decoded audio resident, so a file
/// long enough to exhaust memory is refused with an explanation rather than
/// taking the machine down. Lifting this means a streaming pyramid build --
/// tracked in docs/07-autonomous-queue.md.
constexpr std::size_t kMaximumDecodedBytes = 1'500'000'000;

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
    setCentralWidget(central);

    // One time axis. Either view can drive it; the other and the ruler follow.
    connect(waveform_, &WaveformView::viewRangeChanged, spectrogram_,
            &SpectrogramView::setViewRange);
    connect(spectrogram_, &SpectrogramView::viewRangeChanged, waveform_,
            &WaveformView::setViewRange);
    connect(waveform_, &WaveformView::viewRangeChanged, ruler_, &TimeRuler::setViewRange);
    connect(spectrogram_, &SpectrogramView::viewRangeChanged, ruler_, &TimeRuler::setViewRange);

    connect(waveform_, &WaveformView::cursorMoved, this, &MainWindow::showWaveformCursor);
    connect(spectrogram_, &SpectrogramView::cursorMoved, this, &MainWindow::showSpectrogramCursor);

    status_ = new QLabel{tr("Open an audio file to begin"), this};
    readout_ = new QLabel{this};
    readout_->setMinimumWidth(320);
    readout_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addWidget(status_, 1);
    statusBar()->addPermanentWidget(readout_);

    buildMenus();

    setStyleSheet("QMainWindow { background: #101015; }"
                  "QWidget { background: #101015; }"
                  "QMenuBar { background: #16171d; color: #c8ccd8; }"
                  "QMenuBar::item:selected { background: #2a2c38; }"
                  "QMenu { background: #16171d; color: #c8ccd8; }"
                  "QMenu::item:selected { background: #2a2c38; }"
                  "QStatusBar { background: #16171d; color: #8a8fa0; }"
                  "QStatusBar::item { border: none; }"
                  "QLabel { color: #8a8fa0; }"
                  "QSplitter::handle { background: #2a2c38; }");
}

void MainWindow::buildMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::chooseFile);
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, qApp, &QApplication::quit);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Zoom to &fit"), QKeySequence{Qt::Key_F}, this, &MainWindow::zoomToFit);
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

void MainWindow::zoomToFit() {
    waveform_->showAll();
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
        this, tr("Open audio"), {}, tr("Audio files (*.wav *.aif *.aiff *.aifc);;All files (*)"));
    if (!path.isEmpty()) {
        openFile(path.toStdString());
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

    // The peak pyramid streams: it reads the file in blocks and never holds more
    // than one block, so a long recording costs the same memory as a short one.
    auto peaks = io::PeakPyramid::buildStreaming(*audio);
    if (!peaks) {
        status_->setText(tr("Could not analyse %1").arg(name));
        return false;
    }
    peaks_ = std::make_shared<const io::PeakPyramid>(std::move(peaks).value());

    // The spectrogram does not stream yet, so it needs the decoded audio
    // resident. Refuse a file too long for that rather than being killed
    // halfway through allocating it.
    spectra_.reset();
    const std::size_t decodedBytes = static_cast<std::size_t>(info.frameCount) *
                                     static_cast<std::size_t>(info.channelCount()) * sizeof(float);
    if (decodedBytes > kMaximumDecodedBytes) {
        status_->setText(tr("%1 is too long for spectrogram analysis in this build "
                            "(%2 GB decoded); the waveform is shown.")
                             .arg(name)
                             .arg(static_cast<double>(decodedBytes) / 1e9, 0, 'f', 1));
    } else {
        AudioBuffer whole{info.layout, info.frameCount};
        if (auto read = audio->read(0, whole.view()); read) {
            // 4096 at 48 kHz is an 11.7 Hz bin and a 21 ms hop. A log axis
            // stretches the bottom two octaves over half the display, and 2048
            // gives them four bins to fill it with; this gives them eight, at a
            // time resolution transients still survive.
            spectral::SpectrogramConfig config;
            config.fftSize = 4096;
            config.hopSize = 1024;
            auto spectra = spectral::SpectrogramPyramid::build(whole.constView(), 0, config);
            if (spectra) {
                spectra_ = std::make_shared<const spectral::SpectrogramPyramid>(
                    std::move(spectra).value());
            }
        }
    }

    ruler_->setSampleRate(info.sampleRate);
    waveform_->setPyramid(peaks_, info.sampleRate);
    spectrogram_->setPyramid(spectra_, info.sampleRate);
    spectrogram_->setViewRange(waveform_->viewStart(), waveform_->viewLength());
    ruler_->setViewRange(waveform_->viewStart(), waveform_->viewLength());

    if (spectra_) {
        const double seconds = info.durationSeconds();
        status_->setText(
            tr("%1  ·  %2  ·  %3 Hz  ·  %4 ch  ·  %5")
                .arg(name, QString::fromStdString(std::string{io::toString(info.format)}))
                .arg(static_cast<int>(info.sampleRate.hz()))
                .arg(info.channelCount())
                .arg(QString::fromStdString(formatTime(seconds, 60.0))));
    }
    setWindowTitle(tr("%1 — Sound Analyser").arg(name));
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
