#include <sa/ui/MainWindow.h>

#include <QApplication>
#include <QCommandLineParser>
#include <cstdio>
#include <filesystem>
#include <optional>

namespace {

/// Parse "1.5-3.0" into a pair of seconds. Returns false on anything else.
bool parseSpan(const QString& text, double& from, double& to) {
    const qsizetype dash = text.indexOf('-', 1);
    if (dash <= 0) {
        return false;
    }
    bool okFrom = false;
    bool okTo = false;
    from = text.left(dash).toDouble(&okFrom);
    to = text.mid(dash + 1).toDouble(&okTo);
    return okFrom && okTo;
}

} // namespace

/// Entry point.
///
/// The batch options exist so the interface can be driven and checked in an
/// environment with no display. They are not a substitute for `sa-cli`, which
/// drives the engine with no Qt at all; they check that *this window* wires the
/// engine up correctly, which is a different claim.
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Auscultate");
    QApplication::setOrganizationName("Delta Creation Co.");

    QCommandLineParser parser;
    parser.setApplicationDescription("Analysis-first audio repair and mastering");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "Audio file to open");

    QCommandLineOption screenshot{"screenshot", "Render the window to <png> and exit.", "png"};
    QCommandLineOption plot{"screenshot-spectrogram",
                            "Render the spectrogram plot alone to <png> and exit.", "png"};
    QCommandLineOption curve{"screenshot-spectrum",
                             "Render the spectrum panel alone to <png> and exit.", "png"};
    QCommandLineOption wave{"screenshot-waveform",
                            "Render the waveform plot alone to <png> and exit.", "png"};
    QCommandLineOption select{"select",
                              "Select <from>-<to> in seconds before --apply runs. For anything "
                              "with more than one step, put select: inside --apply instead: Qt "
                              "keeps only the last value of a repeated option, so alternating "
                              "--select and --apply silently drops all but the final pair.",
                              "span"};
    QCommandLineOption apply{
        "apply",
        "Comma-separated operations, applied in order: select:<from>-<to> (seconds), gain:<dB>, "
        "normalise, fadein, fadeout, flatten, cut, copy, paste, delete, silence, trim, undo, "
        "redo, selectall, deselect, beatgrid, nobeatgrid, pitchcontour, nopitchcontour, bands, "
        "nobands, room, noroom, analysisprint.",
        "ops"};
    QCommandLineOption exportTo{"export", "Write the edited document to <wav>.", "wav"};
    QCommandLineOption printAnalysis{"print-analysis",
                                     "Print the measured loudness and peaks on stdout."};
    QCommandLineOption printMusical{"print-musical",
                                    "Print the key, tempo, contour and room figures on stdout, "
                                    "with the text the panel decided to show for each."};
    QCommandLineOption saveSession{"save-session", "Save the arrangement to <file>.", "file"};
    QCommandLineOption play{"play",
                            "Play the selection to its end and report where the transport got "
                            "to. Runs in real time."};
    // A settings file of its own, so that a run can be given one rather than
    // standing on whatever the person running it has saved. Without this,
    // checking that settings persist would mean writing into the real
    // per-user file -- and every other headless check would inherit whatever
    // the last one left there.
    QCommandLineOption settingsFile{"settings", "Read and write settings in <ini>.", "ini"};
    QCommandLineOption printSettings{"print-settings",
                                     "Print the settings in force on stdout, as restored."};
    for (const QCommandLineOption& option :
         {screenshot, plot, curve, wave, select, apply, exportTo, printAnalysis, printMusical, play,
          saveSession, settingsFile, printSettings}) {
        parser.addOption(option);
    }
    parser.process(app);

    // A run that is here to do a job rather than to be looked at.
    const bool working = parser.isSet(screenshot) || parser.isSet(plot) || parser.isSet(curve) ||
                         parser.isSet(wave) || parser.isSet(exportTo) || parser.isSet(apply) ||
                         parser.isSet(printAnalysis) || parser.isSet(printMusical) ||
                         parser.isSet(play) || parser.isSet(saveSession);
    const bool batch = working || parser.isSet(printSettings);

    // A named file, or -- for a run that is doing a job and named none -- no
    // file at all. A script is not a person: it should not open at somebody's
    // remembered window position, and the files it opens are not files that
    // person chose to open. Without the second rule, every check in tools/
    // would depend on whatever the check before it happened to leave behind.
    //
    // --print-settings is the exception, because the question it asks is
    // "which settings are in force", and answering that from a file nobody
    // uses would answer a different question.
    const auto where = [&]() -> std::optional<std::filesystem::path> {
        if (parser.isSet(settingsFile)) {
            return std::filesystem::path{parser.value(settingsFile).toStdString()};
        }
        if (working) {
            return std::filesystem::path{};
        }
        return std::nullopt;
    };

    sa::ui::MainWindow window{where()};
    const QStringList positional = parser.positionalArguments();
    const auto openPositional = [&window](const QString& path) {
        return window.openPath(path.toStdString());
    };
    if (!positional.isEmpty() && !openPositional(positional.first())) {
        std::fprintf(stderr, "could not open %s\n", qPrintable(positional.first()));
        return 1;
    }

    if (!batch) {
        window.show();
        return QApplication::exec();
    }

    // A fixed size, unless a stored geometry was restored. The screenshots the
    // batch options exist for are compared pixel by pixel, so a run with
    // nothing saved has to be the same size every time; a run that did restore
    // something must not then be resized out from under the thing being
    // checked.
    if (!window.restoredWindow()) {
        window.resize(1280, 760);
    }
    window.show();

    if (parser.isSet(select)) {
        double from = 0.0;
        double to = 0.0;
        if (!parseSpan(parser.value(select), from, to)) {
            std::fprintf(stderr, "could not parse --select %s\n", qPrintable(parser.value(select)));
            return 2;
        }
        window.selectSeconds(from, to);
    }

    if (parser.isSet(apply)) {
        const QStringList operations = parser.value(apply).split(',', Qt::SkipEmptyParts);
        for (const QString& operation : operations) {
            if (!window.applyOperation(operation.trimmed())) {
                std::fprintf(stderr, "unknown operation %s\n", qPrintable(operation));
                return 2;
            }
        }
    }

    // Let the meters finish before anything is captured or written. Their
    // measurement runs on a worker, so a batch run would otherwise screenshot a
    // panel of dashes.
    if (!window.waitForAnalysis()) {
        std::fprintf(stderr, "analysis did not finish in time\n");
        return 1;
    }

    if (parser.isSet(play) && !window.playToEnd()) {
        std::fprintf(stderr, "playback did not complete\n");
        return 1;
    }
    if (parser.isSet(printAnalysis) && !window.printAnalysis()) {
        std::fprintf(stderr, "no measurement to print\n");
        return 1;
    }
    if (parser.isSet(printMusical) && !window.printMusicalAnalysis()) {
        std::fprintf(stderr, "no musical analysis to print\n");
        return 1;
    }
    if (parser.isSet(printSettings) && !window.printSettings()) {
        std::fprintf(stderr, "no settings to print\n");
        return 1;
    }
    if (parser.isSet(saveSession) && !window.saveSession(parser.value(saveSession).toStdString())) {
        std::fprintf(stderr, "could not save the session\n");
        return 1;
    }
    if (parser.isSet(exportTo) && !window.exportTo(parser.value(exportTo).toStdString(), false)) {
        std::fprintf(stderr, "export failed\n");
        return 1;
    }
    if (parser.isSet(screenshot) &&
        !window.saveScreenshot(parser.value(screenshot).toStdString())) {
        std::fprintf(stderr, "screenshot failed\n");
        return 1;
    }
    if (parser.isSet(plot) && !window.saveSpectrogramImage(parser.value(plot).toStdString())) {
        std::fprintf(stderr, "spectrogram screenshot failed\n");
        return 1;
    }
    if (parser.isSet(curve) && !window.saveSpectrumImage(parser.value(curve).toStdString())) {
        std::fprintf(stderr, "spectrum screenshot failed\n");
        return 1;
    }
    if (parser.isSet(wave) && !window.saveWaveformImage(parser.value(wave).toStdString())) {
        std::fprintf(stderr, "waveform screenshot failed\n");
        return 1;
    }
    return 0;
}
