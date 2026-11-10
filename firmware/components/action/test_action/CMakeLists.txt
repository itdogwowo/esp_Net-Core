# 測試指令：只喺 NC_ENABLE_TESTS=ON 時編譯。
set(_req nc4 mem nvs_flash esp_driver_uart esp_driver_gpio freertos esp_timer heap esp_mm)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND _req esp_wifi esp_netif esp_event esp_lcd)
endif()
idf_component_register(
    SRCS "nc_test_action.c"
    INCLUDE_DIRS "include"
    REQUIRES ${_req}
)
