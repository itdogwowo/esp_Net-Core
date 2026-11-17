# 8×8 WS2812 矩陣測試任務（I80 波形 + 分區時間抖動）。
# 只喺 NC_ENABLE_TESTS=ON 時由根 CMakeLists（components/tasks）加入編譯。
idf_component_register(
    SRCS "nc_rgb_matrix.c"
    INCLUDE_DIRS "include"
    REQUIRES task mem hw core esp_lcd esp_timer esp_rom esp_driver_gpio
)
