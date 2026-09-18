add_library(sa-io STATIC
    src/PeakPyramid.cpp
)
add_library(sa::io ALIAS sa-io)

target_include_directories(sa-io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-io PUBLIC sa-core)
target_link_libraries(sa-io PRIVATE sa-warnings)

if(SA_BUILD_TESTS)
    add_executable(sa-io-tests tests/PeakPyramidTests.cpp)
    target_link_libraries(sa-io-tests PRIVATE sa-io sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-io-tests)
endif()
