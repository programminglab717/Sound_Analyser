cmake_minimum_required(VERSION 3.24)

project(SoundAnalyser
    VERSION 0.1.0
    DESCRIPTION "Analysis-first audio repair and mastering workstation"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SA_BUILD_TESTS       "Build the test suite"                      ON)
option(SA_RT_SAFETY_CHECKS  "Trap allocations on the audio thread"      ON)
option(SA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"        ON)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Warning interface — every sa-* target links this
# ---------------------------------------------------------------------------
add_library(sa-warnings INTERFACE)

if(MSVC)
    target_compile_options(sa-warnings INTERFACE /W4 /permissive- /Zc:__cplusplus)
    # MSVC's Debug default is _ITERATOR_DEBUG_LEVEL=2, which makes standard
    # containers allocate for iterator bookkeeping. That breaks the "allocates
    # nothing on the audio thread" assertions this codebase is built around:
    # they pass under MSVC Release and fail under MSVC Debug by 8 and 20
    # allocations respectively, in code that allocates nothing under GCC.
    #
    # Allocation-freedom is a core invariant here, so being able to assert it in
    # the configuration developers actually debug in is worth more than iterator
    # validity checking -- which the Linux ASan/UBSan jobs cover anyway. Every
    # target in this tree is built with the same setting, so there is no ODR
    # mismatch across the libraries or Catch2.
    add_compile_definitions($<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>)
    if(SA_WARNINGS_AS_ERRORS)
        target_compile_options(sa-warnings INTERFACE /WX)
    endif()
    # LGPL compliance (ADR 0006): dynamic CRT, never static.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    target_compile_options(sa-warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
    if(SA_WARNINGS_AS_ERRORS)
        target_compile_options(sa-warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SA_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
endif()

# ---------------------------------------------------------------------------
# Modules — dependencies point downward only (see docs/03-architecture.md §2)
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Third-party
# ---------------------------------------------------------------------------
# Session files are untrusted input -- a user opens a session someone sent them
# -- so the parser is a well-tested library rather than something hand-rolled.
include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_subdirectory(src/sa-core)
add_subdirectory(src/sa-analysis)
add_subdirectory(src/sa-device)
add_subdirectory(src/sa-dsp)
add_subdirectory(src/sa-engine)
add_subdirectory(src/sa-io)
add_subdirectory(src/sa-spectral)
add_subdirectory(src/sa-ui)
add_subdirectory(benchmarks)

message(STATUS "")
message(STATUS "Sound Analyser ${PROJECT_VERSION}")
message(STATUS "  Build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests .............. ${SA_BUILD_TESTS}")
message(STATUS "  RT safety checks ... ${SA_RT_SAFETY_CHECKS}")
message(STATUS "")
