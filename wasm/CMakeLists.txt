cmake_minimum_required(VERSION 3.13)
project(pixel_preview_wasm)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Get git commit hash
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
    OUTPUT_VARIABLE GIT_COMMIT_SHORT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
    OUTPUT_VARIABLE GIT_COMMIT_FULL
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)

# Fallback if git is not available
if(NOT GIT_COMMIT_SHORT)
    set(GIT_COMMIT_SHORT "unknown")
    set(GIT_COMMIT_FULL "unknown")
endif()

# Generate version header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/pixel_version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/pixel_version.h
    @ONLY
)

# Source files
set(SOURCES
    pixel_preview.cpp
    pixel_platform_wasm.cpp
    bindings.cpp
)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# Create executable (WASM module)
add_executable(pixel_preview ${SOURCES})

# Emscripten link flags
set_target_properties(pixel_preview PROPERTIES
    SUFFIX ".js"
    LINK_FLAGS "\
        -s MODULARIZE=1 \
        -s EXPORT_NAME='createPixelPreview' \
        -s WASM=1 \
        -s ALLOW_MEMORY_GROWTH=1 \
        -s NO_EXIT_RUNTIME=1 \
        -s ENVIRONMENT='web,worker' \
        --bind \
        -O3 \
    "
)

# Debug build flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(pixel_preview PRIVATE -g -O0)
    set_target_properties(pixel_preview PROPERTIES
        LINK_FLAGS "\
            -s MODULARIZE=1 \
            -s EXPORT_NAME='createPixelPreview' \
            -s WASM=1 \
            -s ALLOW_MEMORY_GROWTH=1 \
            -s NO_EXIT_RUNTIME=1 \
            -s ENVIRONMENT='web,worker' \
            -s ASSERTIONS=1 \
            --bind \
            -g \
        "
    )
endif()
