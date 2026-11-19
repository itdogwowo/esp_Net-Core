# pixel（sw 層）：環、編碼器、內建效果生產者。
#
# **純軟件**：不知道 ESP 硬體、不知道任務、不知道驅動。
# 生產者寫 4 B/px（R,G,B,W）進環；編碼器把 canonical 轉成各燈種的物理格式。
# 這一層的 .c 同時被 firmware 與 host 測試編譯（tools/hosttest/pixel_test）。
idf_component_register(
    SRCS "nc_pixel_q.c" "nc_pixel_encode.c" "nc_pixel_producer.c" "nc_pixel_registry.c"
         "nc_pixel_bench.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer log heap
)
