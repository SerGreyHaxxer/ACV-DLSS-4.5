cmake_minimum_required(VERSION 3.25)
project(TensorBoot
    VERSION 1.0.0
    DESCRIPTION "Safe Mode Bootstrapper for Tensor-Curie DLSS Proxy"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT for dependency isolation
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Source files
set(SRC_FILES
    main.cpp
    integrity_checker.cpp
)

# Create console executable
add_executable(${PROJECT_NAME} ${SRC_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    psapi
    crypt32
    advapi32
)

# Output name
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "TensorBoot"
)

# Compile options
target_compile_options(${PROJECT_NAME} PRIVATE
    /W4
    /permissive-
    /Zc:__cplusplus
)

# Definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    TENSORBOOT_VERSION="${PROJECT_VERSION}"
)

# Install
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)
