cmake_minimum_required(VERSION 3.25)

find_package(Catch2 3 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

# Test executable — includes source files needed by integration tests
add_executable(acv-dlss-tests
    test_main.cpp
    test_inplace_vector.cpp
    test_hive.cpp
    test_reflection.cpp
    test_pattern_scanner.cpp
    test_config_serialization.cpp
    test_sentinel.cpp
    test_ghost_hook.cpp
    # Source implementations needed by tests
    ${CMAKE_SOURCE_DIR}/src/pattern_scanner.cpp
    ${CMAKE_SOURCE_DIR}/src/sentinel_crash_handler.cpp
    ${CMAKE_SOURCE_DIR}/src/ghost_hook.cpp
)

target_include_directories(acv-dlss-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/local_headers
    ${CMAKE_SOURCE_DIR}/external
)

target_link_libraries(acv-dlss-tests PRIVATE
    Catch2::Catch2WithMain
    spdlog::spdlog
    tomlplusplus::tomlplusplus
    dbghelp
    psapi
)

target_compile_options(acv-dlss-tests PRIVATE
    /W4
    /permissive-
    /Zc:__cplusplus
)

target_compile_definitions(acv-dlss-tests PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    ACV_DLSS_TESTING=1
)

# Use dynamic CRT for tests (matches vcpkg x64-windows triplet default)
set_target_properties(acv-dlss-tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

# Register tests with CTest
include(CTest)
include(Catch)
catch_discover_tests(acv-dlss-tests)
