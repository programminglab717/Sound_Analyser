# Owned by the device-layer work. Add sources and tests here.
file(GLOB SA_DEVICE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB SA_DEVICE_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)

# The platform backends are added back conditionally below. Taking them out of
# the glob is what keeps an unguarded Windows or ALSA source from compiling on a
# machine that has neither, which the glob would otherwise do silently.
list(REMOVE_ITEM SA_DEVICE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AlsaAudioDevice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WasapiAudioDevice.cpp)
list(REMOVE_ITEM SA_DEVICE_TESTS
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/AlsaAudioDeviceTests.cpp)

add_library(sa-device STATIC ${SA_DEVICE_SOURCES})
add_library(sa::device ALIAS sa-device)

target_include_directories(sa-device PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sa-device PUBLIC sa-core)
target_link_libraries(sa-device PRIVATE sa-warnings)

if(WIN32)
    target_sources(sa-device PRIVATE src/WasapiAudioDevice.cpp)
    # ole32 carries CoCreateInstance, CoTaskMemFree and PropVariantClear; avrt
    # carries AvSetMmThreadCharacteristics, which is how the render thread gets
    # the scheduling class an audio thread needs. Both ship with the Windows
    # SDK, so neither is a third-party dependency.
    target_link_libraries(sa-device PRIVATE ole32 avrt)
endif()

# ALSA is optional in exactly the way Qt is for sa-ui: without it the device
# layer still builds and every test but the ALSA ones still runs, so a
# contributor on a machine with no libasound headers is not blocked.
find_package(ALSA)

if(ALSA_FOUND)
    target_sources(sa-device PRIVATE src/AlsaAudioDevice.cpp)
    # PUBLIC because the tests have to know whether the backend is in the
    # library before they can decide to exercise it.
    target_compile_definitions(sa-device PUBLIC SA_DEVICE_HAVE_ALSA=1)
    target_link_libraries(sa-device PRIVATE ALSA::ALSA)
    list(APPEND SA_DEVICE_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/AlsaAudioDeviceTests.cpp)
else()
    message(STATUS "  sa-device .......... ALSA backend SKIPPED (libasound headers not found)")
endif()

if(SA_BUILD_TESTS AND SA_DEVICE_TESTS)
    add_executable(sa-device-tests ${SA_DEVICE_TESTS})
    target_link_libraries(sa-device-tests PRIVATE sa-device sa-warnings Catch2::Catch2WithMain)
    catch_discover_tests(sa-device-tests)
endif()
