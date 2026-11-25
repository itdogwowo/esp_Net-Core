# render_task（tasks 層）：渲染任務（環 → 編碼 → 後端 → 硬件）。
#
# 將來 LCD 同 LED 都由這一個 render 任務處理；這裡刻意不放效果。
# 後端（i80／spi／direct／mock）是任務與 driver 之間的轉接；
# 硬體在 components/driver/drv_i80、drv_spi_led；環／編碼在 components/sw/pixel。
idf_component_register(
    SRCS "nc_render_task.c" "nc_pixel_backend_mock.c" "nc_pixel_backend_direct.c"
         "nc_i80_out.c" "nc_pixel_backend_spi.c" "nc_fps_clock.c"
    INCLUDE_DIRS "include"
    REQUIRES task mem hw core log config pixel
    PRIV_REQUIRES freertos esp_timer esp_driver_gpio drv_i80 drv_spi_led
    WHOLE_ARCHIVE
)
