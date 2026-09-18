#pragma once

#include <sa/engine/Document.h>
#include <sa/ui/Colourmap.h>
#include <sa/ui/SpectrogramView.h>
#include <sa/ui/TimeRuler.h>
#include <sa/ui/WaveformView.h>

#include <QMainWindow>
#include <filesystem>
#include <memory>

class QLabel;

namespace sa::ui {

/// The application window: a shared time ruler over a waveform over a
/// spectrogram, all on one time axis.
///
/// The views are locked together deliberately. The product's whole claim is
/// that analysis and editing are the same surface, and that falls apart the
/// moment the user has to reconcile two different scroll positions in their
/// head.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();

    /// Load an audio file, build its caches and display it. Returns false and
    /// reports on the status bar if the file cannot be opened.
    bool openFile(const std::filesystem::path& path);

    /// Render the window to a PNG without needing a display. This is how the UI
    /// is checked in an environment with no screen, and it doubles as a CI smoke
    /// test that the whole load-analyse-draw path really runs.
    [[nodiscard]] bool saveScreenshot(const std::filesystem::path& path);

    /// Render just the spectrogram's plotting area, with no gutter and no
    /// window chrome. A test that checks pixels needs to know what it is
    /// looking at; grabbing the whole window makes it guess where the plot
    /// starts, and that guess is what breaks the next time the layout moves.
    [[nodiscard]] bool saveSpectrogramImage(const std::filesystem::path& path);

private slots:
    void chooseFile();
    void zoomToFit();

private:
    void buildMenus();
    void setColourmap(Colourmap map);
    void setFrequencyScale(FrequencyScale scale);
    void showWaveformCursor(double seconds, double peakDecibels);
    void showSpectrogramCursor(double seconds, double hz, double decibels);

    TimeRuler* ruler_ = nullptr;
    WaveformView* waveform_ = nullptr;
    SpectrogramView* spectrogram_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* readout_ = nullptr;

    engine::Document document_;
    std::shared_ptr<const io::PeakPyramid> peaks_;
    std::shared_ptr<const spectral::SpectrogramPyramid> spectra_;
};

} // namespace sa::ui
