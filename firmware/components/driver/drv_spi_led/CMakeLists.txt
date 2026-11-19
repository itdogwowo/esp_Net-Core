# drv_spi_led：SPI 時脈式燈串（WS2801／APA102）。
#
# 只管 SPI 硬體與 DMA 槽；像素格式、顏色次序、環都在上層。
idf_component_register(
    SRCS "nc_spi_led_drv.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_spi heap log
)
