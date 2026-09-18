# Owned by the device-layer work. Add sources and tests here.
file(GLOB SA_DEVICE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB SA_DEVICE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)

add_library(sa-device STATIC ${SA_DEVICE_SOURCES})
add_library(sa::device ALIAS sa-device)

target_include_directories(sa-device PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-device PUBLIC sa-core)
target_link_libraries(sa-device PRIVATE sa-warnings)

if(SA_BUILD_TESTS AND SA_DEVICE_TESTS)
    add_executable(sa-device-tests ${SA_DEVICE_TESTS})
    target_link_libraries(sa-device-tests PRIVATE sa-device sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-device-tests)
endif()
