add_library(sa-dsp STATIC
    src/Biquad.cpp
    src/BiquadCascade.cpp
    src/Dynamics.cpp
    src/EnvelopeFollower.cpp
    src/Fft.cpp
    src/ParametricEq.cpp
    src/Stft.cpp
    src/Window.cpp
)
add_library(sa::dsp ALIAS sa-dsp)

target_include_directories(sa-dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-dsp PUBLIC sa-core)
target_link_libraries(sa-dsp PRIVATE sa-warnings)

if(SA_BUILD_TESTS)
    add_executable(sa-dsp-tests
        tests/BiquadTests.cpp
        tests/DynamicsTests.cpp
        tests/FftTests.cpp
        tests/ParametricEqTests.cpp
        tests/StftTests.cpp
        tests/WindowTests.cpp
    )
    target_link_libraries(sa-dsp-tests PRIVATE sa-dsp sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-dsp-tests)
endif()
