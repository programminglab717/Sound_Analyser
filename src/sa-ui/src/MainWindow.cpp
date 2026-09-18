#include <sa/io/AudioFile.h>
#include <sa/ui/MainWindow.h>

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

MainWindow::MainWindow() {
    setWindowTitle(tr("Sound Analyser"));
    resize(1280, 760);

    auto* splitter = new QSplitter{Qt::Vertical, this};
    waveform_ = new WaveformView{splitter};
    spectrogram_ = new SpectrogramView{splitter};
    splitter->addWidget(waveform_);
    splitter->addWidget(spectrogram_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    setCentralWidget(splitter);

    // One time axis. Either view can drive it; both follow.
    connect(waveform_, &WaveformView::viewRangeChanged, spectrogram_,
            &SpectrogramView::setViewRange);
    connect(spectrogram_, &SpectrogramView::viewRangeChanged, waveform_,
            &WaveformView::setViewRange);

    status_ = new QLabel{tr("Open an audio file to begin"), this};
    statusBar()->addWidget(status_);

    buildMenus();

    setStyleSheet("QMainWindow { background: #101015; }"
                  "QMenuBar { background: #16171d; color: #c8ccd8; }"
                  "QMenuBar::item:selected { background: #2a2c38; }"
                  "QMenu { background: #16171d; color: #c8ccd8; }"
                  "QMenu::item:selected { background: #2a2c38; }"
                  "QStatusBar { background: #16171d; color: #8a8fa0; }"
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

void MainWindow::zoomToFit() {
    waveform_->showAll();
}

void MainWindow::chooseFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open audio"), {}, tr("Audio files (*.wav *.aif *.aiff *.aifc);;All files (*)"));
    if (!path.isEmpty()) {
        openFile(path.toStdString());
    }
}

bool MainWindow::openFile(const std::filesystem::path& path) {
    auto opened = io::openAudioFile(path);
    if (!opened) {
        status_->setText(tr("Could not open %1: %2")
                             .arg(QString::fromStdString(path.filename().string()),
                                  QString::fromStdString(std::string{opened.error().what()})));
        return false;
    }

    const auto audio = opened.value();
    const io::AudioFileInfo& info = audio->info();

    document_ = engine::Document{info.sampleRate, info.layout};
    auto source = document_.addSource(audio, path.filename().string(), path);
    if (!source || !document_.appendSource(source.value(), 0)) {
        status_->setText(tr("Could not place %1 on the timeline")
                             .arg(QString::fromStdString(path.filename().string())));
        return false;
    }

    // Both caches stream from the file rather than loading it, so opening a long
    // recording costs the same memory as a short one.
    auto peaks = io::PeakPyramid::buildStreaming(*audio);
    if (!peaks) {
        status_->setText(tr("Could not analyse %1")
                             .arg(QString::fromStdString(path.filename().string())));
        return false;
    }
    peaks_ = std::make_shared<const io::PeakPyramid>(std::move(peaks).value());

    AudioBuffer whole{info.layout, info.frameCount};
    if (auto read = audio->read(0, whole.view()); read) {
        auto spectra = spectral::SpectrogramPyramid::build(whole.constView(), 0);
        if (spectra) {
            spectra_ =
                std::make_shared<const spectral::SpectrogramPyramid>(std::move(spectra).value());
        }
    }

    waveform_->setPyramid(peaks_, info.sampleRate);
    spectrogram_->setPyramid(spectra_, info.sampleRate);
    spectrogram_->setViewRange(waveform_->viewStart(), waveform_->viewLength());

    const double seconds = info.durationSeconds();
    status_->setText(tr("%1  ·  %2  ·  %3 Hz  ·  %4 ch  ·  %5:%6")
                         .arg(QString::fromStdString(path.filename().string()),
                              QString::fromStdString(std::string{io::toString(info.format)}))
                         .arg(static_cast<int>(info.sampleRate.hz()))
                         .arg(info.channelCount())
                         .arg(static_cast<int>(seconds) / 60)
                         .arg(static_cast<int>(seconds) % 60, 2, 10, QChar{'0'}));
    setWindowTitle(tr("%1 — Sound Analyser")
                       .arg(QString::fromStdString(path.filename().string())));
    return true;
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
