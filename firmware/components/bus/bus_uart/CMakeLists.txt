# UART 線的 bus adapter（把 drv_uart 包成統一線路 API）
idf_component_register(
    SRCS "nc_bus_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES bus_core drv_uart esp_driver_uart
)
