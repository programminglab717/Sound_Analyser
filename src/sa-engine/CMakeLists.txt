add_library(sa-engine STATIC
    src/Clip.cpp
    src/Document.cpp
    src/DocumentSource.cpp
    src/Edits.cpp
    src/SessionFile.cpp
    src/Timeline.cpp
    src/UndoHistory.cpp
)
add_library(sa::engine ALIAS sa-engine)

target_include_directories(sa-engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-engine PUBLIC sa-core sa-io)
target_link_libraries(sa-engine PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(sa-engine PRIVATE sa-warnings)

if(SA_BUILD_TESTS)
    add_executable(sa-engine-tests
        tests/DocumentSourceTests.cpp
        tests/DocumentTests.cpp
        tests/EditTests.cpp
        tests/SessionTests.cpp
        tests/UndoTests.cpp
    )
    target_link_libraries(sa-engine-tests PRIVATE sa-engine sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-engine-tests)
endif()
