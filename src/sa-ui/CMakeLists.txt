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
    src/Colourmap.cpp
    src/FieldDialog.cpp
    src/LoudnessPanel.cpp
    src/MainWindow.cpp
    src/SpectrogramView.cpp
    src/SpectrumView.cpp
    src/TimeAxisView.cpp
    src/TimeRuler.cpp
    src/ViewGeometry.cpp
    src/WaveformView.cpp
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

add_executable(sound-analyser src/main.cpp)
target_link_libraries(sound-analyser PRIVATE sa-ui)
