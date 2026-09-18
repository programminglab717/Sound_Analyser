add_library(sa-transport STATIC
    src/Player.cpp
)
add_library(sa::transport ALIAS sa-transport)

target_include_directories(sa-transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-transport PUBLIC sa-core sa-device sa-io)
target_link_libraries(sa-transport PRIVATE sa-warnings)

if(SA_BUILD_TESTS)
    add_executable(sa-transport-tests tests/PlayerTests.cpp)
    target_link_libraries(sa-transport-tests PRIVATE sa-transport sa-warnings
                                                     Catch2::Catch2WithMain)
    catch_discover_tests(sa-transport-tests)
endif()
