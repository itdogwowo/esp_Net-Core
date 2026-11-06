# log 管道：兩個核的 ESP_LOG → 每核 ring → 單一 consumer → CDC 出口。
# 元件名用 nclog（唔可以用 log，會撞 IDF 的 log 元件）。
idf_component_register(
    SRCS "nc_log.c"
    INCLUDE_DIRS "include"
    REQUIRES log freertos esp_timer state
)
