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
    src/Colourmap.cpp
    src/EqCurve.cpp
    src/EqCurveView.cpp
    src/FieldDialog.cpp
    src/LoudnessPanel.cpp
    src/MainWindow.cpp
    src/Settings.cpp
    src/SettingsStore.cpp
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

# The view geometry and the EQ curve are deliberately windowless, so they are
# tested the same way every other module is: by calling them. sa-warnings is
# applied here although it is not applied to sa-ui, because nothing under
# tests/ includes a Qt header -- the whole point of the classes it covers is
# that they do not need one.
if(SA_BUILD_TESTS)
    add_executable(sa-ui-tests
        tests/AnalysisOverlayTests.cpp
        tests/AnalysisReadoutTests.cpp
        tests/EqCurveTests.cpp
        tests/SettingsTests.cpp
        tests/TimePlotTests.cpp
    )
    target_link_libraries(sa-ui-tests PRIVATE sa-ui sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-ui-tests)
endif()
