# ---------------------------------------------------------------------------
# Vendored decoders
# ---------------------------------------------------------------------------
# dr_flac and dr_mp3 are header-only third-party C, compiled in exactly one
# translation unit (src/DrLibs.cpp) which is given a target of its own.
#
# That target deliberately does not link sa-warnings. Third-party C does not
# survive -Wall -Wextra -Wconversion -Wold-style-cast -Werror, and relaxing the
# warning set for the whole of sa-io to accommodate two vendored headers would
# throw the warnings away exactly where they are useful -- on the code we can
# actually fix. Quarantining the vendored implementation in its own target keeps
# sa-io itself under the full warning set, FlacReader.cpp and Mp3Reader.cpp
# included.
add_library(sa-io-drlibs STATIC src/DrLibs.cpp)

# Relative to this directory rather than to the top of the project: sa-io knows
# where its own vendored code is, and nothing about that should depend on which
# project happens to be including it.
target_include_directories(sa-io-drlibs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/dr_libs)

# PUBLIC because every translation unit including these headers must agree:
# DR_MP3_FLOAT_OUTPUT changes the layout of the drmp3 struct, so a mismatch
# between this target and sa-io would be a silent ODR violation rather than a
# link error.
#
#   NO_STDIO      -- the readers pull bytes through a ByteSource, so the stdio
#                    convenience wrappers are dead code, and on MSVC they drag
#                    in deprecated-fopen warnings with them.
#   NO_OGG        -- the sniffer only ever routes native `fLaC` streams to
#                    FlacReader, so the Ogg path is unreachable attack surface.
#   FLOAT_OUTPUT  -- the whole application is float32 internally; decoding to
#                    int16 first would throw away resolution for nothing.
target_compile_definitions(sa-io-drlibs PUBLIC
    DR_FLAC_NO_STDIO
    DR_FLAC_NO_OGG
    DR_MP3_NO_STDIO
    DR_MP3_FLOAT_OUTPUT
)

if(MSVC)
    target_compile_options(sa-io-drlibs PRIVATE /W0)
else()
    target_compile_options(sa-io-drlibs PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# sa-io
# ---------------------------------------------------------------------------
add_library(sa-io STATIC
    src/AiffReader.cpp
    src/AiffWriter.cpp
    src/AudioFile.cpp
    src/AudioFileInfo.cpp
    src/ByteSource.cpp
    src/FlacReader.cpp
    src/FlacWriter.cpp
    src/Mp3Reader.cpp
    src/PeakPyramid.cpp
    src/WavReader.cpp
    src/WavWriter.cpp
)
add_library(sa::io ALIAS sa-io)

target_include_directories(sa-io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-io PUBLIC sa-core)
# PRIVATE: dr_flac.h and dr_mp3.h are an implementation detail of this module
# and must not leak into anything that links sa-io.
target_link_libraries(sa-io PRIVATE sa-io-drlibs sa-warnings)

if(SA_BUILD_TESTS)
    add_executable(sa-io-tests
        tests/AiffTests.cpp
        tests/AiffWriterTests.cpp
        tests/AudioFileTests.cpp
        tests/FlacTests.cpp
        tests/FlacWriterTests.cpp
        tests/Mp3Tests.cpp
        tests/PeakPyramidTests.cpp
        tests/StreamingPyramidTests.cpp
        tests/WavRobustnessTests.cpp
        tests/WavTests.cpp
    )
    target_link_libraries(sa-io-tests PRIVATE sa-io sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-io-tests)
endif()
