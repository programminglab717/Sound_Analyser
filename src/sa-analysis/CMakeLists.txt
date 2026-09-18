# Owned by the loudness/metering work. Add sources and tests here.
file(GLOB SA_ANALYSIS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB SA_ANALYSIS_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)

add_library(sa-analysis STATIC ${SA_ANALYSIS_SOURCES})
add_library(sa::analysis ALIAS sa-analysis)

target_include_directories(sa-analysis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-analysis PUBLIC sa-core)
target_link_libraries(sa-analysis PRIVATE sa-warnings)

if(SA_BUILD_TESTS AND SA_ANALYSIS_TESTS)
    add_executable(sa-analysis-tests ${SA_ANALYSIS_TESTS})
    target_link_libraries(sa-analysis-tests PRIVATE sa-analysis sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-analysis-tests)
endif()
