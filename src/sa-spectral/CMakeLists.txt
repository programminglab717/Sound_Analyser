add_library(sa-spectral STATIC
    src/Denoise.cpp
    src/SpectralEdit.cpp
    src/SpectrogramPyramid.cpp
)
add_library(sa::spectral ALIAS sa-spectral)

target_include_directories(sa-spectral PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-spectral PUBLIC sa-core sa-dsp)
target_link_libraries(sa-spectral PRIVATE sa-warnings)

if(SA_BUILD_TESTS)
    add_executable(sa-spectral-tests
        tests/DenoiseTests.cpp
        tests/SpectralEditTests.cpp
        tests/SpectrogramPyramidTests.cpp
    )
    target_link_libraries(sa-spectral-tests PRIVATE sa-spectral sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-spectral-tests)
endif()
