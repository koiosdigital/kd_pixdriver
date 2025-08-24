idf_component_register(
    SRCS "src/kd_pixdriver.cpp"
         "src/pixel_effects.cpp"
         "src/i2s_pixel_protocol.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES "nvs_flash" "esp_system"
    REQUIRES "esp_driver_i2s" "esp_driver_gpio"
)
