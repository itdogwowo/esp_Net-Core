# render_task（tasks 層）：公用輸出層 —— 推送／渲染（LED 現在、LCD 將來）。
#
# 2026-11-25：render 唔屬 pixel 系列（其他子系統都會用）。它擁有環／lane／後端／
# 節拍；pixel 的編碼由另一個 component（pixel_encode_task）經 nc_render_encode_* 呼叫，
# 中間只傳「格索引」隊列，唔 copy 像素。
# 硬體在 components/driver/drv_i80、drv_spi_led；環／編碼在 components/sw/pixel。
idf_component_register(
    SRCS "nc_render_task.c" "nc_pixel_backend_mock.c" "nc_pixel_backend_direct.c"
         "nc_i80_out.c" "nc_pixel_backend_spi.c" "nc_fps_clock.c"
    INCLUDE_DIRS "include"
    REQUIRES task mem hw core log config pixel state
    PRIV_REQUIRES freertos esp_timer esp_driver_gpio drv_i80 drv_spi_led
    WHOLE_ARCHIVE
)
