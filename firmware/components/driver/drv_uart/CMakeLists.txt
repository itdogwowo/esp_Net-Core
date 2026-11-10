# UART 匯流排驅動器（對照舊專案 slave/driver/uart_drv.py）
idf_component_register(
    SRCS "nc_uart_drv.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart freertos log
)
