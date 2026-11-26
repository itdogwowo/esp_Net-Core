# drv_i80：I80（LCD_CAM 8080 並口）＋ GDMA。
#
# 只管硬體：腳位、PCLK、DMA 槽、完成回呼。唔知道像素／燈／環／任務。
# 腳位 claim 與保留腳政策留在上層（task adapter），所以呢個 component 唔依賴 sys/hw。
#
# 兩個實作二選一（同一個 nc_i80_drv.h 介面）：
#   · nc_i80_drv.c        — 經 esp_lcd 的 i80 panel IO（要 8/16 data＋WR＋DC 全部真腳）
#   · nc_i80_drv_direct.c — 直接寫 LCD_CAM（LL＋AHB GDMA），只 route 真正接燈的 lane
#                            （＝ FastLED 在 S3 的做法；未接燈的線／WR／DC 唔需要 GPIO）
if(CONFIG_NETCORE_PIXEL_I80_DIRECT)
    set(_i80_srcs "nc_i80_drv_direct.c")
else()
    set(_i80_srcs "nc_i80_drv.c")
endif()

idf_component_register(
    SRCS ${_i80_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_dma esp_hal_lcd esp_mm
                   esp_hw_support soc hal esp_rom heap log esp_timer
)
