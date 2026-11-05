# 測試指令：只喺 NC_ENABLE_TESTS=ON 時編譯。
idf_component_register(
    SRCS "nc_test_action.c"
    INCLUDE_DIRS "include"
    REQUIRES nc4 mem nvs_flash esp_driver_uart freertos esp_timer
)
