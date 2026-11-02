idf_component_register(
    SRCS "nc_task.c" "nc_task_loop.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos mem esp_timer
)
