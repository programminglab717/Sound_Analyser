add_library(sa-core STATIC
    src/AudioBuffer.cpp
    src/ChannelLayout.cpp
    src/RealtimeGuard.cpp
    src/Result.cpp
)
add_library(sa::core ALIAS sa-core)

target_include_directories(sa-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-core PRIVATE sa-warnings)

# The RT-safety instrumentation replaces global operator new/delete, so it has
# to be visible to everything that links sa-core -- hence PUBLIC.
if(SA_RT_SAFETY_CHECKS)
    target_compile_definitions(sa-core PUBLIC SA_RT_SAFETY_CHECKS=1)
endif()

if(SA_BUILD_TESTS)
    add_executable(sa-core-tests
        tests/AudioBufferTests.cpp
        tests/ChannelLayoutTests.cpp
        tests/RealtimeGuardTests.cpp
        tests/TypesTests.cpp
    )
    target_link_libraries(sa-core-tests PRIVATE sa-core sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-core-tests)
endif()
