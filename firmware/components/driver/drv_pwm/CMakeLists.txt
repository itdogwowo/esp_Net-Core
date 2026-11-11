idf_component_register(
    SRCS "nc_pwm_drv.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_ledc
)
