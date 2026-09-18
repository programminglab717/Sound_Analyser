add_library(sa-dsp STATIC
    src/Fft.cpp
    src/Stft.cpp
    src/Window.cpp
)
add_library(sa::dsp ALIAS sa-dsp)

target_include_directories(sa-dsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-dsp PUBLIC sa-core)
target_link_libraries(sa-dsp PRIVATE sa-warnings)

if(SA_BUILD_TESTS)
    add_executable(sa-dsp-tests
        tests/FftTests.cpp
        tests/StftTests.cpp
        tests/WindowTests.cpp
    )
    target_link_libraries(sa-dsp-tests PRIVATE sa-dsp sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-dsp-tests)
endif()
