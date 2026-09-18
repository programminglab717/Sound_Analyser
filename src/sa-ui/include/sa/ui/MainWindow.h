#pragma once

#include <sa/engine/Document.h>
#include <sa/engine/DocumentSource.h>
#include <sa/engine/UndoHistory.h>
#include <sa/ui/Colourmap.h>
#include <sa/ui/LoudnessPanel.h>
#include <sa/ui/SpectrogramView.h>
#include <sa/ui/TimeRuler.h>
#include <sa/ui/WaveformView.h>

#include <QMainWindow>
#include <filesystem>
#include <memory>
#include <optional>

class QAction;
class QLabel;

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

    /// Load an audio file, build its caches and display it. Returns false and
    /// reports on the status bar if the file cannot be opened.
    bool openFile(const std::filesystem::path& path);

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
    /// Spin the event loop until no measurement is outstanding, or `timeoutMs`
    /// passes. Returns false on timeout.
    [[nodiscard]] bool waitForAnalysis(int timeoutMs = 120000);

    /// Print the completed measurement as key=value lines on stdout, so a test
    /// can check the numbers rather than the pixels showing them.
    [[nodiscard]] bool printAnalysis() const;

    [[nodiscard]] bool saveScreenshot(const std::filesystem::path& path);

    /// Render just the spectrogram's plotting area, with no gutter and no
    /// window chrome. A test that checks pixels needs to know what it is
    /// looking at; grabbing the whole window makes it guess where the plot
    /// starts, and that guess is what breaks the next time the layout moves.
    [[nodiscard]] bool saveSpectrogramImage(const std::filesystem::path& path);

private slots:
    void chooseFile();
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

    /// Rebuild the peak and spectrogram caches from the *document*, not the
    /// file. After the first edit those are different things, and showing the
    /// file is showing the user something they did not ask for.
    void rebuildCaches();
    void refreshViews();
    void refreshActions();
    void updateStatus();

    /// Run an edit, commit it to the history under `label`, and bring the views
    /// back in step. An edit that fails leaves the document and the history
    /// untouched, which is why the verbs return a Status rather than throwing.
    template <typename Edit>
    [[nodiscard]] bool applyEdit(const QString& label, Edit&& edit);

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
    QAction* exportSelectionAction_ = nullptr;

    engine::Document document_;
    std::optional<engine::UndoHistory> history_;
    std::shared_ptr<const engine::DocumentSource> documentSource_;
    std::shared_ptr<const io::PeakPyramid> peaks_;
    std::shared_ptr<const spectral::SpectrogramPyramid> spectra_;

    AudioBuffer clipboard_;
    std::filesystem::path openedPath_;
    QString spectrogramNote_;
};

} // namespace sa::ui
