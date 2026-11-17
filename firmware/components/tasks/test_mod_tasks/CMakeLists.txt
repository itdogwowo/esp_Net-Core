idf_component_register(
    SRCS "nc_mod.c" "nc_ncd.c" "nc_res.c" "nc_meta.c"
    INCLUDE_DIRS "include"
    REQUIRES mem esp_rom freertos esp_timer task_manager config
)
