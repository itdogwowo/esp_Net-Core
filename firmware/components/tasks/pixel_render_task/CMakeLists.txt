# pixel_render_task（tasks 層）：輸出層 —— pixel_wave_task（畫波）＋ pixel_render_task（推送）。
#
# 第四層 pixel_render_task 擁有環／lane／後端／節拍；第三層 pixel_wave_task 只做
# 環 → 編碼 → 後端 DMA 槽，中間經「格索引」隊列交給 push，唔 copy 像素。
# 硬體在 components/driver/drv_i80、drv_spi_led；環／編碼在 components/sw/pixel。
idf_component_register(
    SRCS "nc_render_task.c" "nc_pixel_backend_mock.c" "nc_pixel_backend_direct.c"
         "nc_i80_out.c" "nc_pixel_backend_spi.c" "nc_fps_clock.c"
    INCLUDE_DIRS "include"
    REQUIRES task mem hw core log config pixel
    PRIV_REQUIRES freertos esp_timer esp_driver_gpio drv_i80 drv_spi_led
    WHOLE_ARCHIVE
)
