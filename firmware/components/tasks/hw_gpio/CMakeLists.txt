idf_component_register(
    SRCS "nc_hw_gpio.c"
    INCLUDE_DIRS "include"
    REQUIRES hw task config esp_driver_gpio freertos
)
