cmake_minimum_required(VERSION 3.25)
project(tensor-curie LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure Vcpkg integration
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# Find Dependencies
find_package(minhook CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

# Source files — explicit list (GLOB doesn't trigger reconfigure on add/remove)
set(SRC_FILES
    main.cpp
    src/camera_scanner.cpp
    src/config_manager.cpp
    src/crash_handler.cpp
    src/d3d12_wrappers.cpp
    src/descriptor_tracker.cpp
    src/dxgi_wrappers.cpp
    src/heuristic_scanner.cpp
    src/hooks.cpp
    src/imgui_overlay.cpp
    src/valhalla_gui.cpp
    src/input_handler.cpp
    src/pattern_scanner.cpp
    src/proxy.cpp
    src/resource_detector.cpp
    src/sampler_interceptor.cpp
    src/streamline_integration.cpp
)

# Includes — target-scoped to avoid polluting consumers
add_library(${PROJECT_NAME} SHARED ${SRC_FILES} dxgi.def)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    local_headers
    external
    external/nvapi/nvapi-main
    external/streamline/lib
)

# Library Directories for NVAPI and Streamline
target_link_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/external/nvapi/nvapi-main/amd64
    ${CMAKE_CURRENT_SOURCE_DIR}/external/streamline/lib
)

# Link Libraries — includes psapi and dbghelp (previously in #pragma comment)
target_link_libraries(${PROJECT_NAME} PRIVATE 
    minhook::minhook
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus
    d3d12
    d3d11
    d2d1
    dwrite
    dxgi
    nvapi64
    sl.interposer
    psapi
    dbghelp
    crypt32
)

# Output Name
set_target_properties(${PROJECT_NAME} PROPERTIES 
    OUTPUT_NAME "dxgi"
    PREFIX ""
)

# Compile Options — enable warnings
target_compile_options(${PROJECT_NAME} PRIVATE
    /W4
    /permissive-
    /Zc:__cplusplus
)

# C++ Modernization Definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE 
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
