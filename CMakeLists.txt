cmake_minimum_required(VERSION 3.25)
project(acv-dlss
    VERSION 1.0.0
    DESCRIPTION "DLSS 4.5 DXGI Proxy for Assassin's Creed Valhalla"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Phase 0: Dependency Isolation - Static CRT Linking
# ============================================================================
# This eliminates vcruntime140.dll dependencies for maximum compatibility
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ============================================================================
# Options
# ============================================================================
option(ACV_DLSS_WERROR "Treat warnings as errors (used by CI)" OFF)

# ============================================================================
# Static Analysis & Sanitizers
# ============================================================================

# Address Sanitizer for debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Debug build: Enabling Address Sanitizer")
    add_compile_options(/fsanitize=address)
    add_link_options(/fsanitize=address)
endif()

# Clang-tidy integration (if available, skip in CI to avoid noise)
if(NOT ACV_DLSS_WERROR)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy.exe)
    if(CLANG_TIDY_EXE)
        message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXE}")
        set(CMAKE_CXX_CLANG_TIDY
            "${CLANG_TIDY_EXE};-checks=modernize-*,performance-*,readability-*,bugprone-*,cppcoreguidelines-*,-modernize-use-trailing-return-type")
    else()
        message(STATUS "clang-tidy not found, skipping static analysis")
    endif()
endif()

# Configure Vcpkg integration (fallback for users not using presets)
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

# Find Dependencies
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)

# Source files — explicit list (GLOB doesn't trigger reconfigure on add/remove)
set(SRC_FILES
    main.cpp
    # Phase 0: Stability & Safety
    src/sentinel_crash_handler.cpp
    src/ghost_hook.cpp
    # Phase 2: Render Pipeline
    src/render_passes/ray_tracing_pass.cpp
    # Original sources
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
    # Phase 3.5: Resource State Tracking
    src/resource_state_tracker.cpp
    # Phase 4: Jitter Extraction Engine
    src/jitter_engine.cpp
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

# Warnings-as-errors for CI builds
if(ACV_DLSS_WERROR)
    target_compile_options(${PROJECT_NAME} PRIVATE /WX)
    message(STATUS "Warnings-as-errors enabled (CI mode)")
endif()

# C++ Modernization Definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    ACV_DLSS_VERSION="${PROJECT_VERSION}"
)

# ============================================================================
# Phase 0: Dependency Hardening
# ============================================================================
# Strip debug info from Release builds for smaller DLL and security
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/DEBUG:NONE>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

# ============================================================================
# Install target — for packaging and deployment scripts
# ============================================================================
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
)

# ============================================================================
# Phase 0: Safe Mode Bootstrapper (TensorBoot.exe)
# ============================================================================
add_subdirectory(tools/TensorBoot)

# ============================================================================
# Phase 1: Testing Infrastructure (Catch2)
# ============================================================================
option(ACV_DLSS_BUILD_TESTS "Build the unit test suite" OFF)
if(ACV_DLSS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
