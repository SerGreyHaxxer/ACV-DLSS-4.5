# ============================================================================
# DLSS 4 Proxy DLL - CMake Build Configuration
# ============================================================================
# Build commands:
#   mkdir build && cd build
#   cmake .. -G "Visual Studio 17 2022" -A x64
#   cmake --build . --config Release
# ============================================================================

cmake_minimum_required(VERSION 3.16)
project(DLSS4Proxy VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output name must be dxgi.dll for proxy to work
set(CMAKE_SHARED_LIBRARY_PREFIX "")

# Source files - Streamline integration path
set(SOURCES
    main.cpp
    src/proxy.cpp
    src/hooks.cpp
    src/ngx_wrapper.cpp
    src/streamline_integration.cpp
    src/d3d12_wrappers.cpp
    src/dxgi_wrappers.cpp
    src/resource_detector.cpp
    src/crash_handler.cpp
)

set(HEADERS
    src/dlss4_config.h
    src/proxy.h
    src/hooks.h
    src/ngx_wrapper.h
    src/streamline_integration.h
    src/d3d12_wrappers.h
    src/dxgi_wrappers.h
    src/resource_detector.h
    src/logger.h
    src/crash_handler.h
)

# Create the DLL
add_library(dxgi SHARED ${SOURCES} ${HEADERS} dxgi.def)

# Link required libraries
target_link_libraries(dxgi PRIVATE
    d3d12
    dxgi
    dxguid
    user32
    dbghelp
)

# Set output directory
set_target_properties(dxgi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Compiler definitions
target_compile_definitions(dxgi PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Include path for Streamline SDK headers (local or external)
target_include_directories(dxgi PRIVATE
    "${CMAKE_SOURCE_DIR}"
)

# Enable warnings
if(MSVC)
    target_compile_options(dxgi PRIVATE /W4 /WX-)
endif()

message(STATUS "DLSS 4 Proxy DLL configured for build")
message(STATUS "Output: dxgi.dll")
