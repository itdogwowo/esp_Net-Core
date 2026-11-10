idf_component_register(
    SRCS "nc_mem.c" "nc_plan.c" "nc_hub.c" "nc_buf.c"
    INCLUDE_DIRS "include"
    REQUIRES heap espressif__cjson freertos esp_timer
)
