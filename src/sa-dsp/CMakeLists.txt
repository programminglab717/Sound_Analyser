add_library(sa-dsp STATIC
    src/Biquad.cpp
    src/BiquadCascade.cpp
    src/Dynamics.cpp
    src/EnvelopeFollower.cpp
    src/Declick.cpp
    src/Dither.cpp
    src/Declip.cpp
    src/Deess.cpp
    src/Dehum.cpp
    src/Fft.cpp
    src/FilterBank.cpp
    src/LinearPrediction.cpp
    src/MultibandCompressor.cpp
    src/ParametricEq.cpp
    src/ExactTruePeak.cpp
    src/OfflineDynamics.cpp
    src/OfflineLimiter.cpp
    src/Resampler.cpp
    src/Stft.cpp
    src/TimeStretch.cpp
    src/TruePeakDetector.cpp
    src/Window.cpp
    src/WindowedSinc.cpp
)
add_library(sa::dsp ALIAS sa-dsp)

target_include_directories(sa-dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-dsp PUBLIC sa-core)
target_link_libraries(sa-dsp PRIVATE sa-warnings)

if(SA_BUILD_TESTS)
    add_executable(sa-dsp-tests
        tests/BiquadTests.cpp
        tests/ChannelOpsTests.cpp
        tests/DynamicsTests.cpp
        tests/DeclickTests.cpp
        tests/DitherTests.cpp
        tests/DeclipTests.cpp
        tests/DeessTests.cpp
        tests/DehumTests.cpp
        tests/FftTests.cpp
        tests/FilterBankTests.cpp
        tests/LinearPredictionTests.cpp
        tests/MultibandCompressorTests.cpp
        tests/ParametricEqTests.cpp
        tests/ExactTruePeakTests.cpp
        tests/OfflineDynamicsTests.cpp
        tests/OfflineLimiterTests.cpp
        tests/ResamplerTests.cpp
        tests/StereoLinkTests.cpp
        tests/StftTests.cpp
        tests/TimeStretchTests.cpp
        tests/TruePeakTests.cpp
        tests/WindowTests.cpp
    )
    target_link_libraries(sa-dsp-tests PRIVATE sa-dsp sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-dsp-tests)
endif()
