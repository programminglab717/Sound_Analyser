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
add_subdirectory(src/sa-core)
add_subdirectory(src/sa-analysis)
add_subdirectory(src/sa-device)
add_subdirectory(src/sa-dsp)
add_subdirectory(src/sa-engine)
add_subdirectory(src/sa-io)
add_subdirectory(src/sa-spectral)
add_subdirectory(benchmarks)

message(STATUS "")
message(STATUS "Sound Analyser ${PROJECT_VERSION}")
message(STATUS "  Build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests .............. ${SA_BUILD_TESTS}")
message(STATUS "  RT safety checks ... ${SA_RT_SAFETY_CHECKS}")
message(STATUS "")
