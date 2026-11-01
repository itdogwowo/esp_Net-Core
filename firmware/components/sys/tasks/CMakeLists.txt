idf_component_register(
    SRCS "nc_task.c" "nc_service.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos
    PRIV_REQUIRES mem esp_timer
)
