#include <sa/ui/MainWindow.h>

#include <QApplication>
#include <QCommandLineParser>

/// Entry point.
///
/// `--screenshot <file>` loads, renders and saves without user interaction,
/// which is how the interface is checked in an environment with no display and
/// how CI proves the load-analyse-draw path still works end to end.
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Sound Analyser");
    QApplication::setOrganizationName("Sound Analyser");

    QCommandLineParser parser;
    parser.setApplicationDescription("Analysis-first audio repair and mastering");
    parser.addHelpOption();
    parser.addPositionalArgument("file", "Audio file to open");

    QCommandLineOption screenshot{"screenshot", "Render the window to <png> and exit.", "png"};
    parser.addOption(screenshot);
    QCommandLineOption plot{"screenshot-spectrogram",
                            "Render the spectrogram plot alone to <png> and exit.", "png"};
    parser.addOption(plot);
    parser.process(app);

    sa::ui::MainWindow window;
    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty()) {
        window.openFile(positional.first().toStdString());
    }

    if (parser.isSet(screenshot) || parser.isSet(plot)) {
        window.resize(1280, 760);
        window.show();
        bool saved = true;
        if (parser.isSet(screenshot)) {
            saved = window.saveScreenshot(parser.value(screenshot).toStdString());
        }
        if (saved && parser.isSet(plot)) {
            saved = window.saveSpectrogramImage(parser.value(plot).toStdString());
        }
        return saved ? 0 : 1;
    }

    window.show();
    return QApplication::exec();
}
