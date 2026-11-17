# 測試指令：只喺 NC_ENABLE_TESTS=ON 時編譯。
set(_req nc4 mem hw hw_gpio drv_pwm drv_uart nvs_flash esp_driver_uart esp_driver_gpio freertos esp_timer heap esp_mm)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND _req esp_lcd)
endif()
# CONFIG_NETCORE_WIFI=y 時 nc_test_action.c 會 include esp_wifi／esp_now／esp_event／
# esp_netif。呢三個依賴**無條件加**：component CMake 的第一輪 configure 睇唔到
# CONFIG_*（sdkconfig 未生成），用 if(CONFIG_...) 會在第一輪漏掉、之後被 cache 住。
# 代價只是多幾個 header 路徑；P4 接 esp_hosted 時 esp_wifi 元件本身就係 remote shim。
list(APPEND _req esp_wifi esp_netif esp_event)
# 0xF060 MATRIX 只喺 Kconfig NETCORE_RGB_MATRIX=y 時有內容；component 本身
# 一定存在（測試映像），所以直接加依賴，唔靠 CONFIG_*（第一輪 configure 睇唔到）。
list(APPEND _req test_rgb_led_matrix_tasks)
idf_component_register(
    SRCS "nc_test_action.c"
    INCLUDE_DIRS "include"
    REQUIRES ${_req}
)
