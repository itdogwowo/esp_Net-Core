# drv_i80：I80（LCD_CAM 8080 並口）＋ GDMA。
#
# 只管硬體：腳位、PCLK、DMA 槽、完成回呼。唔知道像素／燈／環／任務。
# 腳位 claim 與保留腳政策留在上層（task adapter），所以呢個 component 唔依賴 sys/hw。
idf_component_register(
    SRCS "nc_i80_drv.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_lcd esp_driver_gpio heap log
)
