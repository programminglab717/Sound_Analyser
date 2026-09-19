add_library(sa-spectral STATIC
    src/FrameAnalysis.cpp
    src/Denoise.cpp
    src/SpectralEdit.cpp
    src/SpectrogramPyramid.cpp
    src/SpectrogramTileStore.cpp
    src/SpectrogramTiles.cpp
)
add_library(sa::spectral ALIAS sa-spectral)

target_include_directories(sa-spectral PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-spectral PUBLIC sa-core sa-dsp sa-io)
target_link_libraries(sa-spectral PRIVATE sa-warnings)

if(SA_BUILD_TESTS)
    add_executable(sa-spectral-tests
        tests/DenoiseTests.cpp
        tests/SpectralEditTests.cpp
        tests/SpectrogramPyramidTests.cpp
        tests/SpectrogramTileStoreTests.cpp
        tests/SpectrogramTilesTests.cpp
    )
    target_link_libraries(sa-spectral-tests PRIVATE sa-spectral sa-warnings Catch2::Catch2WithMain)
    # The tile store's hashing lives in src/ rather than include/, because
    # nothing outside this library should be hashing anything with it. The tests
    # check it against published vectors, which is the only reason they reach in.
    target_include_directories(sa-spectral-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    catch_discover_tests(sa-spectral-tests)
endif()
