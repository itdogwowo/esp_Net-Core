# pixel：像素渲染器（renderer）＋內建效果生產者
#
# 生產者寫 4 B/px（R,G,B,W）進環 → 渲染器按自己的格式讀 → 後端驅動送出去。
# 效果（nc_pixel_producer.c，任務名 "rgbmatrix" 的舊內容）只是眾多生產者之一：
# `.nce` 外掛與主機推送寫的是**同一條環、同一個格式**，所以換生產者不動渲染器。
#
# esp_lcd / esp_driver_spi / heap 是 PRIV_REQUIRES：
#   只有 i80／spi 兩個後端的 .c 需要它們，而且受 CONFIG_NETCORE_PIXEL_I80／SPI
#   守衛（沒開時那兩個檔只編 stub），所以不會把 LCD/SPI 驅動拉進沒有燈的映像。
idf_component_register(
    SRCS "nc_pixel_q.c" "nc_pixel_encode.c" "nc_pixel_task.c" "nc_pixel_producer.c"
         "nc_pixel_backend_mock.c" "nc_pixel_backend_direct.c"
         "nc_pixel_backend_i80.c" "nc_pixel_backend_spi.c"
    INCLUDE_DIRS "include"
    REQUIRES task mem hw core log
    PRIV_REQUIRES freertos esp_timer esp_lcd esp_driver_spi esp_driver_gpio heap
)
