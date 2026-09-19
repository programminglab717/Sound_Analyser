#include <QApplication>
#include <QByteArray>
#include <catch2/catch_session.hpp>

/// Catch2's entry point, with a QApplication round it.
///
/// The rest of the interface is tested by calling windowless classes, which is
/// the right way round and is why sa-ui-tests exists. What is left over is the
/// part that only a widget does: a panel that cancels a worker in the middle of
/// a measurement, and a view whose claim is about the pen it drew a line with.
/// Neither can be reached without an application object, so this binary has one
/// and the other does not.
///
/// Offscreen unless the caller has said otherwise, so the tests run wherever
/// ctest does. It is set here rather than in CMake because a test binary that
/// only works when its environment was arranged for it is a test binary that
/// silently does not run.
int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArray{"offscreen"});
    }
    QApplication application{argc, argv};
    return Catch::Session().run(argc, argv);
}
