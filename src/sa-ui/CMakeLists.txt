# The UI is optional: the engine, the analysis and the CLI all build and test
# without Qt, so a contributor without it can still work on everything below
# this layer.
find_package(Qt6 COMPONENTS Widgets QUIET)

if(NOT Qt6_FOUND)
    message(STATUS "  sa-ui .............. SKIPPED (Qt6 not found)")
    return()
endif()

set(CMAKE_AUTOMOC ON)

# The Q_OBJECT headers have to be listed as sources: AUTOMOC only finds a header
# by itself when it sits next to its .cpp, and ours live under include/.
add_library(sa-ui STATIC
    src/AnalysisOverlay.cpp
    src/AnalysisPanel.cpp
    src/AnalysisReadout.cpp
    src/ChunkedPitch.cpp
    src/Colourmap.cpp
    src/EqCurve.cpp
    src/EqCurveView.cpp
    src/FieldDialog.cpp
    src/LoudnessPanel.cpp
    src/MainWindow.cpp
    src/SpectrogramView.cpp
    src/SpectrumView.cpp
    src/TimeAxisView.cpp
    src/TimeRuler.cpp
    src/ViewGeometry.cpp
    src/WaveformView.cpp
    include/sa/ui/AnalysisPanel.h
    include/sa/ui/EqCurveView.h
    include/sa/ui/FieldDialog.h
    include/sa/ui/LoudnessPanel.h
    include/sa/ui/MainWindow.h
    include/sa/ui/SpectrogramView.h
    include/sa/ui/SpectrumView.h
    include/sa/ui/TimeAxisView.h
    include/sa/ui/TimeRuler.h
    include/sa/ui/WaveformView.h
)
add_library(sa::ui ALIAS sa-ui)

target_include_directories(sa-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-ui PUBLIC sa-analysis sa-core sa-device sa-engine sa-io sa-spectral
                                  sa-transport Qt6::Widgets)
# Qt headers do not survive this project's warning set, so sa-warnings is not
# applied to the UI layer. Everything below it still builds with -Werror.
target_compile_features(sa-ui PUBLIC cxx_std_20)

add_executable(auscultate src/main.cpp)
target_link_libraries(auscultate PRIVATE sa-ui)

# The view geometry, the EQ curve and the chunked contour are deliberately
# windowless, so they are tested the same way every other module is: by calling
# them. sa-warnings is applied here although it is not applied to sa-ui, because
# nothing in this first binary includes a Qt header -- the whole point of the
# classes it covers is that they do not need one. The widget binary below is
# where anything that does need one lives, and it is kept separate so that this
# one stays honest.
if(SA_BUILD_TESTS)
    add_executable(sa-ui-tests
        tests/AnalysisOverlayTests.cpp
        tests/AnalysisReadoutTests.cpp
        tests/ChunkedPitchTests.cpp
        tests/EqCurveTests.cpp
        tests/TimePlotTests.cpp
    )
    target_link_libraries(sa-ui-tests PRIVATE sa-ui sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-ui-tests)

    # And a second binary for the handful of claims a windowless class cannot
    # be asked about: a panel that cancels a worker in the middle of a
    # measurement, and a view whose claim is about the pen it drew a line with.
    # It builds its own main so that there is a QApplication to make a widget
    # under, and it is not given sa-warnings for the reason the library above
    # is not: Qt's headers do not survive this project's warning set.
    #
    # Not in a sanitiser build, and the reason is the sanitiser rather than
    # anything here. The CI sanitiser jobs install no Qt at all, for the reason
    # the workflow gives: a distribution Qt is built without instrumentation,
    # so the sanitisers report its internals rather than this project's code.
    # This says the same thing to a developer who does have Qt installed, and
    # it matters most for ThreadSanitizer, because this is the binary that
    # would reach the part of Qt it is least able to see.
    #
    # A panel hands its result to the window through a queued connection, and
    # the ordering that makes that safe is a QMutex inside libQt6Core:
    # futex-based, in a library nothing instrumented, so the sanitiser cannot
    # see it and reports the value that crossed as a race. That is not a
    # judgement about this code -- a thirty-line program that hands one integer
    # across a queued connection and shares nothing else is reported the same
    # way, which is what tools/tsan_selftest.py demonstrates on any machine
    # with Qt on it. Whether it reports at all depends on the event dispatcher
    # in use, so it is intermittent as well as wrong: one of these tests came
    # back with twenty-one reports in four runs out of six and none in the
    # other two, while the pair of analysis-panel tests came back with
    # thirty-nine between them in six runs out of six -- and all thirty-nine
    # were that one hand-off, with nothing else of any kind reported.
    #
    # What that costs is real and is written down rather than glossed, here and
    # in the README: the panels' worker hand-off is the one piece of threading
    # in this project that ThreadSanitizer does not police, and closing it
    # needs a Qt built with the sanitiser.
    string(FIND "${CMAKE_CXX_FLAGS}" "-fsanitize=" SA_SANITISER_FLAG)
    if(SA_SANITISER_FLAG EQUAL -1)
        add_executable(sa-ui-widget-tests
            tests/widget/PanelHandoffTests.cpp
            tests/widget/WaveformOverlayTests.cpp
            tests/widget/WidgetTestMain.cpp
        )
        target_link_libraries(sa-ui-widget-tests PRIVATE sa-ui Catch2::Catch2)
        catch_discover_tests(sa-ui-widget-tests)
    else()
        message(STATUS "  sa-ui-widget-tests . SKIPPED (sanitiser build; see the comment "
                       "in src/sa-ui/CMakeLists.txt)")
    endif()
endif()
