# Get git commit hash
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_SHORT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
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
    ${CMAKE_CURRENT_SOURCE_DIR}/include/pixel_version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/pixel_version.h
    @ONLY
)

idf_component_register(
    SRCS "src/kd_pixdriver.cpp"
         "src/pixel_effects.cpp"
         "src/i2s_pixel_protocol.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "nvs_flash" "esp_system"
    REQUIRES "esp_driver_i2s" "esp_driver_gpio"
)
