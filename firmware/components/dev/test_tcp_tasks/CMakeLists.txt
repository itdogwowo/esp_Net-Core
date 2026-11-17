# TCP 吞吐量測端點（測試任務）；只喺 NC_ENABLE_TESTS=ON 時編譯。
idf_component_register(
    SRCS "nc_test_tcp.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_timer heap freertos
)
